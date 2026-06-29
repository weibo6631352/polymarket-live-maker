#!/usr/bin/env python3
"""Hunt for PROFITABLE BOTS with a SYSTEMATIC (borrowable) edge. ZERO real money — read-only GET only.

Discovery: aggregate the global /trades tape (high-frequency wallets = bots) + leaderboard volume.
Profile each: reliable realized P&L (activity cash-flow SELL+REDEEM-BUY + open value), trade cadence,
median holding period, round-trip (scalp) rate, market-type mix, two-sidedness.
Classify edge + bucket borrowability:
  (a) SYSTEMATIC market-structure (scalp/short-hold/two-sided/non-model, profitable) <- the target
  (b) prediction MODEL (sports/event, holds to resolution) -- edge is the model, not borrowable
  (c) airdrop-farmer (~0 P&L, big OI, many mkts)
  (d) variance/survivorship (few resolved bets)
Run: python3 backtest/bot_hunt.py [--tape-pages N] [--lb N]
"""
import os
import sys
import time
import collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import curated_backtest as cb  # noqa: E402

DA = "https://data-api.polymarket.com"


def get(u):
    try:
        return cb.http_get(u)
    except Exception:  # noqa: BLE001
        return None


def mtype(q):
    q = q.lower()
    if any(k in q for k in ("war", "peace", "iran", "israel", "russia", "ukraine", "regime", "nuclear",
                            "ceasefire", "hormuz", "coup")):
        return "geo"
    if any(k in q for k in ("election", "president", "nominee", "primary", "senate", "governor", "parliament",
                            "impeach", "resign", "scotus")):
        return "politics"
    if any(k in q for k in ("bitcoin", "btc", "ethereum", " eth ", "solana", "crypto", "fdv", "token",
                            "airdrop", "coin", "price", "up or down")):
        return "crypto/macro"
    if any(k in q for k in ("fed ", "gdp", "cpi", "inflation", "rate", "temperature", "jobs")):
        return "crypto/macro"
    if any(k in q for k in (" vs", "win", "cup", "league", "nba", "nfl", "mlb", "score", "goal", "champion",
                            "playoff", "trophy", "final", "o/u", "match", "tournament")):
        return "sports"
    return "other"


def discover(tape_pages, lb_n):
    cnt = collections.Counter()
    mkts = collections.defaultdict(set)
    seen_tx = set()
    off, advancing = 0, True
    for _ in range(tape_pages):
        tr = get(f"{DA}/trades?limit=500&offset={off}")
        if not isinstance(tr, list) or not tr:
            break
        new = 0
        for t in tr:
            tx = t.get("transactionHash", "") + str(t.get("asset", "")) + str(t.get("timestamp", ""))
            if tx in seen_tx:
                continue
            seen_tx.add(tx)
            new += 1
            w = t.get("proxyWallet")
            cnt[w] += 1
            mkts[w].add(t.get("conditionId"))
        if new < 50:        # pagination not advancing (offset unsupported) -> stop
            break
        off += 500
        time.sleep(0.05)
    cands = {}
    for w, c in cnt.most_common(50):
        cands[w] = ("tape", c, len(mkts[w]))
    lb = get(f"{DA}/v1/leaderboard?window=30d&limit={lb_n}&orderBy=vol") or []
    for r in lb:
        cands.setdefault(r["proxyWallet"], ("lb", 0, 0))
    return cands, len(seen_tx)


