#!/usr/bin/env python3
"""Data-driven maker backtest for the DDS-curated strategy.

ZERO real money. Pure offline analysis over PUBLIC Polymarket data — it places no orders, signs
nothing, and never touches PM_TRADER_LIVE. It answers one question:

    Is the curated high-reward-SHARE, broad-based strategy NET-POSITIVE (reward - adverse selection)?

Background: live-maker was net-negative at $1k on numeric-selected pools because reward and
adverse-selection are coupled on actively-traded pools. Hypothesis: curating high-reward-SHARE
broad-based pools (outrights: cup winners, golden boot, league futures, election fields) breaks
that coupling — broad-based "behavioral" flow is less informed than single-name news flow.

Economics ported verbatim from cpp/src/pmm/orderbook.cpp + maker_sim.cpp:
    inband_weight(s_c, M_c) = ((M_c - s_c)/M_c)^2          for 0 <= s_c <= M_c, else 0
    my_score    = min_size * inband_weight(half_spread_c, max_spread_c)
    share_gross = my_score / (my_score + existing_inband_score)     # existing from live /book
    reward_real = KAPPA * share_gross * daily * window_days         # KAPPA=0.237 real-settled
    a fill happens when a real /trades print GAPS THROUGH the two-sided quote at mid +/- S
    adverse(fill) = signed_qty * (mid_after_T - fill_price)         # post-fill drift = adverse sel.
    net         = reward_real + sum(adverse) - residual_unwind

Data sources (public, read-only):
    clob.polymarket.com/sampling-markets         -> reward pools + rewards config
    clob.polymarket.com/book?token_id=...        -> current mid + existing in-band competition
    data-api.polymarket.com/trades?market=<cond> -> every trade print (price/size/side/ts/outcome)

Run on the box (has internet): python3 backtest/curated_backtest.py [--inspect] [--max-pools N]
"""
import json
import sys
import time
import urllib.request
import urllib.error

CLOB = "https://clob.polymarket.com"
DATA = "https://data-api.polymarket.com"
KAPPA = 0.237          # real-settled / gross reward (cpp config reward_calib)
SHARE_CEIL = 0.40      # cpp rewards.hpp
UA = "Mozilla/5.0 (curated-backtest; read-only)"

# ---- tunables (swept in the report) ----
HALF_SPREAD_TICKS = [1, 2, 3]   # quote offset from mid, in ticks
AS_HORIZONS_S = [30, 120, 600]  # adverse-selection mark horizons (seconds post-fill)
WINDOW_DAYS = 3.0               # trade-history window per pool
MAX_TRADES = 6000              # cap trades pulled per pool


def http_get(url, tries=3, pause=0.4):
    last = None
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": "application/json"})
            with urllib.request.urlopen(req, timeout=20) as r:
                return json.loads(r.read().decode())
        except Exception as e:  # noqa: BLE001
            last = e
            time.sleep(pause * (i + 1))
    raise RuntimeError(f"GET failed: {url} :: {last}")


def inband_weight(s_c, max_spread_c):
    if max_spread_c <= 0 or s_c < -1e-9 or s_c > max_spread_c + 1e-9:
        return 0.0
    w = (max_spread_c - s_c) / max_spread_c
    return w * w


# --------------------------------------------------------------------------- pool discovery
def fetch_reward_pools(max_pages=40):
    """Pull /sampling-markets (paginated by next_cursor) -> list of reward-market dicts."""
    out, cursor, pages = [], "", 0
    while pages < max_pages:
        url = f"{CLOB}/sampling-markets?next_cursor={cursor}" if cursor else f"{CLOB}/sampling-markets"
        d = http_get(url)
        data = d.get("data", d) if isinstance(d, dict) else d
        if not data:
            break
        out.extend(data)
        pages += 1
        nxt = d.get("next_cursor", "") if isinstance(d, dict) else ""
        if not nxt or nxt in ("LTE=", cursor):
            break
        cursor = nxt
        time.sleep(0.2)
    return out


