#!/usr/bin/env python3
"""Rigorous longshot-fade backtest. ZERO real money — read-only public-API GET, no orders.

The naive +7.1c No-edge is CONFOUNDED: markets resolving No drift toward 0 near settlement, so their
median-trade price falls INTO the longshot bucket -> over-counts No-resolvers in low buckets -> inflates
the edge. Fix: anchor the entry price at a FIXED LEAD TIME before the last trade (7/14/30 days), an
unbiased ex-ante probability snapshot, and match to the eventual outcome. Compare de-confounded edge vs
naive, stratified by category.

Stage 1 (this file): collect resolved binary markets + lead-time entry prices + outcome + category,
SAVE to JSON (so the Monte-Carlo/sizing stage reads it without re-hitting the API).
Stage 2 (this file): de-confounded calibration per lead/bucket/category.
Run: python3 backtest/longshot_rigorous.py [--pages N] [--minvol V] [--max M]
"""
import os
import sys
import json
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import curated_backtest as cb  # noqa: E402

GAMMA = "https://gamma-api.polymarket.com"
DA = "https://data-api.polymarket.com"
DATAFILE = "/tmp/longshot_data.json"
LEADS = [7, 14, 30]
EDGES = [0.0, 0.03, 0.07, 0.12, 0.20, 0.35, 0.50, 0.65, 0.80, 0.93, 1.0]


def get(u):
    try:
        return cb.http_get(u)
    except Exception:  # noqa: BLE001
        return None


def category(q):
    q = q.lower()
    if any(k in q for k in ("war", "peace", "nuclear", "nuke", "missile", "strike", "iran", "israel",
                            "russia", "ukraine", "hormuz", "regime", "ceasefire", "invade", "troops",
                            "sanction", "gaza", "hezbollah", "hamas", "attack")):
        return "geopolitics"
    if any(k in q for k in ("election", "president", "nominee", "primary", "senate", "governor",
                            "parliament", "prime minister", "chancellor", "impeach", "resign", "cabinet",
                            "coalition", "mayor", "supreme court", "scotus")):
        return "politics"
    if any(k in q for k in ("bitcoin", "btc", "ethereum", " eth ", "solana", "crypto", "fdv", "token",
                            "airdrop", "coin", "$")):
        return "crypto"
    if any(k in q for k in ("fed ", "gdp", "cpi", "inflation", "jobs", "rate cut", "rate hike",
                            "up or down", "temperature", "recession")):
        return "macro"
    if any(k in q for k in (" vs", "win the", "cup", "league", "nba", "nfl", "mlb", "score", "goal",
                            "champion", "playoff", "trophy", "final", "o/u", "match", "tournament", "win on")):
        return "sports"
    if any(k in q for k in ("movie", "oscar", "album", "box office", "grammy", "rotten", "spotify")):
        return "entertainment"
    return "other"


