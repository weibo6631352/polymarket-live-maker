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


def is_broad_based(p):
    """Heuristic: outright/field markets (many-way, behavioral) vs single-name news (binary).
    neg_risk multi-candidate fields are the canonical broad-based pools; also catch outright
    question patterns. Single-name 'Will X happen by <date>' news binaries are NOT broad-based."""
    q = p["question"].lower()
    broad_kw = ("win the", "winner", "champion", "golden boot", "top scorer", "mvp", "to win",
                "relegated", "promoted", "nominee", "nomination", "win group", "advance",
                "finals", "final four", "playoff", "cup", "league", "open", "election",
                "president", "be the next", "win most", "ballon")
    if p["neg_risk"]:
        return True
    return any(k in q for k in broad_kw)


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
    Normalize every print into YES-space: (ts, yes_price, yes_side, size)."""
    rows, offset = [], 0
    while len(rows) < MAX_TRADES:
        url = f"{DATA}/trades?market={cond}&limit=500&offset={offset}"
        d = http_get(url)
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

    # curate: makeable (mid 0.30-0.70) + has reward + min_size sane; split broad vs single-name
    cand = [pp for pp in pools if 0.30 <= pp["mid0"] <= 0.70 and pp["daily"] > 0 and pp["min_size"] > 0]
    cand.sort(key=lambda pp: pp["daily"], reverse=True)
    broad = [pp for pp in cand if is_broad_based(pp)][:max_pools]
    single = [pp for pp in cand if not is_broad_based(pp)][:max_pools // 2]
    print(f"# curated: {len(broad)} broad-based + {len(single)} single-name (makeable, high-reward)", flush=True)

    now = int(time.time())
    since = now - int(WINDOW_DAYS * 86400)
    rows = []
    for grp, plist in (("BROAD", broad), ("SINGLE", single)):
        for pp in plist:
            try:
                comp = book_competition(pp["yes_token"], pp["mid0"], pp["max_spread_c"])
                trades = fetch_trades(pp["cond"], since)
            except Exception as e:  # noqa: BLE001
                print(f"  ! skip {pp['question'][:40]}: {e}")
                continue
            # representative: middle half_spread, middle horizon
            sh = my_share(pp, HALF_SPREAD_TICKS[1], comp)
            base = simulate(pp, trades, HALF_SPREAD_TICKS[1], AS_HORIZONS_S[1], sh)
            if not base:
                print(f"  - {grp} {pp['question'][:46]:46s} no trades in window")
                continue
            be_share = (-base["adverse"] + base["unwind"]) / (KAPPA * pp["daily"] * max(base["span_days"], 1e-9)) \
                if pp["daily"] > 0 else float("inf")
            rows.append((grp, pp, base, comp, sh, be_share))
            print(f"  {grp:6s} {pp['question'][:42]:42s} fills={base['fills']:4d} "
                  f"reward={base['reward_real']:+8.2f} adverse={base['adverse']:+8.2f} "
                  f"net={base['net']:+8.2f} share={sh:.3f} be_share={be_share:.2f}", flush=True)
            time.sleep(0.2)

    # ----- sensitivity grid + totals -----
    print("\n# === sensitivity (sum of NET over each group, $ over window) ===")
    print(f"# {'group':6s} {'S(ticks)':8s} " + " ".join(f"T={h}s".rjust(10) for h in AS_HORIZONS_S))
    summary = {}
    for grp in ("BROAD", "SINGLE"):
        grows = [r for r in rows if r[0] == grp]
        for st in HALF_SPREAD_TICKS:
            cells = []
            for h in AS_HORIZONS_S:
                tot = 0.0
                for (_, pp, _b, comp, _sh, _be) in grows:
                    sh = my_share(pp, st, comp)
                    tr = fetch_trades_cache.get(pp["cond"])
                    s = simulate(pp, tr, st, h, sh) if tr else None
                    if s:
                        tot += s["net"]
                cells.append(tot)
                summary[(grp, st, h)] = tot
            print(f"# {grp:6s} {st:<8d} " + " ".join(f"{c:+10.2f}" for c in cells))

    print("\n# === VERDICT ===")
    for grp in ("BROAD", "SINGLE"):
        grows = [r for r in rows if r[0] == grp]
        if not grows:
            continue
        rew = sum(r[2]["reward_real"] for r in grows)
        adv = sum(r[2]["adverse"] for r in grows)
        unw = sum(r[2]["unwind"] for r in grows)
        net = sum(r[2]["net"] for r in grows)
        npos = sum(1 for r in grows if r[2]["net"] > 0)
        print(f"# {grp}: {len(grows)} pools | reward(real)={rew:+.2f} adverse={adv:+.2f} unwind={unw:+.2f} "
              f"NET={net:+.2f} | net-positive pools={npos}/{len(grows)} (S=2tk,T=120s window~{WINDOW_DAYS}d)")


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
