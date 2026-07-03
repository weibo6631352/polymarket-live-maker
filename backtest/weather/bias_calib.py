#!/usr/bin/env python3
"""Per-station NWP-grid bias from trailing METAR days (MANDATORY before bucketing).

The open-meteo grid runs cold vs the resolving station at 34/48 stations (mean +0.42 degC,
up to about +/-1.9 degC at Chengdu/Guangzhou/Buenos Aires/Incheon/Munich; -1.8 degC at
Lucknow). This script measures, per station:

    bias_c = mean( forecast_prev_day{lead}_max  -  METAR_daily_max )   over trailing N days
    sd_c   = stdev of those residuals            (~0.95 degC typical after correction)

using ONE historical-forecast call + ONE IEM METAR call per station. model.py then uses
mu_corrected = forecast - bias_c and sd_c as the distribution width.

Inputs : station ICAO, --end (last training day, INCLUSIVE — for a backtest of target
         date D pass --end D-1 so no target-day data leaks), --days N (default 10),
         --lead (default 1).
Outputs: merges {icao: {bias_c, sd_c, n, lead, train_end}} into cache/station_bias.json.

CLI: python3 bias_calib.py KLGA --end 2026-06-30 --days 10
     python3 bias_calib.py EGLC --end 2026-06-30 --kind wunderground
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import statistics

from common import CACHE, station_meta
from forecast import previous_run_daily_max
from metar_max import metar_daily_max

BIAS_PATH = CACHE / "station_bias.json"


def calibrate(icao: str, end_date: str, days: int = 10, lead: int = 1,
              kind: str = "") -> dict | None:
    """Trailing-window bias for one station; returns the bias record or None if <3 days."""
    meta = station_meta(icao, kind)
    d_end = dt.date.fromisoformat(end_date)
    d_start = d_end - dt.timedelta(days=days - 1)
    fc = previous_run_daily_max(meta["lat"], meta["lon"], meta["tz"],
                                d_start.isoformat(), d_end.isoformat(), lead=lead)
    obs = metar_daily_max(icao, d_start.isoformat(), d_end.isoformat(), tz=meta["tz"])
    resid = [fc[day] - obs[day]["max_c"] for day in sorted(fc) if day in obs]
    if len(resid) < 3:
        return None
    return {
        "bias_c": round(statistics.mean(resid), 3),
        "sd_c": round(statistics.stdev(resid), 3),
        "n": len(resid),
        "lead": lead,
        "train_end": end_date,
    }


def load_bias() -> dict:
    return json.loads(BIAS_PATH.read_text()) if BIAS_PATH.exists() else {}


def save_bias(all_bias: dict) -> None:
    BIAS_PATH.parent.mkdir(parents=True, exist_ok=True)
    BIAS_PATH.write_text(json.dumps(all_bias, indent=1, sort_keys=True))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("icao", help="resolution station ICAO")
    ap.add_argument("--end", required=True,
                    help="last training day YYYY-MM-DD (inclusive); use target-date-minus-1")
    ap.add_argument("--days", type=int, default=10, help="trailing window length")
    ap.add_argument("--lead", type=int, default=1, help="forecast lead to calibrate (1..7)")
    ap.add_argument("--kind", default="", help="resolution kind ('hko' switches coordinates)")
    args = ap.parse_args()

    rec = calibrate(args.icao, args.end, args.days, args.lead, args.kind)
    if rec is None:
        raise SystemExit("fewer than 3 overlapping forecast/METAR days — cannot calibrate")
    all_bias = load_bias()
    all_bias[args.icao] = rec
    save_bias(all_bias)
    print(f"{args.icao}: bias {rec['bias_c']:+.2f} C (grid-minus-station), "
          f"resid sd {rec['sd_c']:.2f} C over {rec['n']} days (lead {rec['lead']}) "
          f"-> {BIAS_PATH}")


if __name__ == "__main__":
    main()
