#!/usr/bin/env python3
"""Stage 3: realized backtest + Monte-Carlo fat-tail + sizing for longshot-fade.
ZERO real money — reads /tmp/longshot_data.json (from longshot_rigorous.py), no API, no orders.

Per longshot No-position (Yes price p at the de-confounded lead): buy 1 share No, cost q=1-p; win +p
w.p. (1-h), lose -q w.p. h (true hit). Edge/share = p-h. Risk/reward is ~9:1 against (a p=0.10 bet
risks 0.90 to make 0.10) so it lives or dies on (a) the de-confounded edge p-h and (b) clustered hits.

Outputs: historical realized return (incl. real hits), per-category de-confounded edge + hit-rate,
Monte-Carlo of a diversified book under correlation scenarios (geopolitical longshots cluster),
blow-up probability + drawdown, and the Kelly/per-position cap that survives.
Run: python3 backtest/longshot_mc.py [--lead 14] [--thresh 0.20]
"""
import json
import math
import random
import sys

random.seed(42)
DATAFILE = "/tmp/longshot_data.json"


def Phi(x):
    return 0.5 * (1.0 + math.erf(x / math.sqrt(2.0)))


def main():
    lead = sys.argv[sys.argv.index("--lead") + 1] if "--lead" in sys.argv else "14"
    thr = float(sys.argv[sys.argv.index("--thresh") + 1]) if "--thresh" in sys.argv else 0.20
    D = json.load(open(DATAFILE))
    uni = [r for r in D if r.get("lead", {}).get(lead) is not None and 0.005 < r["lead"][lead] < thr]
    print(f"# longshot universe: lead {lead}d Yes-price in (0.005,{thr}) -> {len(uni)} resolved markets\n")
    if len(uni) < 20:
        print("# too few markets; widen thresh or collect more"); return

    # 1) HISTORICAL REALIZED (buy 1 share No on each at de-confounded entry) ----------------------
    def realized(rows):
        cost = sum(1 - r["lead"][lead] for r in rows)        # No cost = 1-p
        wins = [r for r in rows if r["yes"] == 0]
        hits = [r for r in rows if r["yes"] == 1]
        payoff = len(wins) * 1.0
        net = payoff - cost
        return net, cost, len(hits), len(rows)
    net, cost, nhit, ntot = realized(uni)
    print(f"# === HISTORICAL REALIZED (equal 1-share No each, de-confounded entry) ===")
    print(f"#   {ntot} positions, capital deployed ${cost:.0f}, net ${net:+.0f} "
          f"-> return {net/cost*100:+.1f}% over the book; longshot HITS={nhit} ({nhit/ntot*100:.1f}% hit rate)")

    # 2) per-category de-confounded edge + hit rate ----------------------------------------------
    print(f"\n# === per-category (de-confounded lead {lead}d) ===")
    cats = {}
    for r in uni:
        cats.setdefault(r["cat"], []).append(r)
    cat_stats = {}
    for c in sorted(cats, key=lambda k: -len(cats[k])):
        rows = cats[c]
        mp = sum(r["lead"][lead] for r in rows) / len(rows)
        h = sum(r["yes"] for r in rows) / len(rows)         # realized hit rate = empirical "true" h
        net, cost, nhit, ntot = realized(rows)
        cat_stats[c] = (mp, h, len(rows))
        print(f"#   {c:14s} n={len(rows):4d} mean_p={mp:.3f} hit_rate(h)={h:.3f} edge(p-h)={mp-h:+.3f} "
              f"realized_return={net/cost*100:+.1f}%")

    # 3) MONTE-CARLO diversified book with correlation (Gaussian copula) --------------------------
    # Each position: category c -> hit prob h_c; entry p_c=mean_p. Within-category latent correlation
    # rho (geopolitical/macro longshots cluster; sports nearly independent). Book = N positions drawn
    # from the universe's category mix, equal 1-share. Report return distribution + blow-up.
    rho_by_cat = {"geopolitics": 0.45, "politics": 0.30, "macro": 0.35, "crypto": 0.25,
                  "sports": 0.05, "entertainment": 0.15, "other": 0.20}
    mix = []
    for c, rows in cats.items():
        mix += [c] * len(rows)
    print(f"\n# === MONTE-CARLO book (10k paths), positions drawn from universe category mix ===")
    print(f"#   {'N_pos':>5s} {'mean_ret%':>9s} {'std%':>7s} {'P5_ret%':>8s} {'P1_ret%':>8s} "
          f"{'worst%':>7s} {'P(book<0)':>9s} {'P(loss>25%)':>11s}")
    for N in (25, 50, 100, 200):
        rets = []
        for _ in range(10000):
            # common factors per category (shared shock), plus idiosyncratic
            fac = {c: random.gauss(0, 1) for c in cats}
            cost = pay = 0.0
            for _ in range(N):
                c = random.choice(mix)
                mp, h, _n = cat_stats[c]
                q = 1 - mp
                rho = rho_by_cat.get(c, 0.2)
                z = math.sqrt(rho) * fac[c] + math.sqrt(1 - rho) * random.gauss(0, 1)
                hit = Phi(z) < h            # latent below threshold -> longshot hits (loss)
                cost += q
                pay += 0.0 if hit else 1.0
            rets.append((pay - cost) / cost * 100.0)
        rets.sort()
        mean = sum(rets) / len(rets)
        std = (sum((x - mean) ** 2 for x in rets) / len(rets)) ** 0.5
        p5 = rets[int(0.05 * len(rets))]
        p1 = rets[int(0.01 * len(rets))]
        worst = rets[0]
        ploss = sum(1 for x in rets if x < 0) / len(rets)
        pbig = sum(1 for x in rets if x < -25) / len(rets)
        print(f"#   {N:5d} {mean:9.1f} {std:7.1f} {p5:8.1f} {p1:8.1f} {worst:7.1f} "
              f"{ploss*100:8.1f}% {pbig*100:10.1f}%")

    # 4) SIZING — Kelly + per-position cap ------------------------------------------------------
    # aggregate edge per $ of No-cost
    mp_all = sum(r["lead"][lead] for r in uni) / len(uni)
    h_all = sum(r["yes"] for r in uni) / len(uni)
    q_all = 1 - mp_all
    edge_share = mp_all - h_all                      # per No-share
    ret_on_cost = edge_share / q_all                 # per $ deployed, per resolution cycle
    # Kelly for the binary No bet: win prob w=1-h, net-odds b = p/q (win p per q staked)
    w = 1 - h_all
    b = mp_all / q_all
    kelly = (w * b - (1 - w)) / b if b > 0 else 0.0
    print(f"\n# === SIZING ===")
    print(f"#   aggregate: mean_p={mp_all:.3f} hit h={h_all:.3f} edge/share={edge_share:+.3f} "
          f"return-on-capital/cycle={ret_on_cost*100:+.1f}%")
    print(f"#   full-Kelly fraction per bet={kelly:+.3f} (NEGATIVE => no edge => DO NOT BET); "
          f"fractional-Kelly + per-position cap advised given the fat tail")


if __name__ == "__main__":
    main()
