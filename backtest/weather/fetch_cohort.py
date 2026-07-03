#!/usr/bin/env python3
"""Fetch one date's cohort of Polymarket daily-temperature markets from gamma.

Polymarket lists 49 cities x 11 negRisk buckets/day ("Highest temperature in <CITY> on
<DATE>?"), created around 01:03 UTC (as of 2026-07 markets exist up to ~2 days ahead),
endDate = <target date>T12:00:00Z. Discovery is by tag_id=104596 + an END-DATE WINDOW on
the target day (gamma /events offset caps make raw offset paging unreliable — page by
date windows).

Inputs : target date YYYY-MM-DD; optional --cities filter (slug fragments, e.g. nyc,paris);
         optional --books (CLOB batch /books best-bid/ask per YES token — extra API load).
Outputs: cache/cohort_<date>.json —
         {date, fetched_at, cities: {city: {event_slug, neg_risk, created_at, closed,
            resolution: {kind, icao, unit, semantics},   # parsed PER MARKET from the
                                                         # gamma description (station URL);
                                                         # never hardcode a guessed station
            markets: [{slug, question, bucket:{lo,hi,unit}, condition_id,
                       token_yes, token_no, outcome_prices, closed, book?}] }}}

CLI: python3 fetch_cohort.py 2026-07-03 --cities nyc,london --books
"""

from __future__ import annotations

import argparse
import datetime as dt
import json

from common import (CACHE, CLOB, city_from_slug, gamma_temp_events, parse_bucket,
                    parse_resolution_source, post_json)


def _bucket_sort_key(b: dict) -> float:
    if b["lo"] is None:
        return -1e9
    if b["hi"] is None:
        return 1e9
    return float(b["lo"])


def build_cohort(date_iso: str, cities: set[str] | None = None) -> dict:
    """Cohort dict for a date (see module docstring for shape). No books here."""
    events = gamma_temp_events(date_iso)
    out: dict = {"date": date_iso,
                 "fetched_at": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
                 "cities": {}}
    for ev in events:
        city = city_from_slug(ev["slug"])
        if cities and city not in cities:
            continue
        markets = []
        resolution = None
        for m in ev.get("markets", []):
            try:
                bucket = parse_bucket(m["question"])
            except ValueError:
                continue  # non-bucket market slipped into the event (never seen; defensive)
            if resolution is None:
                resolution = parse_resolution_source(m.get("description", ""))
            toks = json.loads(m["clobTokenIds"])  # outcomes are ["Yes","No"] -> toks[0]=Yes
            markets.append({
                "slug": m["slug"],
                "question": m["question"],
                "bucket": bucket,
                "condition_id": m.get("conditionId"),
                "token_yes": toks[0],
                "token_no": toks[1] if len(toks) > 1 else None,
                "outcome_prices": json.loads(m.get("outcomePrices") or "[]"),
                "closed": m.get("closed", False),
            })
        markets.sort(key=lambda m: _bucket_sort_key(m["bucket"]))
        out["cities"][city] = {
            "event_slug": ev["slug"],
            "neg_risk": ev.get("negRisk"),
            "created_at": ev.get("createdAt"),
            "closed": ev.get("closed"),
            "resolution": resolution,
            "markets": markets,
        }
    return out


def attach_books(cohort: dict, batch: int = 20) -> None:
    """Fetch CLOB batch /books for every YES token; store compact best-bid/ask/mid."""
    toks = [m["token_yes"] for c in cohort["cities"].values() for m in c["markets"]]
    books: dict[str, dict] = {}
    for i in range(0, len(toks), batch):
        chunk = toks[i:i + batch]
        for b in post_json(f"{CLOB}/books", [{"token_id": t} for t in chunk]):
            bids, asks = b.get("bids") or [], b.get("asks") or []
            bb = max((float(x["price"]) for x in bids), default=None)
            ba = min((float(x["price"]) for x in asks), default=None)
            books[b["asset_id"]] = {
                "best_bid": bb, "best_ask": ba,
                "mid": round((bb + ba) / 2, 4) if bb is not None and ba is not None else None,
            }
    for c in cohort["cities"].values():
        for m in c["markets"]:
            m["book"] = books.get(m["token_yes"])


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("date", help="target date YYYY-MM-DD")
    ap.add_argument("--cities", default=None,
                    help="comma-separated city slugs to keep (default: all)")
    ap.add_argument("--books", action="store_true", help="also fetch CLOB books per bucket")
    args = ap.parse_args()

    cities = set(args.cities.split(",")) if args.cities else None
    cohort = build_cohort(args.date, cities)
    if not cohort["cities"]:
        raise SystemExit(f"no temperature events found for {args.date}")
    if args.books:
        attach_books(cohort)

    path = CACHE / f"cohort_{args.date}.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(cohort, indent=1))

    for city, c in sorted(cohort["cities"].items()):
        r = c["resolution"]
        print(f"{city:15s} {r['kind']:14s} {r['icao']}  unit={r['unit']} sem={r['semantics']}"
              f"  buckets={len(c['markets'])}  closed={c['closed']}")
    print(f"\n{len(cohort['cities'])} cities -> {path}")


if __name__ == "__main__":
    main()