def profile(w):
    buy = sell = redeem = 0.0
    ntr = nred = 0
    mkt = collections.Counter()
    conds = collections.defaultdict(list)
    first = last = None
    off = 0
    while off <= 3000:
        a = get(f"{DA}/activity?user={w}&limit=500&offset={off}")
        if not isinstance(a, list) or not a:
            break
        for r in a:
            t = r.get("type")
            usd = float(r.get("usdcSize", 0) or 0)
            ts = int(r.get("timestamp", 0) or 0)
            cond = r.get("conditionId")
            first = ts if first is None else min(first, ts)
            last = ts if last is None else max(last, ts)
            mkt[mtype(str(r.get("title", "")))] += usd
            if t == "TRADE":
                ntr += 1
                side = str(r.get("side"))
                conds[cond].append((ts, side))
                if side == "BUY":
                    buy += usd
                else:
                    sell += usd
            elif t in ("REDEEM", "REWARD", "CONVERSION"):
                redeem += usd
                if t == "REDEEM":
                    nred += 1
        if len(a) < 500:
            break
        off += 500
    pos = get(f"{DA}/positions?user={w}&limit=500") or []
    openval = sum(float(p.get("currentValue", 0) or 0) for p in pos)
    flowpnl = sell + redeem - buy + openval
    spans, rt = [], 0
    for cond, evs in conds.items():
        ts_ = [e[0] for e in evs]
        sides = set(e[1] for e in evs)
        spans.append(max(ts_) - min(ts_))
        if "BUY" in sides and "SELL" in sides:
            rt += 1
    spans.sort()
    medhold = spans[len(spans) // 2] / 3600.0 if spans else 0.0   # hours
    rtrate = rt / len(conds) if conds else 0.0
    span_days = (last - first) / 86400.0 if first and last else 0.0
    trday = ntr / span_days if span_days > 0 else 0.0
    topmkt = mkt.most_common(1)[0][0] if mkt else "?"
    return dict(flowpnl=flowpnl, ntr=ntr, nred=nred, nmkt=len(conds), medhold=medhold, rtrate=rtrate,
                trday=trday, topmkt=topmkt, openval=openval, capped=(off > 3000))


def classify(p):
    fp, oi, nm, nr = p["flowpnl"], p["openval"], p["nmkt"], p["nred"]
    # airdrop-farmer: large OI, many markets, ~flat realized relative to scale
    if oi > 100000 and nm > 80 and abs(fp) < 0.15 * max(oi, 1):
        return "(c) AIRDROP-FARMER"
    if nr < 8:
        return "(d) VARIANCE/survivorship"
    if fp <= 2000:
        return "unprofitable/flat"
    # profitable + meaningful sample:
    if p["medhold"] < 3 and p["rtrate"] > 0.45:
        return "(a) SCALP/MARKET-STRUCTURE"
    if p["topmkt"] == "sports":
        return "(b) SPORTS-MODEL"
    if p["topmkt"] in ("geo", "politics"):
        return "(b/d) EVENT-DIRECTIONAL"
    return "(b?) CRYPTO/MACRO-DIRECTIONAL"


def main():
    tp = int(sys.argv[sys.argv.index("--tape-pages") + 1]) if "--tape-pages" in sys.argv else 16
    lbn = int(sys.argv[sys.argv.index("--lb") + 1]) if "--lb" in sys.argv else 50
    cands, ntape = discover(tp, lbn)
    print(f"# discovered {len(cands)} candidate bots ({ntape} tape trades aggregated)\n", flush=True)
    print(f"# {'wallet':12s} {'flowPnl$':>11s} {'bets':>5s} {'redeem':>6s} {'tr/day':>7s} "
          f"{'medHold_h':>9s} {'scalp%':>6s} {'top':>11s} {'src':>4s}  class")
    rows = []
    for w, (src, c, m) in cands.items():
        p = profile(w)
        cls = classify(p)
        rows.append((w, p, cls, src))
        print(f"  {w[:12]} {p['flowpnl']:11.0f} {p['nmkt']:5d} {p['nred']:6d} {p['trday']:7.0f} "
              f"{p['medhold']:9.1f} {p['rtrate']*100:5.0f}% {p['topmkt']:>11s} {src:>4s}  {cls}", flush=True)
        time.sleep(0.03)

    print("\n# === BORROWABILITY SUMMARY ===")
    agg = collections.defaultdict(lambda: [0, 0.0])
    for (w, p, cls, src) in rows:
        agg[cls][0] += 1
        agg[cls][1] += p["flowpnl"]
    for cls, (n, tot) in sorted(agg.items(), key=lambda x: -x[1][0]):
        print(f"#  {cls:34s} count={n:3d} total_flowPnl=${tot:12.0f}")
    print("\n# === PROFITABLE SCALP/MARKET-STRUCTURE bots (bucket a — the borrowable target) ===")
    a = [(w, p) for (w, p, cls, src) in rows if cls.startswith("(a)")]
    if not a:
        print("#  NONE — no profitable short-hold two-sided market-structure bot found")
    for w, p in sorted(a, key=lambda x: -x[1]["flowpnl"]):
        print(f"#  {w} flowPnl=${p['flowpnl']:.0f} medHold={p['medhold']:.1f}h scalp={p['rtrate']*100:.0f}% "
              f"top={p['topmkt']} bets={p['nmkt']} redeem={p['nred']}")


if __name__ == "__main__":
    main()