def parse_pool(m):
    """Extract the fields we need from a sampling-markets entry. Defensive about field names."""
    try:
        cond = m.get("condition_id") or m.get("conditionId")
        toks = m.get("tokens") or []
        if not cond or len(toks) < 2:
            return None
        rw = m.get("rewards") or {}
        rates = rw.get("rates") or []
        daily = 0.0
        for r in rates:
            daily += float(r.get("rewards_daily_rate", 0) or 0)
        if daily <= 0:
            daily = float(m.get("rewards_daily_rate", 0) or 0)
        min_size = float(rw.get("min_size", 0) or m.get("rewards_min_size", 0) or 0)
        max_spread = float(rw.get("max_spread", 0) or m.get("rewards_max_spread", 0) or 0)
        tick = float(m.get("minimum_tick_size", 0) or 0.01)
        # YES token + current price
        yes = next((t for t in toks if str(t.get("outcome", "")).lower() in ("yes", "y")), toks[0])
        no = next((t for t in toks if t is not yes), toks[1])
        mid = float(yes.get("price", 0) or 0)
        return {
            "cond": cond,
            "question": m.get("question", "")[:70],
            "yes_token": yes.get("token_id"),
            "no_token": no.get("token_id"),
            "mid0": mid,
            "min_size": min_size,
            "max_spread_c": max_spread,   # already in cents per CLOB
            "tick": tick,
            "daily": daily,
            "neg_risk": bool(m.get("neg_risk", False)),
        }
    except Exception:  # noqa: BLE001
        return None


def category(p):
    """Classify a pool by FLOW TYPE — the real driver of adverse selection. The coordinator's
    'broad-based' target is sports_outright (cup/league/boot futures); politics_news is the
    informed-flow class that killed the numeric strategy; macro_daily are noise up/down dailies."""
    q = p["question"].lower()
    if any(k in q for k in ("up or down", "fed ", "fed decide", "fed pause", "highest temperature",
                            "rate cut", "rate hike", "bps", "basis point", "close above", "close below")):
        return "macro_daily"
    if any(k in q for k in ("fdv", "after launch", "airdrop")):
        return "crypto_fdv"
    if any(k in q for k in ("election", "nominee", "nomination", "primary", "president", "prime minister",
                            "chancellor", "governor", "senate", "house seat", "democratic", "republican",
                            "convicted", "indict", "deal by", "nuclear", "ceasefire", "resign", "impeach")):
        return "politics_news"
    if any(k in q for k in ("win the", "winner", "champion", "golden boot", "top scorer", "to win",
                            "relegated", "promoted", "win group", "advance", "ballon", "title", "cup",
                            "league", "playoff", "finals", " mvp", "world cup", "premier")):
        return "sports_outright"
    if any(k in q for k in (" vs ", "vs.", "o/u", "over/under", " win on ", "to score", "clean sheet")):
        return "sports_match"
    return "other"


def book_competition(token, mid, max_spread_c):
    """Existing in-band maker score from the live book (the reward-share denominator).
    Returns the binding (min of bid/ask) in-band score, like cpp binding_qmin."""
    try:
        b = http_get(f"{CLOB}/book?token_id={token}")
    except Exception:  # noqa: BLE001
        return None
    bid_score = ask_score = 0.0
    for lv in b.get("bids", []):
        px = float(lv["price"]); sz = float(lv["size"])
        bid_score += sz * inband_weight((mid - px) * 100.0, max_spread_c)
    for lv in b.get("asks", []):
        px = float(lv["price"]); sz = float(lv["size"])
        ask_score += sz * inband_weight((px - mid) * 100.0, max_spread_c)
    return min(bid_score, ask_score)   # binding side


# --------------------------------------------------------------------------- trades
def fetch_trades(cond, since_ts):
    """data-api /trades, paginated by offset, newest-first, until older than since_ts or capped.
    Normalize every print into YES-space: (ts, yes_price, yes_side, size).
    NOTE: data-api hard-caps offset at ~3000 (400 beyond) -> we keep the most-recent <=3500 trades
    and scale reward to the actual span covered; a 400/transient just stops pagination (no pool drop)."""
    rows, offset = [], 0
    while len(rows) < MAX_TRADES and offset <= 3000:
        url = f"{DATA}/trades?market={cond}&limit=500&offset={offset}"
        try:
            d = http_get(url, tries=2)
        except Exception:  # noqa: BLE001  -- offset cap or transient: keep partial, don't drop pool
            break
        if not isinstance(d, list) or not d:
            break
        for t in d:
            ts = int(t.get("timestamp", 0))
            price = float(t.get("price", 0) or 0)
            size = float(t.get("size", 0) or 0)
            side = str(t.get("side", "")).upper()
            outcome = str(t.get("outcome", "")).lower()
            if price <= 0 or size <= 0:
                continue
            # to YES-space: a NO trade at q == a YES trade at 1-q with flipped side
            if outcome in ("yes", "y") or t.get("outcomeIndex", 0) == 0:
                yp, ys = price, side
            else:
                yp, ys = 1.0 - price, ("SELL" if side == "BUY" else "BUY")
            rows.append((ts, yp, ys, size))
        offset += 500
        if d and int(d[-1].get("timestamp", 0)) < since_ts:
            break
        time.sleep(0.15)
    rows = [r for r in rows if r[0] >= since_ts]
    rows.sort(key=lambda r: r[0])
    return rows


