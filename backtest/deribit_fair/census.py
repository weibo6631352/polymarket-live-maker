#!/usr/bin/env python3
"""Census of OPEN Polymarket slow crypto date/level markets (not micro up/down)."""
import json, re, time, os, sys
import requests

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "census_open.json")
GAMMA = "https://gamma-api.polymarket.com"
S = requests.Session()
S.headers["User-Agent"] = "research/0.1"

def get(url, params, retries=3):
    for i in range(retries):
        try:
            r = S.get(url, params=params, timeout=30)
            if r.status_code == 200:
                return r.json()
        except Exception as e:
            print("retry", e, file=sys.stderr)
        time.sleep(1.5 * (i + 1))
    raise RuntimeError(f"GET failed {url} {params}")

# ---- pull all active crypto events (tag_slug=crypto), partitioned by end_date
# because the offset cap is 2000. ----
from datetime import datetime, timedelta, timezone

def pull_bucket(dmin, dmax):
    """dmin/dmax ISO strings or None. Returns list of events; raises if >cap."""
    out, offset = [], 0
    while True:
        p = dict(tag_slug="crypto", active="true", closed="false",
                 limit=100, offset=offset)
        if dmin: p["end_date_min"] = dmin
        if dmax: p["end_date_max"] = dmax
        batch = get(f"{GAMMA}/events", p)
        if not batch:
            break
        out.extend(batch)
        offset += 100
        if len(batch) < 100:
            break
        if offset > 2000:
            raise RuntimeError(f"bucket too big {dmin}..{dmax}")
        time.sleep(0.35)
    return out

now = datetime.now(timezone.utc)
# half-day buckets for the next 3 days (micro-dense), then one open tail
edges = []
t = datetime(now.year, now.month, now.day, tzinfo=timezone.utc)
for i in range(7):  # 3.5 days of 12h buckets
    edges.append(t + timedelta(hours=12 * i))
edges.append(t + timedelta(hours=84))
iso = lambda d: d.strftime("%Y-%m-%dT%H:%M:%SZ")

events, seen = [], set()
buckets = [(None, iso(edges[0]))]
buckets += [(iso(edges[i]), iso(edges[i + 1])) for i in range(len(edges) - 1)]
buckets += [(iso(edges[-1]), None)]
for dmin, dmax in buckets:
    b = pull_bucket(dmin, dmax)
    fresh = [e for e in b if e.get("id") not in seen]
    seen.update(e.get("id") for e in fresh)
    events.extend(fresh)
    print(f"bucket {dmin}..{dmax}: {len(b)} (new {len(fresh)}), total {len(events)}",
          file=sys.stderr)

MICRO = re.compile(r"up-or-down|updown|-am-et|-pm-et|5-?min|15-?min|1h-|hourly", re.I)
COIN = re.compile(r"\b(bitcoin|btc|ethereum|eth|solana|sol|xrp|doge|dogecoin)\b", re.I)
LEVEL = re.compile(r"(above|below|reach|hit|dip|price of|close at|all.?time high|\$[\d,]+)", re.I)

rows = []
for ev in events:
    eslug = ev.get("slug", "")
    etitle = ev.get("title", "")
    if MICRO.search(eslug) or MICRO.search(etitle):
        continue
    for m in ev.get("markets", []):
        q = m.get("question", "") or ""
        slug = m.get("slug", "") or ""
        if MICRO.search(slug) or MICRO.search(q):
            continue
        if not COIN.search(q) and not COIN.search(eslug):
            continue
        if not LEVEL.search(q):
            continue
        if not m.get("enableOrderBook"):
            continue
        rows.append(dict(
            event_slug=eslug, event_title=etitle,
            id=m.get("id"), slug=slug, question=q,
            description=m.get("description"),
            endDate=m.get("endDate"), endDateIso=m.get("endDateIso"),
            clobTokenIds=m.get("clobTokenIds"),
            conditionId=m.get("conditionId"),
            negRisk=m.get("negRisk"),
            outcomePrices=m.get("outcomePrices"),
            bestBid=m.get("bestBid"), bestAsk=m.get("bestAsk"),
            spread=m.get("spread"), lastTradePrice=m.get("lastTradePrice"),
            liquidity=m.get("liquidityNum"), volume=m.get("volumeNum"),
            volume24hr=m.get("volume24hr"),
            feeType=m.get("feeType"), feesEnabled=m.get("feesEnabled"),
            feeSchedule=m.get("feeSchedule"),
            rewardsMaxSpread=m.get("rewardsMaxSpread"),
            rewardsMinSize=m.get("rewardsMinSize"),
            orderPriceMinTickSize=m.get("orderPriceMinTickSize"),
            acceptingOrders=m.get("acceptingOrders"),
        ))

print(f"total events {len(events)}, candidate slow level markets {len(rows)}", file=sys.stderr)
with open(OUT, "w") as f:
    json.dump(rows, f, indent=1)
print(OUT)