def collect(pages, minvol, maxm):
    recs = []
    for pg in range(pages):
        ms = get(f"{GAMMA}/markets?closed=true&limit=500&offset={pg*500}&order=endDate&ascending=false")
        if not isinstance(ms, list) or not ms:
            break
        for x in ms:
            if len(recs) >= maxm:
                break
            try:
                op = json.loads(x.get("outcomePrices") or "[]")
                if len(op) != 2:
                    continue
                vol = float(x.get("volume") or 0)
                cond = x.get("conditionId")
                if vol < minvol or not cond:
                    continue
                yes_res = float(op[0]) > 0.5
                trades, off = [], 0
                while off <= 3000:
                    tr = get(f"{DA}/trades?market={cond}&limit=500&offset={off}")
                    if not isinstance(tr, list) or not tr:
                        break
                    for t in tr:
                        ts = int(t.get("timestamp", 0) or 0)
                        pr = float(t.get("price", 0) or 0)
                        sz = float(t.get("size", 0) or 0)
                        if pr <= 0 or pr >= 1 or sz <= 0:
                            continue
                        py = pr if t.get("outcomeIndex", 0) == 0 else 1 - pr
                        trades.append((ts, py, sz))
                    if len(tr) < 500:
                        break
                    off += 500
                if len(trades) < 5:
                    continue
                trades.sort()
                last_ts = trades[-1][0]
                allp = sorted(p for _, p, _ in trades)
                naive = allp[len(allp) // 2]
                lead = {}
                for T in LEADS:
                    tgt = last_ts - T * 86400
                    win = [(p, s) for ts, p, s in trades if abs(ts - tgt) <= 2 * 86400]
                    if win:
                        tot = sum(s for _, s in win) or 1.0
                        lead[str(T)] = sum(p * s for p, s in win) / tot
                recs.append(dict(cond=cond, q=str(x.get("question", ""))[:60], cat=category(x.get("question", "")),
                                 vol=vol, yes=1 if yes_res else 0, naive=naive, lead=lead, ntr=len(trades)))
            except Exception:  # noqa: BLE001
                continue
        if len(recs) >= maxm:
            break
        time.sleep(0.1)
    json.dump(recs, open(DATAFILE, "w"))
    return recs


def calib(recs, key):
    """key='naive' or a lead like '7'. Returns bucket stats using that price anchor."""
    pts = []
    for r in recs:
        p = r["naive"] if key == "naive" else r.get("lead", {}).get(key)
        if p is None:
            continue
        pts.append((p, r["yes"]))
    rows = []
    for i in range(len(EDGES) - 1):
        b = [(p, y) for (p, y) in pts if EDGES[i] <= p < EDGES[i + 1]]
        if not b:
            continue
        mp = sum(p for p, _ in b) / len(b)
        r = sum(y for _, y in b) / len(b)
        rows.append((EDGES[i], EDGES[i + 1], len(b), mp, r, mp - r))
    return rows, len(pts)


def longshot_edge(rows, hi=0.35):
    w = e = 0.0
    for (lo, h2, n, mp, r, edge) in rows:
        if h2 <= hi:
            w += n
            e += edge * n
    return (e / w if w else 0.0), int(w)


def main():
    pages = int(sys.argv[sys.argv.index("--pages") + 1]) if "--pages" in sys.argv else 8
    minvol = float(sys.argv[sys.argv.index("--minvol") + 1]) if "--minvol" in sys.argv else 10000.0
    maxm = int(sys.argv[sys.argv.index("--max") + 1]) if "--max" in sys.argv else 600
    if "--analyze-only" in sys.argv and os.path.exists(DATAFILE):
        recs = json.load(open(DATAFILE))
    else:
        recs = collect(pages, minvol, maxm)
    print(f"# collected {len(recs)} resolved binary markets (vol>=${minvol:.0f}); saved {DATAFILE}\n", flush=True)

    for key in ["naive", "7", "14", "30"]:
        rows, n = calib(recs, key)
        avg, w = longshot_edge(rows)
        lab = "NAIVE (near-settlement, confounded)" if key == "naive" else f"LEAD {key}d (de-confounded)"
        print(f"# === {lab} | priced {n} markets | longshot(<0.35) No-edge = {avg:+.3f} over {w} mkts ===")
        print(f"#   {'bucket':12s} {'n':>4s} {'mean_p':>7s} {'yes_rate':>9s} {'No-edge(p-r)':>13s}")
        for (lo, hi, nn, mp, r, edge) in rows:
            star = " ***" if (hi <= 0.35 and edge > 0.01) else ""
            print(f"    [{lo:.2f},{hi:.2f}) {nn:5d} {mp:7.3f} {r:9.3f} {edge:+13.3f}{star}")
        print()

    # category stratification at lead 14d (de-confounded)
    print("# === de-confounded (lead 14d) LONGSHOT No-edge by CATEGORY ===")
    cats = {}
    for r in recs:
        cats.setdefault(r["cat"], []).append(r)
    for c in sorted(cats, key=lambda k: -len(cats[k])):
        rows, n = calib(cats[c], "14")
        avg, w = longshot_edge(rows)
        # also report the true longshot hit-rate (yes_rate) in the <0.20 region
        ls = [(r["lead"].get("14")) for r in cats[c]
              if r["lead"].get("14") is not None and r["lead"]["14"] < 0.20]
        hits = sum(1 for r in cats[c] if r["lead"].get("14") is not None and r["lead"]["14"] < 0.20 and r["yes"])
        hr = hits / len(ls) if ls else 0.0
        print(f"#   {c:14s} mkts={len(cats[c]):4d} longshot_No-edge={avg:+.3f} (n={w:3d})  "
              f"sub0.20_hitrate={hr:.3f} (n={len(ls)})")


if __name__ == "__main__":
    main()
