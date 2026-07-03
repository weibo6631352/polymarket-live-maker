#!/usr/bin/env python3
"""Model vs live Polymarket mids for a cohort: discrepancy table + mid-sum sanity.

Inputs : cache/cohort_<date>.json and cache/model_<date>.json; live mids via CLOB batch
         POST /midpoints on each bucket's YES token.
Outputs: per city/bucket — model prob, PM mid, edge (model - PM); per-city sum of raw
         mids (negRisk YES mids should sum to ~1.0x; a sum far from 1 means thin/stale
         books — treat edges there with suspicion). PM mids are also shown renormalized.
         Rows are flagged when |edge| >= --flag (default 0.10) on the normalized mid.

This is a DISPLAY/diagnostic tool, not an order engine — nothing here trades. Remember
the standing verdict (README): PM's own mids beat this free model out-of-sample, so a
"discrepancy" is more likely model error than market error.

CLI: python3 compare.py 2026-07-03 [--cities nyc,paris] [--flag 0.10]
"""

from __future__ import annotations

import argparse
import json

from common import CACHE, CLOB, normalize, post_json


def fetch_mids(token_ids: list[str], batch: int = 20) -> dict[str, float]:
    """CLOB batch POST /midpoints -> {token_id: mid}. Missing/zero books absent."""
    mids: dict[str, float] = {}
    for i in range(0, len(token_ids), batch):
        chunk = token_ids[i:i + batch]
        d = post_json(f"{CLOB}/midpoints", [{"token_id": t} for t in chunk])
        for t, v in d.items():
            try:
                mids[t] = float(v)
            except (TypeError, ValueError):
                pass
    return mids


def _label(b: dict) -> str:
    lo, hi, u = b["lo"], b["hi"], b.get("unit", "")
    if lo is None:
        return f"<={hi}{u}"
    if hi is None:
        return f">={lo}{u}"
    return f"{lo}-{hi}{u}" if lo != hi else f"{lo}{u}"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("date", help="target date YYYY-MM-DD")
    ap.add_argument("--cities", default=None, help="comma-separated city filter")
    ap.add_argument("--flag", type=float, default=0.10,
                    help="flag |model - normalized PM mid| >= this")
    args = ap.parse_args()

    cohort = json.loads((CACHE / f"cohort_{args.date}.json").read_text())
    model = json.loads((CACHE / f"model_{args.date}.json").read_text())
    keep = set(args.cities.split(",")) if args.cities else None

    cities = [c for c in sorted(model["cities"])
              if c in cohort["cities"] and (keep is None or c in keep)]
    all_toks = [m["token_yes"] for c in cities for m in cohort["cities"][c]["markets"]]
    mids = fetch_mids(all_toks)

    for city in cities:
        mkts = cohort["cities"][city]["markets"]
        mod = model["cities"][city]
        pm_raw = [mids.get(m["token_yes"]) for m in mkts]
        filled = [p if p is not None else 0.0 for p in pm_raw]
        mid_sum = sum(filled)
        pm_norm = normalize(filled) if mid_sum > 0 else filled
        sanity = "ok" if 0.9 <= mid_sum <= 1.15 else "SUSPECT"
        print(f"\n== {city} ({mod['icao']}, fc {mod['fc_mean_c']:.1f}C"
              f"{'' if mod['calibrated'] else ', UNCALIBRATED'})"
              f"  mid-sum {mid_sum:.3f} [{sanity}]")
        print(f"   {'bucket':>10s} {'model':>7s} {'pm_mid':>7s} {'pm_nrm':>7s} {'edge':>7s}")
        for mkt, bk, praw, pnorm in zip(mkts, mod["buckets"], pm_raw, pm_norm):
            edge = bk["p"] - pnorm
            flag = "  <<<" if abs(edge) >= args.flag else ""
            praw_s = f"{praw:7.3f}" if praw is not None else "      -"
            print(f"   {_label(mkt['bucket']):>10s} {bk['p']:7.3f} {praw_s} "
                  f"{pnorm:7.3f} {edge:+7.3f}{flag}")


if __name__ == "__main__":
    main()
