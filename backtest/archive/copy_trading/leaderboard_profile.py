#!/usr/bin/env python3
"""Profile + classify the top Polymarket leaderboard accounts into strategy archetypes.
ZERO real money — read-only public-API GET only (data-api leaderboard/value/positions/trades), no
orders. Answers: what are the leaderboard 'bots' actually doing, at what scale, and at what P&L?

Archetypes (by directly-measured signals, not the noisy board pnl):
  OI-FARMER     : big open interest ($) across many markets, low reward-mkt overlap -> $POLY OI farming
  REWARD-MAKER  : positions concentrated in active liquidity-reward markets -> our bot's game
  THIN-FLOW     : holds ~nothing ($ value ~0) but huge volume -> pass-through/volume churn
  TWO-SIDED-HFT : mid OI, many markets, balanced buy/sell, high cadence -> classic MM
  DIRECTIONAL   : concentrated, one-sided, non-trivial board pnl
"""
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import curated_backtest as cb  # noqa: E402

DA = "https://data-api.polymarket.com"


def get(u):
    try:
        return cb.http_get(u)
    except Exception:  # noqa: BLE001
        return None


def classify(val, npos, rew_frac, bs_bal, vol):
    if val < 8000 and vol > 1e6:
        return "THIN-FLOW"
    if rew_frac >= 0.35:
        return "REWARD-MAKER"
    if val >= 100000 and npos >= 100:
        return "OI-FARMER"
    if npos >= 60 and 0.25 <= bs_bal <= 0.75:
        return "TWO-SIDED-HFT"
    if npos <= 30:
        return "DIRECTIONAL/CONCENTRATED"
    return "MIXED"


def main():
    n = int(sys.argv[sys.argv.index("--n") + 1]) if "--n" in sys.argv else 50
    raw = cb.fetch_reward_pools()
    rconds = set(m.get("condition_id") for m in raw)
    lb = get(f"{DA}/v1/leaderboard?window=30d&limit={n}&orderBy=vol") or []
    print(f"# profiling {len(lb)} top-volume accounts (30d). reward-mkt universe={len(rconds)}\n")
    print(f"# {'user':18s} {'vol30d$':>11s} {'pnl$':>9s} {'OI$':>10s} {'npos':>5s} {'rew%':>4s} "
          f"{'b/s':>7s} {'tr/day':>7s}  archetype")
    import collections
    agg = collections.defaultdict(lambda: [0, 0.0, 0.0])  # count, sum_OI, sum_vol
    for r in lb:
        w = r["proxyWallet"]
        nm = str(r.get("userName", ""))[:18]
        vol = float(r.get("vol", 0) or 0)
        pnl = float(r.get("pnl", 0) or 0)
        v = get(f"{DA}/value?user={w}")
        val = float(v[0]["value"]) if v else 0.0
        pos = get(f"{DA}/positions?user={w}&limit=500") or []
        npos = len(pos)
        inrew = sum(1 for p in pos if p.get("conditionId") in rconds)
        rew_frac = inrew / npos if npos else 0.0
        tr = get(f"{DA}/trades?user={w}&limit=300")
        if isinstance(tr, list) and tr:
            b = sum(1 for t in tr if str(t.get("side")) == "BUY")
            bs_bal = b / len(tr)
            span = max(1.0, tr[0]["timestamp"] - tr[-1]["timestamp"])
            trday = len(tr) / (span / 86400.0)
        else:
            bs_bal, trday = 0.5, 0.0
        arch = classify(val, npos, rew_frac, bs_bal, vol)
        agg[arch][0] += 1
        agg[arch][1] += val
        agg[arch][2] += vol
        print(f"  {nm:18s} {vol:11.0f} {pnl:9.2f} {val:10.0f} {npos:5d} {rew_frac*100:4.0f} "
              f"{b if isinstance(tr,list) and tr else 0:3d}/{(len(tr)-b) if isinstance(tr,list) and tr else 0:<3d} "
              f"{trday:7.0f}  {arch}")
    print("\n# === archetype mix (top-volume population) ===")
    for a, (c, soi, svol) in sorted(agg.items(), key=lambda x: -x[1][0]):
        print(f"#  {a:26s} count={c:3d}  total_OI=${soi:11.0f}  total_vol30d=${svol:13.0f}")


if __name__ == "__main__":
    main()
