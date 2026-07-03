#!/usr/bin/env python3
"""'Short the fat-tail' on dramatic-unlikely GEOPOLITICAL/POLITICAL event markets — rigorous test.
ZERO real money — read-only public-API GET, no orders.

Selling No-insurance: buy No at 0.85-0.95 on "will [war/ceasefire/regime-change/coup/treaty/
assassination/resign-by-deadline] happen", collect the premium when it doesn't. The geo winners
(ranger44 +$246k Israel-Iran-peace=No@0.93, AngryRhino, Spirit-of-Uk +$1.07M Hormuz=No) lived here.
Prior mixed-category backtest gave de-confounded edge ~0; the geo subset hinted +5.8c on only 8 mkts.
Zoom into geo specifically with a much bigger sample.

Method: scan many gamma closed pages CHEAPLY, filter to geo-dramatic FIRST, then pull /trades only for
those (affordable bigger sample). De-confounded entry at fixed lead (7/14/30d). Calibration + selection
features (event type, deadline length, 'permanent'/conjunctive specificity) + correlated-tail MC.
Run: python3 backtest/geo_fattail.py [--pages N] [--minvol V] [--analyze-only]
"""
import os
import sys
import json
import time
import math
import random

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import curated_backtest as cb  # noqa: E402

random.seed(42)
GAMMA = "https://gamma-api.polymarket.com"
DA = "https://data-api.polymarket.com"
DATAFILE = "/tmp/geo_data.json"
LEADS = [7, 14, 30]

GEO_KW = ("war", "ceasefire", "cease-fire", "peace", "treaty", "regime", "coup", "invade", "invasion",
          "strike", "missile", "nuclear", "nuke", "assassinat", "capture", "resign", "impeach",
          "step down", "leave office", "sanction", "annex", "occupy", "troops", "martial law",
          "overthrow", "depose", "exile", "iran", "israel", "russia", "ukraine", "gaza", "hamas",
          "hezbollah", "north korea", "taiwan", "venezuela", "hormuz", "nato", "putin", "zelensky",
          "netanyahu", "khamenei", "maduro", "kim jong", "hostage", "nuke", "deal by", "shut down")


def get(u):
    try:
        return cb.http_get(u)
    except Exception:  # noqa: BLE001
        return None


def is_geo_dramatic(q):
    ql = q.lower()
    if not any(k in ql for k in GEO_KW):
        return False
    return (ql.startswith("will ") or "by " in ql or "before " in ql
            or any(k in ql for k in ("ceasefire", "peace", "regime", "coup", "resign", "invade",
                                     "strike", "nuclear", "war")))


