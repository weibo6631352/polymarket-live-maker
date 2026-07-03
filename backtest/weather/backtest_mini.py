#!/usr/bin/env python3
"""THE DECISIVE METRIC: PM mid at T-1 vs free model (previous_day1) vs actual outcomes.

For already-resolved target date(s) this script rebuilds, per city:
  outcome   — winning bucket from the resolution-grade METAR daily max (metar_max.py, raw
              IEM rows, never DSM), CROSS-CHECKED against Polymarket's actual resolution
              (gamma outcomePrices); mismatches are flagged and PM's resolution is scored
              (it IS the ground truth; the METAR max validates our plumbing).
  PM T-1    — CLOB /prices-history mid per bucket YES token, last point at or before the
              snapshot (--snap-utc, default 00:00 UTC on the target date, i.e. the evening
              before the local trading day: comparable information set to previous_day1).
              Buckets with no history get 0.005; the 11-vector is renormalized.
  model     — previous_day1 deterministic open-meteo run, bias-corrected per station on a
              trailing window ENDING D-1 (no target-day leakage), N(mu, sd) bucketed with
              resolution semantics (model.py machinery).
Then scores mean per-bucket Brier + winner log-loss for PM, model, and a 50/50 blend,
with a paired t-stat across cities.

Documented verdict this reproduces directionally (full 49-city, multi-date original run):
PM T-1 Brier 0.0616 vs model 0.0674 (paired t ~ 2.6) — the market BEATS the free model,
in every subset; blending toward the model only degrades PM.

CLI: python3 backtest_mini.py 2026-07-01 --cities nyc,london,paris,denver,istanbul
     python3 backtest_mini.py 2026-06-30 2026-07-01                (multiple dates pool)
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import statistics
import urllib.parse

from bias_calib import calibrate
from common import (CACHE, CLOB, bucket_contains, brier, c_to_f, get_json, log_loss_winner,
                    normalize, station_meta)
from fetch_cohort import build_cohort
from forecast import previous_run_daily_max
from metar_max import metar_daily_max
from model import bucket_probs_normal, DEFAULT_SD_C, MIN_SD_C

MISSING_MID = 0.005  # untraded tail buckets have no price history; ~min-tick placeholder


def pm_mid_at(token_id: str, snap_ts: int) -> float | None:
    """Last /prices-history point at or before snap_ts (8h lookback window)."""
    q = urllib.parse.urlencode({"market": token_id, "startTs": snap_ts - 8 * 3600,
                                "endTs": snap_ts + 3600, "fidelity": 10})
    d = get_json(f"{CLOB}/prices-history?{q}", cache_key=f"ph_{token_id[:16]}_{snap_ts}")
    pts = [p for p in d.get("history", []) if p["t"] <= snap_ts]
    return pts[-1]["p"] if pts else None


def score_city(city: str, rec: dict, date_iso: str, snap_ts: int, train_days: int,
               lead: int) -> dict | None:
    res, mkts = rec["resolution"], rec["markets"]
    meta = station_meta(res["icao"], res["kind"])

    # --- outcome: METAR max -> winning bucket; cross-check vs PM resolution
    obs = metar_daily_max(res["icao"], date_iso, tz=meta["tz"]).get(date_iso)
    if obs is None:
        print(f"{city:15s} SKIP no METAR data for {date_iso}")
        return None
    x_native = obs["max_f"] if res["unit"] == "F" else obs["max_c"]
    win_metar = next((i for i, m in enumerate(mkts)
                      if bucket_contains(m["bucket"]["lo"], m["bucket"]["hi"],
                                         res["semantics"], x_native)), None)
    win_pm = next((i for i, m in enumerate(mkts)
                   if (m["outcome_prices"] or ["0"])[0] == "1"), None)
    if win_pm is None:
        print(f"{city:15s} SKIP not resolved yet on PM")
        return None
    win = win_pm
    mismatch = win_metar != win_pm

    # --- PM probabilities at T-1
    raw = [pm_mid_at(m["token_yes"], snap_ts) for m in mkts]
    n_miss = sum(1 for p in raw if p is None)
    pm = normalize([p if p is not None else MISSING_MID for p in raw])

    # --- free model: previous_day{lead} + trailing bias ending D-1 (no leakage)
    d_prev = (dt.date.fromisoformat(date_iso) - dt.timedelta(days=1)).isoformat()
    cal = calibrate(res["icao"], d_prev, days=train_days, lead=lead, kind=res["kind"])
    bias_c = cal["bias_c"] if cal else 0.0
    sd_c = max(MIN_SD_C, cal["sd_c"]) if cal else DEFAULT_SD_C
    fc = previous_run_daily_max(meta["lat"], meta["lon"], meta["tz"], date_iso, lead=lead)
    if date_iso not in fc:
        print(f"{city:15s} SKIP no previous_day{lead} archive for {date_iso}")
        return None
    model = normalize(bucket_probs_normal(fc[date_iso], bias_c, sd_c,
                                          [m["bucket"] for m in mkts], res["semantics"]))
    blend = normalize([0.5 * (a + b) for a, b in zip(pm, model)])

    return {
        "city": city, "date": date_iso, "icao": res["icao"], "win_idx": win,
        "win_label": mkts[win]["question"], "obs_native": x_native,
        "metar_pm_mismatch": mismatch, "n_missing_mids": n_miss,
        "fc_c": round(fc[date_iso], 2), "bias_c": bias_c, "sd_c": sd_c,
        "brier_pm": brier(pm, win), "brier_model": brier(model, win),
        "brier_blend": brier(blend, win),
        "ll_pm": log_loss_winner(pm, win), "ll_model": log_loss_winner(model, win),
    }


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dates", nargs="+", help="resolved target date(s) YYYY-MM-DD")
    ap.add_argument("--cities", default=None, help="comma-separated city filter")
    ap.add_argument("--train-days", type=int, default=10, help="bias-calibration window")
    ap.add_argument("--lead", type=int, default=1, help="model forecast lead (previous_dayN)")
    ap.add_argument("--snap-utc", default="00:00",
                    help="PM snapshot HH:MM UTC on the target date (default 00:00 = T-1 eve)")
    args = ap.parse_args()

    keep = set(args.cities.split(",")) if args.cities else None
    hh, mm = map(int, args.snap_utc.split(":"))
    rows = []
    for date_iso in args.dates:
        snap_ts = int(dt.datetime.fromisoformat(date_iso)
                      .replace(hour=hh, minute=mm, tzinfo=dt.timezone.utc).timestamp())
        cohort = build_cohort(date_iso, keep)
        for city, rec in sorted(cohort["cities"].items()):
            r = score_city(city, rec, date_iso, snap_ts, args.train_days, args.lead)
            if r:
                rows.append(r)
                unit = "F" if rec["resolution"]["unit"] == "F" else "C"
                warn = "  [METAR!=PM]" if r["metar_pm_mismatch"] else ""
                print(f"{city:15s} {date_iso} obs {r['obs_native']:6.1f}{unit} "
                      f"fc {c_to_f(r['fc_c']):5.1f}F/{r['fc_c']:4.1f}C bias {r['bias_c']:+.2f} "
                      f"| Brier pm {r['brier_pm']:.4f} model {r['brier_model']:.4f} "
                      f"blend {r['brier_blend']:.4f}{warn}")

    if not rows:
        raise SystemExit("nothing scored")
    (CACHE / f"backtest_{args.dates[0]}.json").write_text(json.dumps(rows, indent=1))

    n = len(rows)
    b_pm = statistics.mean(r["brier_pm"] for r in rows)
    b_mod = statistics.mean(r["brier_model"] for r in rows)
    b_bl = statistics.mean(r["brier_blend"] for r in rows)
    diffs = [r["brier_model"] - r["brier_pm"] for r in rows]
    t = (statistics.mean(diffs) / (statistics.stdev(diffs) / math.sqrt(n))
         if n > 1 and statistics.stdev(diffs) > 0 else float("nan"))
    print(f"\n=== {n} city-days ===")
    print(f"Brier    : PM {b_pm:.4f}   model {b_mod:.4f}   blend50 {b_bl:.4f}")
    print(f"log-loss : PM {statistics.mean(r['ll_pm'] for r in rows):.3f}   "
          f"model {statistics.mean(r['ll_model'] for r in rows):.3f}")
    print(f"paired (model - PM) Brier diff {statistics.mean(diffs):+.4f}, t = {t:.2f}  "
          f"(positive diff => the MARKET beats the free model, matching the archived verdict)")
    mism = [r["city"] for r in rows if r["metar_pm_mismatch"]]
    if mism:
        print(f"METAR-vs-PM resolution mismatches (investigate): {mism}")


if __name__ == "__main__":
    main()