# --------------------------------------------------------------------------- simulation
def simulate(pool, trades, half_spread_ticks, horizon_s, share_gross):
    """Two-sided min_size maker re-centered each print at mid +/- S. Fills when a real print
    gaps THROUGH the quote; adverse selection = inventory marked at the price `horizon_s` later."""
    tick = pool["tick"]
    S = half_spread_ticks * tick
    min_size = pool["min_size"] if pool["min_size"] > 0 else 100.0
    if not trades:
        return None
    # price path = last-trade price at each event time
    times = [t[0] for t in trades]
    prices = [t[1] for t in trades]

    def price_at(ts):
        # last price at or before ts (binary search would be faster; linear ok at our sizes)
        lo, hi, ans = 0, len(times) - 1, prices[0]
        while lo <= hi:
            m = (lo + hi) // 2
            if times[m] <= ts:
                ans = prices[m]; lo = m + 1
            else:
                hi = m - 1
        return ans

    inv = 0.0          # YES inventory (shares)
    fills = []         # (ts, signed_qty, fill_price)
    mid = prices[0]
    for (ts, p, side, size) in trades:
        b = mid - S
        a = mid + S
        if side == "SELL" and p <= b + 1e-12:      # sweep through my bid -> I buy YES at b
            q = min(min_size, size)
            inv += q
            fills.append((ts, q, b))
        elif side == "BUY" and p >= a - 1e-12:      # lift through my ask -> I sell YES at a
            q = min(min_size, size)
            inv -= q
            fills.append((ts, -q, a))
        mid = p
    # adverse selection: mark each fill's qty at price horizon_s later
    adverse = 0.0
    for (ts, sq, fp) in fills:
        p_after = price_at(ts + horizon_s)
        adverse += sq * (p_after - fp)   # buy(+q) and price falls -> negative; sells symmetric
    # residual inventory unwind at final mid, paying the half-spread to cross out
    final_mid = prices[-1]
    unwind = abs(inv) * S
    resid_mtm = inv * (final_mid - (sum(sq * fp for _, sq, fp in fills) / inv)) if inv != 0 else 0.0

    span_s = max(1.0, times[-1] - times[0])
    span_days = span_s / 86400.0
    reward_real = KAPPA * share_gross * pool["daily"] * span_days

    net = reward_real + adverse - unwind
    return {
        "fills": len(fills),
        "span_days": round(span_days, 3),
        "reward_real": reward_real,
        "adverse": adverse,
        "unwind": unwind,
        "net": net,
        "share_gross": share_gross,
        "resid_inv": inv,
    }


def my_share(pool, half_spread_ticks, existing_score):
    tick = pool["tick"]
    s_c = half_spread_ticks * tick * 100.0
    my_score = pool["min_size"] * inband_weight(s_c, pool["max_spread_c"])
    if my_score <= 0:
        return 0.0
    denom = my_score + (existing_score if existing_score is not None else 0.0)
    sh = my_score / denom if denom > 0 else 0.0
    return min(sh, SHARE_CEIL)


