#!/usr/bin/env python3
# copy_realistic.py — the REAL slippage test. Instead of a flat +3c, copy each winner BUY at the
# actual market price LAG seconds AFTER their trade (from prices-history) = what a copy-bot would
# really fill at given detection+execution lag. Held to resolution. Does the edge survive REAL drift?
import json, urllib.request, collections, sys

def get(u):
    try:
        req = urllib.request.Request(u, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=25) as r:
            return json.load(r)
    except Exception:
        return None

# strong, high-sample winners to test
lb = get("https://data-api.polymarket.com/v1/leaderboard?window=all&limit=20") or []
targets = []
for x in lb:
    if (x.get("userName") or "") in ("S-Works",) or (x.get("proxyWallet","").startswith("0x2c335066")):
        targets.append(x.get("proxyWallet"))
LAGS = [300, 1800]  # 5min, 30min detection+exec lag

for addr in targets:
    tr = []
    for off in range(0, 2000, 500):
        d = get("https://data-api.polymarket.com/trades?user=%s&limit=500&offset=%d" % (addr, off))
        if not d: break
        tr += d
        if len(d) < 500: break
    buys = [t for t in tr if t.get("side") == "BUY"]
    conds = list(set(t["conditionId"] for t in buys))
    res = {}
    for i in range(0, len(conds), 20):
        q = "&".join("condition_ids=%s" % c for c in conds[i:i+20])
        m = get("https://gamma-api.polymarket.com/markets?closed=true&limit=100&" + q) or []
        for mm in m:
            op = mm.get("outcomePrices"); outs = mm.get("outcomes")
            if isinstance(op, str): op = json.loads(op)
            if isinstance(outs, str): outs = json.loads(outs)
            tk = mm.get("clobTokenIds")
            if isinstance(tk, str): tk = json.loads(tk)
            if op and outs:
                res[mm.get("conditionId")] = {"win": {o: float(p) for o, p in zip(outs, op)},
                                              "tok": {o: t for o, t in zip(outs, tk)} if tk else {}}
    # for a sample of resolved buys, fetch price at trade_time + lag
    sample = [t for t in buys if t["conditionId"] in res][:200]
    agg = {0: [0.0, 0.0], 300: [0.0, 0.0], 1800: [0.0, 0.0]}  # lag -> [pnl, cost]
    used = 0
    for t in sample:
        c = t["conditionId"]; o = t.get("outcome"); p0 = float(t.get("price", 0)); sz = float(t.get("size", 0))
        tt = int(t.get("timestamp", 0))
        tok = res[c]["tok"].get(o)
        win = res[c]["win"].get(o, 0.0)
        if not tok: continue
        ph = get("https://clob.polymarket.com/prices-history?market=%s&startTs=%d&endTs=%d&fidelity=1" % (tok, tt-120, tt+2000))
        hist = (ph or {}).get("history") or []
        if not hist: continue
        used += 1
        agg[0][0] += (win - p0) * sz; agg[0][1] += p0 * sz
        for lag in LAGS:
            # price at tt+lag (closest history point >= tt, nearest to target)
            target = tt + lag
            cand = [h for h in hist if int(h["t"]) >= tt]
            if not cand: cand = hist
            px = min(cand, key=lambda h: abs(int(h["t"]) - target))
            cp = float(px["p"])
            agg[lag][0] += (win - cp) * sz; agg[lag][1] += cp * sz
    nm = (lb and next((y.get("userName") for y in lb if y.get("proxyWallet") == addr), addr[:10])) or addr[:10]
    print("=== %s (sample buys w/ history: %d) ===" % (nm, used))
    for lag in [0, 300, 1800]:
        pnl, cost = agg[lag]
        tag = "their price" if lag == 0 else "+%dmin" % (lag//60)
        print("  copy @ %-11s: %+.1f%%  (on $%.0f)" % (tag, 100*pnl/max(cost, 1), cost))
