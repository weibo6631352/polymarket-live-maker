#!/usr/bin/env python3
"""Test the favorite-longshot bias on Polymarket = is fading longshots (buy No on unlikely Yes) a
real systematic edge? ZERO real money — read-only public-API GET, no orders.

Method: pull RESOLVED binary markets (gamma closed=true -> outcomePrices give the resolution), take a
representative ACTIVE trading price for Yes (median of /prices-history over the market life), bucket
markets by that price, and compare bucket mean-price p vs realized Yes-rate r:
   No-edge per $1 (buying No at 1-p) = p - r   [>0 means Yes overpriced -> fading longshots pays]
A clean favorite-longshot bias shows p - r > 0 for LOW-p (longshot) buckets. Reports the calibration
curve + the realized edge + tail (how often longshots actually hit = the loss events).
Caveat: median-over-life price drifts toward the outcome, so this is a gross-bias check, not a
tradeable-after-costs proof; the sign + magnitude of p-r in low buckets is the signal.

Run: python3 backtest/calibration_longshot.py [--pages N] [--minvol V]
"""
import os
import sys
import json
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import curated_backtest as cb  # noqa: E402

GAMMA = "https://gamma-api.polymarket.com"
CLOB = "https://clob.polymarket.com"


def get(u):
    try:
        return cb.http_get(u)
    except Exception:  # noqa: BLE001
        return None


def resolved_markets(pages, minvol):
    out = []
    for pg in range(pages):
        m = get(f"{GAMMA}/markets?closed=true&limit=500&offset={pg*500}&order=endDate&ascending=false")
        if not isinstance(m, list) or not m:
            break
        for x in m:
            try:
                op = json.loads(x.get("outcomePrices") or "[]")
                tk = json.loads(x.get("clobTokenIds") or "[]")
                vol = float(x.get("volume") or 0)
                if len(op) != 2 or len(tk) != 2 or vol < minvol:
                    continue
                yes_res = float(op[0]) > 0.5            # outcomePrices[0] = Yes settle (1 or 0)
                out.append((x.get("conditionId"), yes_res, vol, str(x.get("question", ""))[:40]))
            except Exception:  # noqa: BLE001
                continue
        time.sleep(0.1)
    return out


DA = "https://data-api.polymarket.com"


def life_price(cond):
    # representative Yes price from real trades (prices-history is empty for closed markets).
    # median over the OLDEST available trades (paginate toward the tail) = ex-ante-ish, less
    # confounded by the resolution drift than the most-recent trades.
    last = None
    off = 0
    while off <= 2500:
        tr = get(f"{DA}/trades?market={cond}&limit=500&offset={off}")
        if not isinstance(tr, list) or not tr:
            break
        last = tr
        if len(tr) < 500:
            break
        off += 500
    if not last:
        return None
    ps = sorted(float(t["price"]) for t in last
                if t.get("outcomeIndex", 0) == 0 and 0.0 < float(t.get("price", 0)) < 1.0)
    return ps[len(ps) // 2] if len(ps) >= 3 else None


def main():
    pages = int(sys.argv[sys.argv.index("--pages") + 1]) if "--pages" in sys.argv else 3
    minvol = float(sys.argv[sys.argv.index("--minvol") + 1]) if "--minvol" in sys.argv else 20000.0
    mk = resolved_markets(pages, minvol)
    print(f"# {len(mk)} resolved binary markets (vol>=${minvol:.0f})", flush=True)
    # buckets by Yes price
    edges = [0.0, 0.03, 0.07, 0.12, 0.20, 0.35, 0.50, 0.65, 0.80, 0.88, 0.93, 0.97, 1.0]
    buckets = {i: [] for i in range(len(edges) - 1)}
    n = 0
    for (tok, yes_res, vol, q) in mk:
        p = life_price(tok)
        if p is None:
            continue
        n += 1
        for i in range(len(edges) - 1):
            if edges[i] <= p < edges[i + 1]:
                buckets[i].append((p, 1.0 if yes_res else 0.0, vol))
                break
        time.sleep(0.03)
    print(f"# priced {n} markets\n")
    print(f"# {'Yes-price bucket':18s} {'n':>4s} {'mean_p':>7s} {'yes_rate':>9s} {'p-r(No edge)':>13s} "
          f"{'<- fade-longshot pays if >0'}")
    tot_lowbucket_edge_w = 0.0
    tot_lowbucket_w = 0.0
    for i in range(len(edges) - 1):
        b = buckets[i]
        if not b:
            continue
        mp = sum(x[0] for x in b) / len(b)
        r = sum(x[1] for x in b) / len(b)
        edge = mp - r
        lab = f"[{edges[i]:.2f},{edges[i+1]:.2f})"
        star = " ***" if (edges[i+1] <= 0.35 and edge > 0.01) else ""
        print(f"  {lab:18s} {len(b):4d} {mp:7.3f} {r:9.3f} {edge:+13.3f}{star}")
        if edges[i + 1] <= 0.35:  # longshot region
            tot_lowbucket_edge_w += edge * len(b)
            tot_lowbucket_w += len(b)
    print()
    if tot_lowbucket_w:
        avg = tot_lowbucket_edge_w / tot_lowbucket_w
        print(f"# LONGSHOT region (Yes price < 0.35): avg No-edge (p-r) = {avg:+.3f} per $1 over "
              f"{int(tot_lowbucket_w)} markets")
        print(f"# interpretation: buying No on sub-0.35 Yes returned ~{avg*100:+.1f}c per $1 of Yes-overpricing"
              f" (BEFORE the tail: those longshots that DID hit cost the full No stake)")


if __name__ == "__main__":
    main()
