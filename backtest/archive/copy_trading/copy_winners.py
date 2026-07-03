#!/usr/bin/env python3
# copy_winners.py — can we COPY-TRADE the proven leaderboard winners profitably?
# For each top-PnL account: their REAL realized P&L (proper buy+sell+resolution accounting),
# and the P&L of copying every BUY they make at their price + slippage, held to resolution.
# If copying survives realistic slippage across MANY winners -> a verified, copyable edge.
# Read-only, zero money.
import json, urllib.request, collections

def get(u):
    try:
        req = urllib.request.Request(u, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=25) as r:
            return json.load(r)
    except Exception:
        return None

lb = get("https://data-api.polymarket.com/v1/leaderboard?window=all&limit=20") or []
print("%-18s %10s %7s %7s %7s %7s %6s" % ("acct", "realP&L", "copy0", "copy3", "copy5", "win%", "mkts"))
agg = collections.defaultdict(float)
for x in lb[:20]:
    a = x.get("proxyWallet")
    tr = []
    for off in range(0, 3000, 500):
        d = get("https://data-api.polymarket.com/trades?user=%s&limit=500&offset=%d" % (a, off))
        if not d:
            break
        tr += d
        if len(d) < 500:
            break
    bym = collections.defaultdict(list)
    for t in tr:
        bym[t["conditionId"]].append(t)
    conds = list(bym.keys()); res = {}
    for i in range(0, len(conds), 20):
        q = "&".join("condition_ids=%s" % c for c in conds[i:i+20])
        m = get("https://gamma-api.polymarket.com/markets?closed=true&limit=100&" + q) or []
        for mm in m:
            op = mm.get("outcomePrices"); outs = mm.get("outcomes")
            if isinstance(op, str): op = json.loads(op)
            if isinstance(outs, str): outs = json.loads(outs)
            if op and outs:
                res[mm.get("conditionId")] = {o: float(p) for o, p in zip(outs, op)}
    own = c0 = c3 = c5 = cost = 0.0; n = wins = 0
    for c, ts in bym.items():
        if c not in res:
            continue
        pos = collections.defaultdict(float); cash = 0.0
        for t in ts:
            o = t.get("outcome"); p = float(t.get("price", 0)); sz = float(t.get("size", 0))
            if t.get("side") == "BUY":
                pos[o] += sz; cash -= p*sz; cost += p*sz; win = res[c].get(o, 0.0)
                c0 += (win-p)*sz; c3 += (win-min(p+0.03, 1))*sz; c5 += (win-min(p+0.05, 1))*sz
                n += 1; wins += (win > 0.5)
            else:
                pos[o] -= sz; cash += p*sz
        for o, sh in pos.items():
            cash += sh * res[c].get(o, 0.0)
        own += cash
    if cost < 2000:
        continue
    rc = len([c for c in bym if c in res])
    print("%-18s %10.0f %6.1f%% %6.1f%% %6.1f%% %5d%% %6d" % (
        (x.get("userName") or a[:12])[:18], own, 100*c0/cost, 100*c3/cost, 100*c5/cost,
        100*wins//max(n, 1), rc))
    agg["c0"] += c0; agg["c3"] += c3; agg["c5"] += c5; agg["cost"] += cost
if agg["cost"]:
    print("\n=== AGGREGATE across copyable winners ===")
    print("copy@same=%.1f%%  copy+3c=%.1f%%  copy+5c=%.1f%%  (on $%.0f deployed)" % (
        100*agg["c0"]/agg["cost"], 100*agg["c3"]/agg["cost"], 100*agg["c5"]/agg["cost"], agg["cost"]))
