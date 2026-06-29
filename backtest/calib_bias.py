#!/usr/bin/env python3
# calib_bias.py — favorite-longshot bias test on PM. Across many RESOLVED binary markets,
# bucket the pre-resolution YES price and compare to the realized YES win-rate.
# If low buckets win LESS than priced (longshots overpriced) and high buckets win MORE
# (favorites underpriced), there's an exploitable bias. Read-only, zero money.
import json, math, urllib.request, time, sys, collections

def get(u):
    try:
        req = urllib.request.Request(u, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=20) as r:
            return json.load(r)
    except Exception:
        return None

def iso_to_unix(s):
    return int(time.mktime(time.strptime(s, "%Y-%m-%dT%H:%M:%SZ")) - time.timezone)

MIN_VOL = float(sys.argv[1]) if len(sys.argv) > 1 else 20000.0
LOOKBACK = 3600  # take YES price ~1h before close (the pre-resolution belief)

# Pull high-volume resolved binary markets (paginate), recent first.
base = "https://gamma-api.polymarket.com/markets?closed=true&limit=500&order=volumeNum&ascending=false"
mkts = []
for off in range(0, 6000, 500):
    d = get(base + f"&offset={off}")
    if not d:
        break
    mkts += d
    if len(mkts) >= 3000:
        break
print(f"resolved markets scanned: {len(mkts)}")

# bucket -> [n, wins]
buckets = collections.defaultdict(lambda: [0, 0])
used = 0
# strategy: buy YES when priced in [lo,hi]; track net pnl per $1 with half-spread cost
fav_pnl = 0.0; fav_n = 0   # buy favorites (price>0.5)
fade_pnl = 0.0; fade_n = 0 # fade longshots = buy NO when YES<0.5 (i.e. buy the favorite side either way -> same)
for m in mkts:
    try:
        outs = m.get("outcomes")
        if isinstance(outs, str): outs = json.loads(outs)
        if not outs or len(outs) != 2:
            continue
        op = m.get("outcomePrices")
        if isinstance(op, str): op = json.loads(op)
        toks = m.get("clobTokenIds")
        if isinstance(toks, str): toks = json.loads(toks)
        if not op or not toks or len(toks) < 2:
            continue
        if float(m.get("volumeNum") or 0) < MIN_VOL:
            continue
        close = iso_to_unix(m["endDate"])
        yes_won = 1 if str(op[0]) == "1" else 0
    except Exception:
        continue
    # pre-resolution YES price: prices-history, last live point <= close-600
    ph = get(f"https://clob.polymarket.com/prices-history?market={toks[0]}&startTs={close-2*86400}&endTs={close}&fidelity=60")
    hist = (ph or {}).get("history") or []
    live = [h for h in hist if int(h["t"]) <= close - 600 and 0.01 < float(h["p"]) < 0.99]
    if not live:
        continue
    # price ~LOOKBACK before close (closest)
    target = close - LOOKBACK
    px = min(live, key=lambda h: abs(int(h["t"]) - target))
    p = float(px["p"])
    b = min(int(p * 20) / 20.0, 0.95)  # 0.05-wide buckets
    buckets[b][0] += 1; buckets[b][1] += yes_won
    used += 1
    # strategy: always buy the FAVORITE side (the side priced >0.5), hold to settle, pay half-spread
    SPREAD = 0.02
    if p >= 0.5:
        fav_pnl += (1.0 if yes_won == 1 else 0.0) - (p + SPREAD/2); fav_n += 1
    else:
        fav_pnl += (1.0 if yes_won == 0 else 0.0) - ((1 - p) + SPREAD/2); fav_n += 1

print(f"markets used (had pre-resolution price, vol>={MIN_VOL:.0f}): {used}\n")
print("YES-price bucket | n | priced | actual win-rate | edge(actual-priced)")
for b in sorted(buckets):
    n, w = buckets[b]
    if n < 3:
        continue
    mid = b + 0.025
    wr = w / n
    print(f"  [{b:.2f},{b+0.05:.2f}) n={n:4d}  priced~{mid:.3f}  actual={wr:.3f}  edge={wr-mid:+.3f}")
print(f"\n=== STRATEGY: buy the favorite side every market, hold to settle (half of 2c spread) ===")
print(f"  bets={fav_n}  net_pnl(per $1)=${fav_pnl:.2f}  avg_edge_per_bet={fav_pnl/fav_n if fav_n else 0:+.4f}")
