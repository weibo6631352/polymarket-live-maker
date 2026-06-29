#!/usr/bin/env python3
# broad_edge_scan.py — hunt for a BROAD mechanical edge (snipers/arbs), unlike the tail-driven
# directional bettors. For the highest-frequency accounts on the tape, compute proper realized P&L
# AND the P&L excluding their top 1% positions. An account that stays +EV after removing its top 1%
# (= many small consistent wins, low variance) would be a mechanical edge worth replicating.
# Read-only, zero money.
import json, urllib.request, collections

def get(u):
    try:
        req = urllib.request.Request(u, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=25) as r:
            return json.load(r)
    except Exception:
        return None

# highest-frequency accounts from the recent tape (these are the bots: MMs, snipers, arbs)
W = collections.Counter(); cry = collections.Counter()
seen = set()
for off in range(0, 4000, 500):
    d = get("https://data-api.polymarket.com/trades?limit=500&offset=%d" % off) or []
    for t in d:
        k = (t["proxyWallet"], t.get("timestamp"), t.get("asset"), t.get("size"))
        if k in seen: continue
        seen.add(k)
        W[t["proxyWallet"]] += 1
        if "up or down" in (t.get("title") or "").lower(): cry[t["proxyWallet"]] += 1
cands = [w for w, n in W.most_common(22)]
print("scanning %d high-frequency accounts for a BROAD edge...\n" % len(cands))
print("%-14s %9s %10s %7s %6s %5s" % ("acct", "realP&L", "excl-top1%", "win%", "mkts", "cry%"))
results = []
for addr in cands:
    tr = []
    for off in range(0, 2500, 500):
        d = get("https://data-api.polymarket.com/trades?user=%s&limit=500&offset=%d" % (addr, off))
        if not d: break
        tr += d
        if len(d) < 500: break
    bym = collections.defaultdict(list)
    for t in tr: bym[t["conditionId"]].append(t)
    conds = list(bym.keys()); res = {}
    for i in range(0, len(conds), 20):
        q = "&".join("condition_ids=%s" % c for c in conds[i:i+20])
        for mm in (get("https://gamma-api.polymarket.com/markets?closed=true&limit=100&" + q) or []):
            op = mm.get("outcomePrices"); outs = mm.get("outcomes")
            if isinstance(op, str): op = json.loads(op)
            if isinstance(outs, str): outs = json.loads(outs)
            if op and outs:
                res[mm.get("conditionId")] = {o: float(p) for o, p in zip(outs, op)}
    # per-market realized P&L (proper)
    mpnl = []
    for c, ts in bym.items():
        if c not in res: continue
        pos = collections.defaultdict(float); cash = 0.0
        for t in ts:
            o = t.get("outcome"); p = float(t.get("price", 0)); sz = float(t.get("size", 0))
            if t.get("side") == "BUY": pos[o] += sz; cash -= p*sz
            else: pos[o] -= sz; cash += p*sz
        for o, sh in pos.items(): cash += sh * res[c].get(o, 0.0)
        mpnl.append(cash)
    if len(mpnl) < 15: continue
    total = sum(mpnl)
    k1 = max(1, len(mpnl)//100)
    exTop = total - sum(sorted(mpnl, reverse=True)[:k1])
    win = 100*sum(1 for x in mpnl if x > 0)//len(mpnl)
    crypct = 100*cry[addr]//max(W[addr], 1)
    results.append((exTop, total, win, len(mpnl), crypct, addr))
for exTop, total, win, nm, crypct, addr in sorted(results, reverse=True):
    flag = "  <<< BROAD +EV" if (total > 0 and exTop > 0) else ""
    print("%-14s %9.0f %10.0f %6d%% %6d %4d%%%s" % (addr[:14], total, exTop, win, nm, crypct, flag))
