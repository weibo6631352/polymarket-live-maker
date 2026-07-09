#!/usr/bin/env python3
"""Probe: are any SHORT-DATED (<= N days) crypto reach/hit markets live on Polymarket?

The touch leg only trades reach markets in their final <=3 days (late-entry edge). As of 2026-07-09 PM
lists ONLY 'what-price-will-X-hit-before-2027' year-end reach (~176d out), which the strategy correctly
skips — so the touch leg sits idle with 0 orders. This probe flags when a SHORT-DATED reach series appears
(advance notice, BEFORE it enters the d<=3 tradeable window), so the armed touch leg's first activity is
anticipated. Run it each monitoring cycle (local Mac is fine — same live gamma data the box sees).

  python3 touch_reach_probe.py [max_days=10]      exit 0 always; prints the verdict.
"""
import sys, time
from datetime import datetime
import touch_curate as tc

MAXD = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
COINS = ("bitcoin", "ethereum", "solana", "xrp")

evs = tc._get("/events", dict(tag_slug="crypto", active="true", closed="false", limit=500)) or []
now = time.time()
short, longd = [], 0
for e in evs:
    blob = (e.get("title", "") + " " + e.get("slug", "")).lower()
    if ("hit" not in blob and "reach" not in blob) or not any(c in blob for c in COINS):
        continue
    ds = []
    for m in e.get("markets", []):
        try:
            ds.append(datetime.fromisoformat((m.get("endDate") or "").replace("Z", "+00:00")).timestamp())
        except Exception:
            pass
    if not ds:
        continue
    dmin = (min(ds) - now) / 86400.0
    if -1 < dmin <= MAXD:
        short.append((round(dmin, 1), e.get("slug", ""), len(e.get("markets", []))))
    elif dmin > MAXD:
        longd += 1

if short:
    print(f"*** SHORT-DATED REACH LIVE ({len(short)} series <= {MAXD:.0f}d) — touch leg's prey has appeared; "
          f"watch WL-TOUCH-COUNT / first touch order as they enter the d<=3 window! ***")
    for d, s, n in sorted(short):
        print(f"  d_end={d:>6}d  markets={n:>3}  {s}")
else:
    print(f"no short-dated reach (<= {MAXD:.0f}d) — only {longd} long-dated (before-2027) reach series live; "
          f"touch leg correctly idle (nothing in its d<=3 window).")