def event_type(ql):
    if any(k in ql for k in ("ceasefire", "peace", "treaty", "deal")):
        return "peace/ceasefire"
    if any(k in ql for k in ("regime", "coup", "overthrow", "depose", "fall", "step down", "resign",
                             "impeach", "leave office", "exile")):
        return "regime/leave-office"
    if any(k in ql for k in ("nuclear", "nuke", "missile", "strike", "invade", "invasion", "war", "attack")):
        return "war/strike"
    if any(k in ql for k in ("assassinat", "capture", "hostage", "arrest")):
        return "assassinate/capture"
    return "other-geo"


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
                q = x.get("question", "")
                if not is_geo_dramatic(q):
                    continue
                op = json.loads(x.get("outcomePrices") or "[]")
                vol = float(x.get("volume") or 0)
                cond = x.get("conditionId")
                if len(op) != 2 or vol < minvol or not cond:
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
                last_ts, first_ts = trades[-1][0], trades[0][0]
                allp = sorted(p for _, p, _ in trades)
                naive = allp[len(allp) // 2]
                lead = {}
                for T in LEADS:
                    tgt = last_ts - T * 86400
                    win = [(p, s) for ts, p, s in trades if abs(ts - tgt) <= 2 * 86400]
                    if win:
                        tot = sum(s for _, s in win) or 1.0
                        lead[str(T)] = sum(p * s for p, s in win) / tot
                ql = q.lower()
                life = (last_ts - first_ts) / 86400.0
                feat = dict(permanent=("permanent" in ql), conj=(" and " in ql or " & " in ql),
                            short=(life < 30), life=round(life, 1), etype=event_type(ql))
                recs.append(dict(cond=cond, q=q[:72], vol=vol, yes=1 if yes_res else 0,
                                 naive=naive, lead=lead, feat=feat))
            except Exception:  # noqa: BLE001
                continue
        time.sleep(0.1)
    json.dump(recs, open(DATAFILE, "w"))
    return recs


def wilson(k, n, z=1.96):
    if n == 0:
        return (0.0, 0.0, 1.0)
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return (p, max(0.0, c - h), min(1.0, c + h))


def fade_region(recs, lead, lo=0.03, hi=0.20):
    """markets where the de-confounded Yes-price sits in the No-selling region (Yes lo..hi)."""
    out = []
    for r in recs:
        p = r.get("lead", {}).get(lead)
        if p is not None and lo <= p < hi:
            out.append((p, r["yes"], r))
    return out


def main():
    pages = int(sys.argv[sys.argv.index("--pages") + 1]) if "--pages" in sys.argv else 30
    minvol = float(sys.argv[sys.argv.index("--minvol") + 1]) if "--minvol" in sys.argv else 5000.0
    if "--analyze-only" in sys.argv and os.path.exists(DATAFILE):
        recs = json.load(open(DATAFILE))
    else:
        recs = collect(pages, minvol, 1200)
    print(f"# {len(recs)} resolved GEO-dramatic markets (vol>=${minvol:.0f}); saved {DATAFILE}\n", flush=True)

    # 1) de-confounded No-edge in the fade region (Yes 0.03-0.20) at each lead, + significance
    for lead in ["naive", "7", "14", "30"]:
        if lead == "naive":
            fr = [(r["naive"], r["yes"], r) for r in recs if 0.03 <= r["naive"] < 0.20]
        else:
            fr = fade_region(recs, lead)
        n = len(fr)
        if n == 0:
            print(f"# lead {lead}: 0 markets in fade region"); continue
        mp = sum(p for p, _, _ in fr) / n
        hits = sum(y for _, y, _ in fr)
        h = hits / n
        hp, hlo, hhi = wilson(hits, n)            # CI on the hit rate
        edge = mp - h                              # No-edge per share (mid)
        edge_lo, edge_hi = mp - hhi, mp - hlo      # edge CI (flip the h CI)
        lab = "NAIVE(confounded)" if lead == "naive" else f"LEAD {lead}d(de-conf)"
        print(f"# {lab:18s} fade-region n={n:3d} mean_p={mp:.3f} hits={hits} hit_rate={h:.3f} "
              f"(95%CI {hlo:.3f}-{hhi:.3f}) -> No-edge={edge:+.3f} (95%CI {edge_lo:+.3f}..{edge_hi:+.3f})")

    # 2) SELECTION: hit rate by feature (use lead 14d fade region)
    fr = fade_region(recs, "14")
    print(f"\n# === SELECTION features (de-conf 14d, fade region, n={len(fr)}) — does a rule separate hits? ===")
    def grp(name, pred):
        sub = [(p, y) for (p, y, r) in fr if pred(r)]
        if not sub:
            return
        k = sum(y for _, y in sub); n = len(sub)
        p, lo, hi = wilson(k, n)
        print(f"#   {name:32s} n={n:3d} hit_rate={k/n:.3f} (95%CI {lo:.3f}-{hi:.3f}) "
              f"mean_p={sum(x for x,_ in sub)/n:.3f}")
    grp("permanent/treaty wording", lambda r: r["feat"]["permanent"])
    grp("conjunctive (AND)", lambda r: r["feat"]["conj"])
    grp("short deadline (<30d life)", lambda r: r["feat"]["short"])
    grp("long horizon (>=30d life)", lambda r: not r["feat"]["short"])
    for et in ("peace/ceasefire", "regime/leave-office", "war/strike", "assassinate/capture", "other-geo"):
        grp(f"event={et}", lambda r, et=et: r["feat"]["etype"] == et)

    # 3) CORRELATED-TAIL MC: No-selling book, geo events cluster
    fr = fade_region(recs, "14")
    if len(fr) < 15:
        print("\n# too few geo fade-region markets for a robust MC");
    ps = [p for p, _, _ in fr]
    h_emp = sum(y for _, y, _ in fr) / max(len(fr), 1)
    mp = sum(ps) / max(len(ps), 1)
    # use a CONSERVATIVE hit rate = upper end of the CI (don't under-estimate the tail on a small sample)
    _, _, h_hi = wilson(int(h_emp * len(fr)), len(fr))
    print(f"\n# === CORRELATED-TAIL MC (geo No-selling book) ===")
    print(f"#   fade-region mean_p={mp:.3f}, empirical hit={h_emp:.3f}, CONSERVATIVE hit(95%hi)={h_hi:.3f}")
    for h_use, lbl in ((h_emp, "empirical-h"), (h_hi, "conservative-h")):
        print(f"#   -- using {lbl}={h_use:.3f} --")
        print(f"#     {'N':>4s} {'rho':>4s} {'mean%':>7s} {'P5%':>7s} {'P1%':>7s} {'worst%':>7s} "
              f"{'P(<0)':>7s} {'P(<-25%)':>9s}")
        for N in (20, 40):
            for rho in (0.3, 0.6):
                rets = []
                for _ in range(8000):
                    fac = random.gauss(0, 1)
                    cost = pay = 0.0
                    for _ in range(N):
                        p = random.choice(ps)
                        q = 1 - p
                        z = math.sqrt(rho) * fac + math.sqrt(1 - rho) * random.gauss(0, 1)
                        hit = (0.5 * (1 + math.erf(z / math.sqrt(2)))) < h_use
                        cost += q
                        pay += 0.0 if hit else 1.0
                    rets.append((pay - cost) / cost * 100)
                rets.sort()
                mean = sum(rets) / len(rets)
                p5 = rets[int(0.05 * len(rets))]; p1 = rets[int(0.01 * len(rets))]
                ploss = sum(1 for x in rets if x < 0) / len(rets)
                pbig = sum(1 for x in rets if x < -25) / len(rets)
                print(f"#     {N:4d} {rho:4.1f} {mean:7.1f} {p5:7.1f} {p1:7.1f} {rets[0]:7.1f} "
                      f"{ploss*100:6.0f}% {pbig*100:8.0f}%")


if __name__ == "__main__":
    main()
