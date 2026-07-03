#!/usr/bin/env python3
"""Rigorous validation of the ONE net-positive class from curated_backtest.py: noise-daily markets
(S&P/AAPL/AMZN/crypto "up or down", "close above/below", temperature). ZERO real money — public-API
GET only, no orders, PM_TRADER_LIVE untouched.

Resolves the reward-overstatement caveat: curated_backtest used a single EARLY-life book share
snapshot. As a daily market fills over its ~2.5d life, makers join -> my share decays. Here:
  1. TIME-INTEGRATED share: reward = Sigma_t share(t)*daily*dt, share(t)=my/(my+L(t)), where rival
     in-band liquidity L(t) = L_now * cumvol(t)/cumvol_total ramps with participation, anchored to
     the CURRENT (mature) book L_now. Compare to the early snapshot to quantify overstatement.
  2. KAPPA double-count check: kappa=0.237 was calibrated as real_settled/gross_SNAPSHOT, i.e. it
     ALREADY corrects snapshot share being too high (book understates real competition). So applying
     kappa AND time-integrating risks double-counting. We report BOTH:
        rew_integrated_noK  (time-integration IS the competition correction)  [optimistic]
        rew_integrated_K    (time-integration AND kappa)                       [pessimistic]
     truth is bracketed between.
  3. ADVERSE SELECTION: real /trades fills + post-fill drift (reuses curated_backtest.simulate).
  4. CAPACITY: more capital -> bigger my_size -> higher share (reward saturates) but linearly more
     adverse inventory. Find size* where marginal net peaks; sum net*/capital* over the universe.

Run on the box: python3 backtest/noise_daily_validate.py [--inspect] [--max N]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import curated_backtest as cb  # noqa: E402

S_TICKS = 2
T_AS = 120
KAPPA = cb.KAPPA


def is_noise_daily(q):
    q = q.lower()
    return any(k in q for k in ("up or down", "highest temperature", "close above", "close below",
                                "higher or lower"))


def parse_iso(s):
    if not s:
        return None
    try:
        import datetime as dt
        return dt.datetime.fromisoformat(str(s).replace("Z", "+00:00")).timestamp()
    except Exception:  # noqa: BLE001
        return None


def time_integrated_reward(pool, trades, L_now, t0, t1, s_ticks):
    """Sigma_t share(t)*daily*dt with L(t) ramping by cumulative volume to the mature book L_now."""
    daily, tick = pool["daily"], pool["tick"]
    w = cb.inband_weight(s_ticks * tick * 100.0, pool["max_spread_c"])
    my = pool["min_size"] * w
    if my <= 0 or daily <= 0 or t1 <= t0:
        return None
    tr = sorted([t for t in trades if t0 <= t[0] <= t1], key=lambda r: r[0])
    tot_vol = sum(t[3] for t in tr) or 1.0
    pts, cum = [(t0, 0.0)], 0.0
    for (ts, p, side, sz) in tr:
        cum += sz
        pts.append((min(ts, t1), cum))
    pts.append((t1, cum))
    rew_nok = 0.0
    for i in range(1, len(pts)):
        tA, cA = pts[i - 1]
        tB, cB = pts[i]
        if tB <= tA:
            continue
        L = L_now * (cA / tot_vol)
        share = my / (my + L) if (my + L) > 0 else 0.0
        rew_nok += share * daily * ((tB - tA) / 86400.0)
    life_days = (t1 - t0) / 86400.0
    share_mature = my / (my + L_now) if (my + L_now) > 0 else 0.0
    share_eff = rew_nok / (daily * life_days) if (daily * life_days) > 0 else 0.0
    return dict(life_days=life_days, L_now=L_now, share_mature=share_mature, share_integrated=share_eff,
                rew_integrated_noK=rew_nok, rew_integrated_K=KAPPA * rew_nok,
                rew_snapshot_ceilK=KAPPA * cb.SHARE_CEIL * daily * life_days)  # what curated_backtest assumed


def capacity(pool, L_now, life_days, as_min_full, s_ticks):
    """net(size) = KAPPA*share(size)*daily*life - AS(size); AS scales ~linearly with size (capped by
    flow). Grid size in [min_size, 120x] -> size* maximizing net (marginal->0)."""
    daily, tick, mn = pool["daily"], pool["tick"], pool["min_size"]
    w = cb.inband_weight(s_ticks * tick * 100.0, pool["max_spread_c"])
    best = (mn, -1e18, 0.0)
    for i in range(1, 481):
        size = mn * (i / 4.0)
        my = size * w
        share = my / (my + L_now) if (my + L_now) > 0 else 0.0
        rew = KAPPA * share * daily * life_days
        asz = as_min_full * (size / mn)            # full-pickoff AS scaled with size (conservative)
        net = rew - asz
        if net > best[1]:
            best = (size, net, share)
    return best  # (size*, net*, share*)


def main():
    inspect = "--inspect" in sys.argv
    maxn = int(sys.argv[sys.argv.index("--max") + 1]) if "--max" in sys.argv else 24
    now = int(time.time())

    raw = cb.fetch_reward_pools()
    nd = [m for m in raw if is_noise_daily(m.get("question", ""))]
    # universe gross
    def daily_of(m):
        rw = m.get("rewards") or {}
        return sum(float(r.get("rewards_daily_rate", 0) or 0) for r in (rw.get("rates") or []))
    gross_universe = sum(daily_of(m) for m in nd)
    print(f"# noise-daily universe: {len(nd)} markets, gross reward ${gross_universe:.0f}/day", flush=True)

    # keep still-open markets (end in the future -> live book) sorted by daily rate
    cand = []
    for m in nd:
        t1 = parse_iso(m.get("end_date_iso"))
        t0 = parse_iso(m.get("accepting_order_timestamp"))
        if not t1 or not t0 or t1 <= now:           # need a live book + a real elapsed window
            continue
        pp = cb.parse_pool(m)
        if not pp or pp["min_size"] <= 0 or pp["daily"] <= 0:
            continue
        if not (0.20 <= pp["mid0"] <= 0.80):
            continue
        pp["t0"], pp["t1_end"] = t0, t1
        cand.append(pp)
    cand.sort(key=lambda p: p["daily"], reverse=True)
    cand = cand[:maxn]
    print(f"# {len(cand)} still-open noise-dailies sampled (live book, mid 0.2-0.8)", flush=True)

    if inspect:
        for pp in cand[:10]:
            print("  ", pp["question"][:50], "daily=$%.0f" % pp["daily"], "min=%.0f" % pp["min_size"],
                  "mid=%.2f" % pp["mid0"], "tick=", pp["tick"])
        return

    rows = []
    for pp in cand:
        try:
            L_now = cb.book_competition(pp["yes_token"], pp["mid0"], pp["max_spread_c"])
            trades = cb.fetch_trades(pp["cond"], int(pp["t0"]))
        except Exception as e:  # noqa: BLE001
            print(f"  ! skip {pp['question'][:34]}: {e}")
            continue
        if L_now is None:
            L_now = 0.0
        t1 = min(now, int(pp["t1_end"]))
        ti = time_integrated_reward(pp, trades, L_now, int(pp["t0"]), t1, S_TICKS)
        if not ti:
            continue
        # adverse selection (full pickoff at min_size) + fills
        base = cb.simulate(pp, trades, S_TICKS, T_AS, ti["share_mature"], 1.0)
        as_full = -base["adverse"] + base["unwind"] if base else 0.0  # cost (positive number)
        # realistic NET at min_size: reward(integrated, bracketed) - AS(full)
        net_opt = ti["rew_integrated_noK"] - as_full      # optimistic (no extra kappa)
        net_pes = ti["rew_integrated_K"] - as_full        # pessimistic (kappa on top)
        size_star, net_star, share_star = capacity(pp, L_now, ti["life_days"], as_full, S_TICKS)
        rows.append((pp, ti, base, as_full, net_opt, net_pes, size_star, net_star, share_star))
        print(f"  {pp['question'][:34]:34s} life={ti['life_days']:4.1f}d daily=${pp['daily']:<5.0f} "
              f"sh_snap={cb.SHARE_CEIL:.2f} sh_mat={ti['share_mature']:.3f} sh_int={ti['share_integrated']:.3f} "
              f"| rew_int=${ti['rew_integrated_noK']:6.1f} AS=${as_full:6.1f} "
              f"netK[{net_pes:+6.1f},{net_opt:+6.1f}] | cap_size*={size_star:6.0f} net*=${net_star:+6.1f}",
              flush=True)
        time.sleep(0.15)

    if not rows:
        print("# no rows"); return

    # ---- aggregates ----
    n = len(rows)
    snap_tot = sum(r[1]["rew_snapshot_ceilK"] for r in rows)
    int_tot = sum(r[1]["rew_integrated_K"] for r in rows)
    intnok_tot = sum(r[1]["rew_integrated_noK"] for r in rows)
    as_tot = sum(r[3] for r in rows)
    print(f"\n# === reward overstatement (sampled {n} pools, over their elapsed life) ===")
    print(f"# curated_backtest assumption (SHARE_CEIL*kappa):  ${snap_tot:8.1f}")
    print(f"# time-integrated * kappa  (pessimistic):          ${int_tot:8.1f}  "
          f"({int_tot/snap_tot*100:.0f}% of snapshot)")
    print(f"# time-integrated, no extra kappa (optimistic):    ${intnok_tot:8.1f}  "
          f"({intnok_tot/snap_tot*100:.0f}% of snapshot)")
    print(f"# measured adverse selection (full pickoff):        ${as_tot:8.1f}")

    netK_opt = sum(r[4] for r in rows)
    netK_pes = sum(r[5] for r in rows)
    npos_opt = sum(1 for r in rows if r[4] > 0)
    npos_pes = sum(1 for r in rows if r[5] > 0)
    print(f"\n# === NET at min_size (reward - adverse), bracketed ===")
    print(f"# optimistic (no extra kappa): NET=${netK_opt:+.1f}  net-pos {npos_opt}/{n}")
    print(f"# pessimistic (kappa on top):  NET=${netK_pes:+.1f}  net-pos {npos_pes}/{n}")

    # capacity: per-pool optimal net*/day, extrapolate to universe
    netstar_perday = [r[7] / max(r[1]["life_days"], 1e-9) for r in rows]
    cap_perpool = [r[6] for r in rows]
    tot_netstar_day = sum(netstar_perday)
    avg_netstar_day = tot_netstar_day / n
    avg_cap = sum(cap_perpool) / n
    print(f"\n# === CAPACITY (optimal sizing) ===")
    print(f"# sampled {n} pools: optimal size avg={avg_cap:.0f} shares (~${avg_cap:.0f} capital/pool); "
          f"net* avg=${avg_netstar_day:+.2f}/day/pool")
    print(f"# extrapolated to universe ({len(nd)} pools): "
          f"~${avg_netstar_day*len(nd):+.0f}/day at ~${avg_cap*len(nd):.0f} total capital "
          f"(kappa-on-top; optimistic ~{(sum(r[4] for r in rows)/sum(max(r[1]['life_days'],1e-9) for r in rows))*len(nd):+.0f}/day at min_size)")

    print(f"\n# === VERDICT ===")
    print(f"# universe gross ${gross_universe:.0f}/day; realistic captured share collapses from "
          f"snapshot {cb.SHARE_CEIL:.2f} to integrated ~{sum(r[1]['share_integrated'] for r in rows)/n:.3f}")


if __name__ == "__main__":
    main()
