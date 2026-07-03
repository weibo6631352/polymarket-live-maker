#!/usr/bin/env python3
"""Open-meteo forecasts for a station's coordinates -> daily-max distribution (degC).

Two modes (both keyless, read-only):

  ensemble  — LIVE probability distributions from the ensemble API
              (icon_seamless + gfs_seamless + ecmwf_ifs025 = ~122 members; verified 122
              member series on 2026-07-03). Per member: max of hourly temperature_2m over
              the target STATION-LOCAL date. Only works for today/future dates.

  previous  — BACKTEST: archived deterministic previous-model-runs from the
              historical-forecast API via hourly=temperature_2m_previous_day{N}
              (N = 1..7, the run issued N days before). Single value per lead.
              *** Ensemble previous-runs are NOT archived — requesting the _previous_dayN
              variables against ensemble models returns all-null. Deterministic
              previous-runs are the only archived backtest forecast. ***

NWP GRID BIAS WARNING: the open-meteo grid runs cold vs the resolving station at 34/48
stations (mean +0.42 degC obs-vs-grid, up to about +/-1.9 degC at Chengdu / Guangzhou /
Buenos Aires / Incheon / Munich; -1.8 degC at Lucknow). Per-station bias correction from
trailing METAR days (bias_calib.py) is MANDATORY before turning these into bucket
probabilities (residual sd after correction is about 0.95 degC).

CLI: python3 forecast.py KLGA 2026-07-04 --mode ensemble
     python3 forecast.py KLGA 2026-07-01 --mode previous --lead 1
Outputs cache/forecast_<icao>_<date>_<mode><lead>.json and prints a summary.
"""

from __future__ import annotations

import argparse
import json
import statistics
import urllib.parse

from common import CACHE, OM_ENSEMBLE, OM_HISTFC, get_json, station_meta

ENSEMBLE_MODELS = "icon_seamless,gfs_seamless,ecmwf_ifs025"


def ensemble_daily_max(lat: float, lon: float, tz: str, date_iso: str) -> list[float]:
    """Per-ensemble-member daily max (degC) over the station-local target date."""
    q = urllib.parse.urlencode({
        "latitude": round(lat, 4), "longitude": round(lon, 4),
        "hourly": "temperature_2m",
        "models": ENSEMBLE_MODELS,
        "start_date": date_iso, "end_date": date_iso,
        "timezone": tz,
    })
    d = get_json(f"{OM_ENSEMBLE}/v1/ensemble?{q}",
                 cache_key=f"ens_{lat:.3f}_{lon:.3f}_{date_iso}", ttl_s=3600)
    hourly = d["hourly"]
    maxes = []
    for key, series in hourly.items():
        if not key.startswith("temperature_2m"):
            continue
        vals = [v for v in series if v is not None]
        if vals:
            maxes.append(max(vals))
    return maxes


def previous_run_daily_max(lat: float, lon: float, tz: str, d1: str, d2: str | None = None,
                           lead: int = 1) -> dict[str, float]:
    """Archived deterministic forecast issued `lead` days before, per local date d1..d2.

    Returns {date_iso: daily_max_degC}. Dates the archive has no data for are absent.
    (historical-forecast API, best_match deterministic model; ensemble not archived.)
    """
    d2 = d2 or d1
    var = f"temperature_2m_previous_day{lead}"
    q = urllib.parse.urlencode({
        "latitude": round(lat, 4), "longitude": round(lon, 4),
        "hourly": var,
        "start_date": d1, "end_date": d2,
        "timezone": tz,
    })
    d = get_json(f"{OM_HISTFC}/v1/forecast?{q}",
                 cache_key=f"prev{lead}_{lat:.3f}_{lon:.3f}_{d1}_{d2}", ttl_s=24 * 3600)
    times, series = d["hourly"]["time"], d["hourly"][var]
    by_day: dict[str, list[float]] = {}
    for ts, v in zip(times, series):
        if v is not None:
            by_day.setdefault(ts[:10], []).append(v)
    return {day: max(vs) for day, vs in by_day.items()}


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("icao", help="resolution station ICAO (coordinates looked up via IEM)")
    ap.add_argument("date", help="target station-local date YYYY-MM-DD")
    ap.add_argument("--mode", choices=["ensemble", "previous"], default="ensemble")
    ap.add_argument("--lead", type=int, default=1, help="previous-run lead days (1..7)")
    args = ap.parse_args()

    meta = station_meta(args.icao)
    out = {"icao": args.icao, "date": args.date, "mode": args.mode, "station": meta}

    if args.mode == "ensemble":
        maxes = ensemble_daily_max(meta["lat"], meta["lon"], meta["tz"], args.date)
        if not maxes:
            raise SystemExit("no ensemble members returned — past date? (ensemble is live-only)")
        out["member_max_c"] = [round(m, 2) for m in maxes]
        print(f"{args.icao} {args.date}: {len(maxes)} members, "
              f"mean {statistics.mean(maxes):.1f} C, sd {statistics.pstdev(maxes):.2f} C, "
              f"range [{min(maxes):.1f}, {max(maxes):.1f}]")
    else:
        got = previous_run_daily_max(meta["lat"], meta["lon"], meta["tz"],
                                     args.date, lead=args.lead)
        if args.date not in got:
            raise SystemExit("previous-run archive has no data for that date/lead")
        out["lead"] = args.lead
        out["det_max_c"] = round(got[args.date], 2)
        print(f"{args.icao} {args.date}: previous_day{args.lead} max {got[args.date]:.1f} C")

    path = CACHE / f"forecast_{args.icao}_{args.date}_{args.mode}{args.lead}.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(out, indent=1))
    print(f"wrote {path}")


if __name__ == "__main__":
    main()
