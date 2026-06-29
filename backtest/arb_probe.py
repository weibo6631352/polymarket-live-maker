#!/usr/bin/env python3
"""Cross-market consistency-arbitrage probe. ZERO real money — read-only public-API GET only, no
orders, no signing, PM_TRADER_LIVE untouched. Tests for a STRUCTURAL (non-predictive) edge:

  (1) Multi-outcome groups (neg_risk): Sum(best_ask) over all outcomes < 1  -> buy-all guarantees $1
      for <$1; Sum(best_bid) > 1 -> mint a complete set ($1) and sell-all for >$1. Only valid when the
      group is EXHAUSTIVE (exactly one outcome resolves YES) -> gate on Sum(mid) ~ 1.
  (2) Binary YES_ask + NO_ask < 1 (buy both -> guaranteed $1); YES_bid + NO_bid > 1.
  (3) Conditional ladder: P(A) <= P(B) must hold when A implies B (win <= reach-final <= reach-semi).

For every candidate edge: EXECUTABLE size by walking real /book depth (not just top-of-book), net under
fee scenarios (PM taker fee 0 vs the nominal 1000bps), and PERSISTENCE (re-poll after a few minutes ->
is it competed away like the reward bands were?). Capacity = executable $ that survives.

Run on the box: python3 backtest/arb_probe.py [--inspect] [--groups N] [--repoll-sec S]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import curated_backtest as cb  # noqa: E402

TAKER_FEE_BPS_SCENARIOS = [0.0, 1000.0]   # 0 (likely real) and the nominal field value (10%) for contrast


def book_of(token):
    try:
        b = cb.http_get(f"{cb.CLOB}/book?token_id={token}")
        return {"bids": b.get("bids", []) or [], "asks": b.get("asks", []) or []}
    except Exception:  # noqa: BLE001
        return {"bids": [], "asks": []}


def best_ask(book):
    a = [float(l["price"]) for l in book["asks"]]
    return min(a) if a else None


def best_bid(book):
    b = [float(l["price"]) for l in book["bids"]]
    return max(b) if b else None


def walk(levels, shares, side):
    """Cost to BUY (side='ask', ascending) or revenue to SELL (side='bid', descending) `shares`."""
    lv = sorted(((float(l["price"]), float(l["size"])) for l in levels), reverse=(side == "bid"))
    acc, need = 0.0, shares
    for p, s in lv:
        take = min(need, s)
        acc += take * p
        need -= take
        if need <= 1e-9:
            break
    return acc, shares - need  # (cash, filled)


def yes_token(m):
    toks = m.get("tokens") or []
    for t in toks:
        if str(t.get("outcome", "")).lower() in ("yes", "y"):
            return t.get("token_id")
    return toks[0].get("token_id") if toks else None


def no_token(m):
    toks = m.get("tokens") or []
    for t in toks:
        if str(t.get("outcome", "")).lower() in ("no", "n"):
            return t.get("token_id")
    return toks[1].get("token_id") if len(toks) > 1 else None


def buyall_edge(books):
    """Max $ profit buying complete sets across a group: max_K (K - Sum_i cost_i(K)); fee-aware later.
    Returns (edge_at_K1, best_K, best_edge, executable_set_cost)."""
    # grid K in shares; cap by min available ask depth
    caps = []
    for bk in books:
        caps.append(sum(float(l["size"]) for l in bk["asks"]))
    if not caps or min(caps) <= 0:
        return None
    kmax = max(1.0, min(caps))
    best = (0.0, 0.0, 0.0)  # (K, edge, cost)
    K = 1.0
    while K <= kmax:
        cost = 0.0
        ok = True
        for bk in books:
            c, filled = walk(bk["asks"], K, "ask")
            if filled < K - 1e-6:
                ok = False
                break
            cost += c
        if ok:
            edge = K - cost
            if edge > best[1]:
                best = (K, edge, cost)
        K *= 1.5
    return best  # (K*, edge$, cost$)


def sellall_edge(books):
    """Mint complete set for $1 each, sell all: max_K (Sum_i rev_i(K) - K)."""
    caps = [sum(float(l["size"]) for l in bk["bids"]) for bk in books]
    if not caps or min(caps) <= 0:
        return None
    kmax = max(1.0, min(caps))
    best = (0.0, 0.0, 0.0)
    K = 1.0
    while K <= kmax:
        rev = 0.0
        ok = True
        for bk in books:
            r, filled = walk(bk["bids"], K, "bid")
            if filled < K - 1e-6:
                ok = False
                break
            rev += r
        if ok:
            edge = rev - K
            if edge > best[1]:
                best = (K, edge, rev)
        K *= 1.5
    return best


def net_after_fee(gross_edge, traded_notional, fee_bps):
    return gross_edge - traded_notional * (fee_bps / 10000.0)


def main():
    inspect = "--inspect" in sys.argv
    ngroups = int(sys.argv[sys.argv.index("--groups") + 1]) if "--groups" in sys.argv else 40
    repoll = int(sys.argv[sys.argv.index("--repoll-sec") + 1]) if "--repoll-sec" in sys.argv else 180

    raw = cb.fetch_reward_pools()
    import collections
    groups = collections.defaultdict(list)
    for m in raw:
        k = m.get("neg_risk_market_id") or ""
        if k:
            groups[k].append(m)
    multi = {k: v for k, v in groups.items() if len(v) >= 3}
    print(f"# {len(raw)} markets, {len(multi)} multi-outcome (>=3) neg_risk groups", flush=True)

    # rank groups by outcome count (proxy for liquidity/interest), take top N
    ranked = sorted(multi.values(), key=len, reverse=True)[:ngroups]
    if inspect:
        for g in ranked[:12]:
            print(f"  {len(g):2d} outcomes: {g[0].get('question','')[:50]}")
        return

    # ---- (1) multi-outcome buy-all / mint-sell ----
    print("\n# === (1) MULTI-OUTCOME (neg_risk groups; Sum over outcomes) ===")
    print(f"# {'n':>2s} {'Smid':>6s} {'Sask':>6s} {'Sbid':>6s} {'buyall_edge$':>12s} {'K':>6s} "
          f"{'sellall_edge$':>13s} {'K':>6s}  question")
    hits = []
    for g in ranked:
        toks = [yes_token(m) for m in g]
        books = [book_of(t) for t in toks if t]
        asks = [best_ask(b) for b in books]
        bids = [best_bid(b) for b in books]
        if any(a is None for a in asks) or any(b is None for b in bids):
            continue  # incomplete book -> skip (can't execute the full set)
        smid = sum((a + b) / 2.0 for a, b in zip(asks, bids))
        sask = sum(asks)
        sbid = sum(bids)
        ba = buyall_edge(books)
        sa = sellall_edge(books)
        ba_e = ba[1] if ba else 0.0
        sa_e = sa[1] if sa else 0.0
        exhaustive = 0.95 <= smid <= 1.05
        if (ba_e > 0.01 or sa_e > 0.01) and exhaustive:
            hits.append((g, books, smid, sask, sbid, ba, sa))
        if ba_e > 0.01 or sa_e > 0.01 or not exhaustive:
            print(f"  {len(g):2d} {smid:6.3f} {sask:6.3f} {sbid:6.3f} {ba_e:12.2f} "
                  f"{(ba[0] if ba else 0):6.0f} {sa_e:13.2f} {(sa[0] if sa else 0):6.0f}  "
                  f"{g[0].get('question','')[:40]}{'' if exhaustive else '  [NON-EXHAUSTIVE smid!=1]'}",
                  flush=True)
        time.sleep(0.05)

    # ---- (2) binary YES_ask + NO_ask ----
    print("\n# === (2) BINARY YES+NO (sample of liquid binaries) ===")
    binaries = [m for m in raw if not (m.get("neg_risk")) and len(m.get("tokens") or []) == 2][:120]
    bin_hits = 0
    for m in binaries:
        yt, nt = yes_token(m), no_token(m)
        if not yt or not nt:
            continue
        by, bn = book_of(yt), book_of(nt)
        ya, na = best_ask(by), best_ask(bn)
        yb, nb = best_bid(by), best_bid(bn)
        if ya is None or na is None or yb is None or nb is None:
            continue
        if ya + na < 0.999:
            print(f"  BUY-BOTH ya+na={ya+na:.4f}  edge={1-(ya+na):.4f}  {m.get('question','')[:46]}", flush=True)
            bin_hits += 1
        if yb + nb > 1.001:
            print(f"  SELL-BOTH yb+nb={yb+nb:.4f}  edge={(yb+nb)-1:.4f}  {m.get('question','')[:46]}", flush=True)
            bin_hits += 1
        time.sleep(0.03)
    if bin_hits == 0:
        print("  none (no YES+NO<1 or >1 beyond 0.1c) across sampled binaries")

    # ---- (3) conditional ladder: within a group, 'win' <= 'reach final' <= 'reach semifinal' ----
    print("\n# === (3) CONDITIONAL LADDER violations (win<=reachfinal<=reachsemi etc.) ===")
    # best-effort: find markets sharing an entity with stage keywords, compare mids
    ladder = [("win the", 3), ("reach the final", 2), ("reach the semifinal", 1), ("reach the quarterfinal", 0),
              ("advance", 0)]
    cond_hits = 0
    by_entity = collections.defaultdict(list)
    for m in raw:
        q = m.get("question", "").lower()
        rank = next((r for kw, r in ladder if kw in q), None)
        if rank is None:
            continue
        # entity = words after the stage keyword stripped; crude: use eventSlug
        ent = m.get("neg_risk_market_id") or m.get("market_slug", "")[:20]
        by_entity[ent].append((rank, m))
    for ent, lst in by_entity.items():
        if len(lst) < 2:
            continue
        # pull mids
        priced = []
        for rank, m in lst:
            yt = yes_token(m)
            b = book_of(yt)
            a, bd = best_ask(b), best_bid(b)
            if a is None or bd is None:
                continue
            priced.append((rank, (a + bd) / 2.0, m))
        priced.sort()
        for i in range(len(priced)):
            for j in range(i + 1, len(priced)):
                r_lo, p_lo, m_lo = priced[i]
                r_hi, p_hi, m_hi = priced[j]
                if r_lo < r_hi and p_lo > p_hi + 0.01:  # lower-stage prob < higher-stage but priced higher
                    print(f"  VIOLATION p({m_hi.get('question','')[:24]})={p_hi:.3f} > "
                          f"p({m_lo.get('question','')[:24]})={p_lo:.3f}  edge={p_lo-p_hi:.3f}", flush=True)
                    cond_hits += 1
    if cond_hits == 0:
        print("  none found in identifiable ladders")

    # ---- (4) PERSISTENCE + fee-net for multi-outcome hits ----
    print(f"\n# === (4) PERSISTENCE (re-poll after {repoll}s) + fee-net for multi-outcome hits ===")
    if not hits:
        print("  no exhaustive multi-outcome arb to re-check")
    else:
        snapshot = []
        for (g, books, smid, sask, sbid, ba, sa) in hits:
            edge = max(ba[1] if ba else 0, sa[1] if sa else 0)
            notional = (ba[2] if ba and ba[1] >= (sa[1] if sa else 0) else (sa[2] if sa else 0))
            snapshot.append((g, edge, notional))
        time.sleep(repoll)
        for (g, edge0, notional) in snapshot:
            toks = [yes_token(m) for m in g]
            books = [book_of(t) for t in toks if t]
            if any(best_ask(b) is None or best_bid(b) is None for b in books):
                print(f"  {g[0].get('question','')[:40]}: book incomplete on re-poll"); continue
            ba = buyall_edge(books); sa = sellall_edge(books)
            edge1 = max(ba[1] if ba else 0, sa[1] if sa else 0)
            nets = ", ".join(f"fee{int(f)}bps->${net_after_fee(edge1, notional, f):+.2f}"
                             for f in TAKER_FEE_BPS_SCENARIOS)
            print(f"  {g[0].get('question','')[:40]:40s} edge t0=${edge0:.2f} -> t1=${edge1:.2f}  ({nets})",
                  flush=True)


if __name__ == "__main__":
    main()