# --------------------------------------------------------------------------- main
def main():
    inspect = "--inspect" in sys.argv
    max_pools = 14
    if "--max-pools" in sys.argv:
        max_pools = int(sys.argv[sys.argv.index("--max-pools") + 1])

    print(f"# fetching reward pools (sampling-markets)…", flush=True)
    raw = fetch_reward_pools()
    pools = [pp for pp in (parse_pool(m) for m in raw) if pp]
    print(f"# {len(raw)} sampling markets -> {len(pools)} parsed reward pools", flush=True)

    if inspect:
        # dump a few parsed pools + one raw entry's keys to validate the parser
        if raw:
            print("# raw[0] keys:", sorted(raw[0].keys()))
            print("# raw[0].rewards:", json.dumps(raw[0].get("rewards", {}))[:300])
        for pp in pools[:8]:
            print("  ", {k: pp[k] for k in ("question", "mid0", "min_size", "max_spread_c", "tick", "daily", "neg_risk")})
        return

    import statistics
    # curate: makeable (mid 0.30-0.70) + has reward + sane min_size; group by FLOW category.
    cand = [pp for pp in pools if 0.30 <= pp["mid0"] <= 0.70 and pp["daily"] > 0 and pp["min_size"] > 0]
    for pp in cand:
        pp["cat"] = category(pp)
    cand.sort(key=lambda pp: pp["daily"], reverse=True)
    CAP = {"sports_outright": 14, "sports_match": 6, "politics_news": 8, "macro_daily": 6,
           "crypto_fdv": 3, "other": 4}
    picked, seen = [], {}
    for pp in cand:
        c = pp["cat"]
        if seen.get(c, 0) < CAP.get(c, 4):
            picked.append(pp); seen[c] = seen.get(c, 0) + 1
    print("# curated " + str(len(picked)) + " pools by category: " +
          ", ".join(f"{k}={v}" for k, v in sorted(seen.items())), flush=True)

    now = int(time.time())
    since = now - int(WINDOW_DAYS * 86400)
    results, book_fail = [], 0
    for pp in picked:
        try:
            comp = book_competition(pp["yes_token"], pp["mid0"], pp["max_spread_c"])
            trades = fetch_trades(pp["cond"], since)
        except Exception as e:  # noqa: BLE001
            print(f"  ! skip {pp['question'][:38]}: {e}"); continue
        book_ok = comp is not None
        book_fail += 0 if book_ok else 1
        sh = my_share(pp, HALF_SPREAD_TICKS[1], comp if book_ok else None)
        base = simulate(pp, trades, HALF_SPREAD_TICKS[1], AS_HORIZONS_S[1], sh)
        if not base:
            continue
        be = (-base["adverse"] + base["unwind"]) / (KAPPA * pp["daily"] * max(base["span_days"], 1e-9))
        results.append((pp, base, comp, sh, be, book_ok))
        print(f"  {pp['cat']:15s} {pp['question'][:36]:36s} fills={base['fills']:4d} "
              f"rew={base['reward_real']:+7.1f} adv={base['adverse']:+8.1f} net={base['net']:+8.1f} "
              f"be={be:5.2f} {'bk' if book_ok else 'NB'}", flush=True)
        time.sleep(0.15)

    # ---- per-category verdict: lean on breakeven_share (share-estimate-INDEPENDENT) ----
    print(f"\n# book ok {len(results)-book_fail}/{len(results)} (NB=no book -> share defaulted to ceil)\n")
    print(f"# {'category':16s} {'pools':5s} {'fills':6s} {'med_be':7s} {'%be<=.40':8s} {'netS2T120':10s}"
          "  (be=AS/(k*daily*days); <=0.40 => net-pos at achievable share)")
    cats = {}
    for (pp, base, comp, sh, be, ok) in results:
        cats.setdefault(pp["cat"], []).append((pp, base, be))
    for c in sorted(cats):
        rs = cats[c]
        bes = sorted(b for (_, _, b) in rs)
        med = statistics.median(bes) if bes else float("nan")
        frac = sum(1 for b in bes if b <= SHARE_CEIL) / len(bes)
        netsum = sum(b["net"] for (_, b, _) in rs)
        fillsum = sum(b["fills"] for (_, b, _) in rs)
        print(f"# {c:16s} {len(rs):<5d} {fillsum:<6d} {med:<7.2f} {frac*100:<8.0f} {netsum:+10.1f}")

    # AS-horizon sensitivity on the curated TARGET (sports_outright), share fixed per pool
    so_rows = [r for r in results if r[0]["cat"] == "sports_outright"]
    print("\n# sports_outright NET vs adverse-horizon T (S=2 ticks):")
    for h in AS_HORIZONS_S:
        tot = sum((simulate(pp, fetch_trades_cache.get(pp["cond"]), HALF_SPREAD_TICKS[1], h, sh) or {"net": 0})["net"]
                  for (pp, _b, _c, sh, _be, _ok) in so_rows if fetch_trades_cache.get(pp["cond"]))
        print(f"#   T={h}s: NET={tot:+.1f}")

    print("\n# === VERDICT (does curating sports_outright break the reward/adverse coupling?) ===")

    def agg(rs):
        rew = sum(b["reward_real"] for (_, b, _) in rs); adv = sum(b["adverse"] for (_, b, _) in rs)
        net = sum(b["net"] for (_, b, _) in rs); npos = sum(1 for (_, b, _) in rs if b["net"] > 0)
        return rew, adv, net, npos
    for name in ("sports_outright", "politics_news", "macro_daily", "sports_match"):
        rs = cats.get(name, [])
        if not rs:
            continue
        rew, adv, net, npos = agg(rs)
        print(f"# {name:16s}: {len(rs)} pools reward={rew:+.1f} adverse={adv:+.1f} NET={net:+.1f} "
              f"net-pos={npos}/{len(rs)}")


fetch_trades_cache = {}
_orig_fetch = fetch_trades


def fetch_trades(cond, since_ts):  # noqa: F811  (memoize so the sensitivity grid doesn't re-pull)
    if cond in fetch_trades_cache:
        return fetch_trades_cache[cond]
    r = _orig_fetch(cond, since_ts)
    fetch_trades_cache[cond] = r
    return r


if __name__ == "__main__":
    main()
