#!/usr/bin/env python3
# copy_quality.py — over the longest available history, is the winners' copy-edge BROAD (steadily
# copyable) or TAIL-ONLY (fragile — all profit in a few unrepeatable spikes)?
# Key test: remove the top 1% / 5% positions by P&L; if the rest is still +EV there's a broad base.
# Plus % positive months and max drawdown. Read-only, zero money.
import json, urllib.request, time, calendar, collections

def get(u):
    try:
        req = urllib.request.Request(u, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=25) as r:
            return json.load(r)
    except Exception:
        return None

lb = get("https://data-api.polymarket.com/v1/leaderboard?window=all&limit=20") or []
def addr_of(pred):
    return next((x.get("proxyWallet") for x in lb if pred(x)), None)
picks = [
    ("0x2c335066", addr_of(lambda x: x.get("proxyWallet", "").startswith("0x2c335066"))),
    ("S-Works", addr_of(lambda x: x.get("userName") == "S-Works")),
    ("ferrari", addr_of(lambda x: x.get("userName") == "ferrariChampions2026")),
    ("needheal", addr_of(lambda x: x.get("userName") == "needheal")),
]

for nm, addr in picks:
    if not addr:
        continue
    tr = []
    for off in range(0, 9000, 500):
        d = get("https://data-api.polymarket.com/trades?user=%s&limit=500&offset=%d" % (addr, off))
        if not d:
            break
        tr += d
        if len(d) < 500:
            break
    buys = [t for t in tr if t.get("side") == "BUY"]
    conds = list(set(t["conditionId"] for t in buys)); res = {}
    for i in range(0, len(conds), 20):
        q = "&".join("condition_ids=%s" % c for c in conds[i:i+20])
        for mm in (get("https://gamma-api.polymarket.com/markets?closed=true&limit=100&" + q) or []):
            op = mm.get("outcomePrices"); outs = mm.get("outcomes")
            if isinstance(op, str): op = json.loads(op)
            if isinstance(outs, str): outs = json.loads(outs)
            try:
                end = calendar.timegm(time.strptime(mm["endDate"], "%Y-%m-%dT%H:%M:%SZ"))
            except Exception:
                end = None
            if op and outs and end:
                res[mm.get("conditionId")] = {"w": {o: float(p) for o, p in zip(outs, op)}, "end": end}
    pnls = []
    for t in buys:
        c = t["conditionId"]
        if c not in res:
            continue
        o = t.get("outcome"); p = float(t.get("price", 0)); sz = float(t.get("size", 0))
        pnls.append((res[c]["end"], (res[c]["w"].get(o, 0.0) - min(p + 0.03, 1)) * sz))
    if len(pnls) < 20:
        continue
    pnls.sort()
    total = sum(x[1] for x in pnls)
    span = (pnls[-1][0] - pnls[0][0]) / 86400
    bypnl = sorted([x[1] for x in pnls], reverse=True)
    k1 = max(1, len(bypnl)//100); k5 = max(1, len(bypnl)//20)
    ex1 = total - sum(bypnl[:k1]); ex5 = total - sum(bypnl[:k5])
    mo = collections.defaultdict(float)
    for end, pnl in pnls:
        mo[end//(30*86400)] += pnl
    mvals = [mo[m] for m in sorted(mo)]; posm = sum(1 for v in mvals if v > 0)
    cum = peak = mdd = 0.0
    for _, pnl in pnls:
        cum += pnl; peak = max(peak, cum); mdd = min(mdd, cum - peak)
    print("%-12s %d单 跨%.0f天(%.1f月) 总$%.0f" % (nm, len(pnls), span, span/30, total))
    print("   去top1%%:$%.0f (%+d%%)  去top5%%:$%.0f (%+d%%)  正月%d/%d  最大回撤$%.0f(总利的%d%%)" % (
        ex1, 100*ex1/total if total else 0, ex5, 100*ex5/total if total else 0,
        posm, len(mvals), mdd, abs(100*mdd/total) if total else 0))
