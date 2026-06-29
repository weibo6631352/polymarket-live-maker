#!/usr/bin/env python3
# copy_persist.py — OUT-OF-SAMPLE persistence test. A winner being profitable on their PAST trades
# doesn't mean copying their FUTURE trades works (they could regress). For each strong winner, sort
# resolved buys by time, split old-60% (the track record you'd see before following) vs new-40%
# (their "future" after you start). If copying the new-40% (flat 3c slippage) is still +EV, the edge
# PERSISTS out-of-sample = live-viable. Read-only, zero money.
import json, urllib.request, collections

def get(u):
    try:
        req = urllib.request.Request(u, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=25) as r:
            return json.load(r)
    except Exception:
        return None

lb = get("https://data-api.polymarket.com/v1/leaderboard?window=all&limit=20") or []
# strong, high-sample, large-edge winners identified earlier (exclude favorite-buyers / losers)
KEEP = {"S-Works", "ferrariChampions2026", "needheal", "bbwlover"}
targets = [(x.get("userName") or x.get("proxyWallet")[:10], x.get("proxyWallet"))
           for x in lb if (x.get("userName") in KEEP) or x.get("proxyWallet", "").startswith("0x2c335066")]

def copy_pnl(buys, res, slip=0.03):
    pnl = cost = 0.0; n = wins = 0
    for t in buys:
        c = t["conditionId"]
        if c not in res: continue
        o = t.get("outcome"); p = float(t.get("price", 0)); sz = float(t.get("size", 0))
        win = res[c].get(o, 0.0)
        cp = min(p + slip, 1.0)
        pnl += (win - cp) * sz; cost += cp * sz; n += 1; wins += (win > 0.5)
    return pnl, cost, n, wins

for nm, addr in targets:
    tr = []
    for off in range(0, 2500, 500):
        d = get("https://data-api.polymarket.com/trades?user=%s&limit=500&offset=%d" % (addr, off))
        if not d: break
        tr += d
        if len(d) < 500: break
    buys = sorted([t for t in tr if t.get("side") == "BUY"], key=lambda t: int(t.get("timestamp", 0)))
    conds = list(set(t["conditionId"] for t in buys)); res = {}
    for i in range(0, len(conds), 20):
        q = "&".join("condition_ids=%s" % c for c in conds[i:i+20])
        m = get("https://gamma-api.polymarket.com/markets?closed=true&limit=100&" + q) or []
        for mm in m:
            op = mm.get("outcomePrices"); outs = mm.get("outcomes")
            if isinstance(op, str): op = json.loads(op)
            if isinstance(outs, str): outs = json.loads(outs)
            if op and outs:
                res[mm.get("conditionId")] = {o: float(p) for o, p in zip(outs, op)}
    rb = [t for t in buys if t["conditionId"] in res]
    if len(rb) < 30: continue
    split = int(len(rb) * 0.6)
    old, new = rb[:split], rb[split:]
    po, co, no, wo = copy_pnl(old, res)
    pn, cn, nn, wn = copy_pnl(new, res)
    print("%-20s old60%%(n=%d): %+.1f%% win=%d%%   |   OUT-OF-SAMPLE new40%%(n=%d): %+.1f%% win=%d%%" % (
        nm[:20], no, 100*po/max(co, 1), 100*wo//max(no, 1), nn, 100*pn/max(cn, 1), 100*wn//max(nn, 1)))
