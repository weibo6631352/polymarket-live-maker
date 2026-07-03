#!/usr/bin/env python3
"""Bucket probabilities for a cohort from bias-corrected open-meteo distributions.

Inputs : cache/cohort_<date>.json (fetch_cohort.py), cache/station_bias.json (bias_calib.py;
         stations without a record run UNCALIBRATED with bias 0 and are flagged — the grid
         bias is up to ~2 degC, so calibrate before trusting output), and open-meteo:
           --mode ensemble  (live ~122-member distribution; today/future dates)
           --mode previous  (archived deterministic previous_day{--lead} run; backtests)
Outputs: cache/model_<date>.json —
         {date, mode, cities: {city: {icao, fc_mean_c, bias_c, sd_c, calibrated,
                                      buckets: [{slug, lo, hi, p}], raw_prob_sum}}}

Probability semantics (must mirror how the market RESOLVES — see common.bucket_interval):
  round cities (whole-degree sources: all US degF + intl degC on Wunderground/NOAA):
        P(bucket [lo,hi]) = P(lo-0.5 <= T < hi+0.5) in the native unit
  floor cities (HKO 0.1 degC): P(lo <= T < hi+1)
Distributions are built in degC and bucket edges converted; ensemble uses a small normal
kernel per member (--kernel-sd, default 0.6 degC) so bucket edges are not step functions;
deterministic uses N(fc - bias, sd_c from calibration, default 1.2 / floor 0.9 degC).
Probabilities are renormalized over the 11 buckets (raw sum recorded as sanity).

CLI: python3 model.py 2026-07-03 --mode ensemble
     python3 model.py 2026-07-01 --mode previous --lead 1
"""

from __future__ import annotations

import argparse
import json
import statistics

from bias_calib import load_bias
from common import (CACHE, bucket_interval, f_to_c, interval_prob_normal, normalize,
                    station_meta)
from forecast import ensemble_daily_max, previous_run_daily_max

DEFAULT_SD_C = 1.2   # deterministic lead-1 width when uncalibrated
MIN_SD_C = 0.9       # never sharper than the documented ~0.95 degC residual sd
KERNEL_SD_C = 0.6    # ensemble per-member smoothing kernel


def _edges_c(bucket: dict, semantics: str) -> tuple[float | None, float | None]:
    """Bucket -> half-open interval on the continuous max, converted to degC."""
    a, b = bucket_interval(bucket["lo"], bucket["hi"], semantics)
    if bucket["unit"] == "F":
        a = None if a is None else f_to_c(a)
        b = None if b is None else f_to_c(b)
    return a, b


def bucket_probs_ensemble(member_max_c: list[float], bias_c: float, buckets: list[dict],
                          semantics: str, kernel_sd: float = KERNEL_SD_C) -> list[float]:
    probs = []
    for bk in buckets:
        a, b = _edges_c(bk, semantics)
        p = sum(interval_prob_normal(a, b, m - bias_c, kernel_sd) for m in member_max_c)
        probs.append(p / len(member_max_c))
    return probs


def bucket_probs_normal(fc_max_c: float, bias_c: float, sd_c: float, buckets: list[dict],
                        semantics: str) -> list[float]:
    mu = fc_max_c - bias_c
    return [interval_prob_normal(*_edges_c(bk, semantics), mu, sd_c) for bk in buckets]


def model_city(city_rec: dict, date_iso: str, mode: str, lead: int, bias_all: dict,
               kernel_sd: float = KERNEL_SD_C) -> dict | None:
    """Bucket probabilities for one cohort city; None if the forecast is unavailable."""
    res = city_rec["resolution"]
    meta = station_meta(res["icao"], res["kind"])
    rec = bias_all.get(res["icao"])
    bias_c = rec["bias_c"] if rec else 0.0
    sd_c = max(MIN_SD_C, rec["sd_c"]) if rec else DEFAULT_SD_C
    buckets = [m["bucket"] for m in city_rec["markets"]]

    if mode == "ensemble":
        members = ensemble_daily_max(meta["lat"], meta["lon"], meta["tz"], date_iso)
        if not members:
            return None
        probs = bucket_probs_ensemble(members, bias_c, buckets, res["semantics"], kernel_sd)
        fc_mean = statistics.mean(members)
    else:
        got = previous_run_daily_max(meta["lat"], meta["lon"], meta["tz"], date_iso, lead=lead)
        if date_iso not in got:
            return None
        fc_mean = got[date_iso]
        probs = bucket_probs_normal(fc_mean, bias_c, sd_c, buckets, res["semantics"])

    raw_sum = sum(probs)
    probs = normalize(probs)
    return {
        "icao": res["icao"],
        "fc_mean_c": round(fc_mean, 2),
        "bias_c": bias_c,
        "sd_c": sd_c,
        "calibrated": rec is not None,
        "raw_prob_sum": round(raw_sum, 4),
        "buckets": [{"slug": m["slug"], "lo": m["bucket"]["lo"], "hi": m["bucket"]["hi"],
                     "p": round(p, 5)}
                    for m, p in zip(city_rec["markets"], probs)],
    }


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("date", help="target date YYYY-MM-DD (cohort_<date>.json must exist)")
    ap.add_argument("--mode", choices=["ensemble", "previous"], default="ensemble")
    ap.add_argument("--lead", type=int, default=1, help="previous-run lead days")
    ap.add_argument("--kernel-sd", type=float, default=KERNEL_SD_C)
    args = ap.parse_args()

    cohort_path = CACHE / f"cohort_{args.date}.json"
    if not cohort_path.exists():
        raise SystemExit(f"missing {cohort_path} — run fetch_cohort.py {args.date} first")
    cohort = json.loads(cohort_path.read_text())
    bias_all = load_bias()

    out = {"date": args.date, "mode": args.mode, "lead": args.lead, "cities": {}}
    for city, rec in sorted(cohort["cities"].items()):
        m = model_city(rec, args.date, args.mode, args.lead, bias_all, args.kernel_sd)
        if m is None:
            print(f"{city:15s} SKIP (no forecast for {args.date} in mode={args.mode})")
            continue
        out["cities"][city] = m
        top = max(m["buckets"], key=lambda b: b["p"])
        flag = "" if m["calibrated"] else "  [UNCALIBRATED]"
        print(f"{city:15s} {m['icao']:4s} fc {m['fc_mean_c']:5.1f}C bias {m['bias_c']:+.2f}"
              f" -> top {top['lo']}-{top['hi']} p={top['p']:.2f}"
              f" (raw sum {m['raw_prob_sum']:.3f}){flag}")

    path = CACHE / f"model_{args.date}.json"
    path.write_text(json.dumps(out, indent=1))
    print(f"\n{len(out['cities'])} cities -> {path}")


if __name__ == "__main__":
    main()
