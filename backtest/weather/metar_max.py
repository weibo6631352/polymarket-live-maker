#!/usr/bin/env python3
"""Resolution-grade daily maximum temperature from RAW METAR rows (IEM ASOS endpoint).

Inputs : station ICAO + local date(s). Day boundaries are the STATION'S timezone
         (IEM's `tz` request parameter does the local-time bucketing for us).
Outputs: per-local-date {max_f, max_c, n_obs} — the max over every raw METAR row
         (routine + specials, report_type 3 & 4) in that local day.

WHY RAW ROWS AND NOT THE DAILY SUMMARY: the IEM *daily summary* (DSM) endpoint reads
1-2 degF HIGH versus what Polymarket/Wunderground actually resolve at US stations
(DSM highs come from the station's 6-hour max-temp groups / sensor extremes, while
Wunderground's daily-history "High" is the max over the displayed METAR observations).
NEVER use the DSM endpoint for outcomes — this was measured, not theoretical.

CLI: python3 metar_max.py KLGA 2026-07-02
     python3 metar_max.py EGLC 2026-06-25 --end 2026-07-01   (range, one IEM request)

Used for: market outcomes (bucket membership) and for bias-training data (bias_calib.py).
"""

from __future__ import annotations

import argparse
import datetime as dt
import math
import urllib.parse

from common import IEM, f_to_c, get_text, iem_station_id, station_meta


def metar_daily_max(icao: str, d1: str, d2: str | None = None, tz: str | None = None) -> dict:
    """Daily max temp from raw METAR rows for local dates d1..d2 (inclusive, YYYY-MM-DD).

    Returns {date_iso: {"max_f": float, "max_c": float, "n_obs": int}} — dates with no
    parseable temperature rows are absent. One IEM request for the whole range.
    """
    d2 = d2 or d1
    if tz is None:
        tz = station_meta(icao)["tz"]
    a = dt.date.fromisoformat(d1)
    b = dt.date.fromisoformat(d2) + dt.timedelta(days=1)  # asos.py end date is exclusive
    q = urllib.parse.urlencode(
        {
            "station": iem_station_id(icao),
            "data": "tmpf",
            "year1": a.year, "month1": a.month, "day1": a.day,
            "year2": b.year, "month2": b.month, "day2": b.day,
            "tz": tz,                      # local-day bucketing at the station's timezone
            "format": "onlycomma",
            "latlon": "no",
            "missing": "M",
            "trace": "T",
        }
    ) + "&report_type=3&report_type=4"      # routine METAR + specials; NEVER DSM (see above)
    csv = get_text(f"{IEM}/cgi-bin/request/asos.py?{q}",
                   cache_key=f"metar_{icao}_{d1}_{d2}", ttl_s=6 * 3600)

    out: dict[str, dict] = {}
    for line in csv.splitlines()[1:]:
        parts = line.split(",")
        if len(parts) < 3:
            continue
        day, raw = parts[1][:10], parts[2].strip()
        try:
            tf = float(raw)
        except ValueError:
            continue  # 'M' missing
        cur = out.setdefault(day, {"max_f": -math.inf, "n_obs": 0})
        cur["max_f"] = max(cur["max_f"], tf)
        cur["n_obs"] += 1
    for day, cur in out.items():
        cur["max_c"] = round(f_to_c(cur["max_f"]), 3)
        cur["max_f"] = round(cur["max_f"], 3)
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("icao", help="resolution station ICAO, e.g. KLGA / EGLC / LFPB")
    ap.add_argument("date", help="local date YYYY-MM-DD (start of range)")
    ap.add_argument("--end", default=None, help="optional range end date (inclusive)")
    args = ap.parse_args()

    meta = station_meta(args.icao)
    print(f"# {args.icao} ({meta['name']}), tz={meta['tz']}")
    days = metar_daily_max(args.icao, args.date, args.end, tz=meta["tz"])
    for day in sorted(days):
        r = days[day]
        print(f"{day}  max {r['max_f']:6.1f} F = {r['max_c']:5.1f} C   ({r['n_obs']} obs)")
    if not days:
        print("no METAR rows returned (future date, or station id wrong?)")


if __name__ == "__main__":
    main()
