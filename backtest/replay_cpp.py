#!/usr/bin/env python3
# replay_cpp.py — FAITHFUL 1:1 replay of the live C++ sniper on the raw CSV, so the offline number matches what
# the bot really does. Models every real-execution friction the headline backtest ignored:
#   - ONE position at a time (the C++ can't trigger while holding -> it MISSES triggers during a hold)
#   - edge-trigger + re-arm hysteresis + post-exit cooldown (not the backtest's level-trigger)
#   - entry at the displayed ask on a BTC seconds-move, CHEAP side only (ask<CHEAP_MAX)
#   - EXIT delayed to the SETTLEMENT floor (~3.5s): shares aren't sellable until they settle (CLOB "balance:0"),
#     so the real exit is ~max(HOLD_MS, SETTLE_MS) at the bid THEN — not the optimistic 1.5s
#   - NET of the real taker fee 0.07*p*(1-p) on BOTH legs
# Sweeps SETTLE_MS to show sensitivity. This is the honest "does the real strategy net positive".
import sys, math, bisect
CSV = sys.argv[1] if len(sys.argv) > 1 else "/tmp/raw_ticks.csv"
THRESH = 0.0003
LAT = 50
HOLD_MS = 3000
COOLDOWN_MS = 2000
CHEAP_MAX = float(__import__("os").environ.get("PMAX", "0.55"))   # price-band upper (quant: skip near-0.5 = max fee/min gap)
PMIN = float(__import__("os").environ.get("PMIN", "0.03"))        # price-band lower
SETTLES = [0, 1500, 3500, 5000]   # 0 = the optimistic backtest; 3500 = the observed live settlement floor
def fee(p):
    return 0.07 * p * (1 - p)

okx = []; pm = {"up": [], "dn": []}; wins = []
for line in open(CSV):
    p = line.rstrip("\n").split(",")
    if len(p) < 3:
        continue
    try:
        t = float(p[0]); k = p[1]
        if k == "okx":
            okx.append((t, (float(p[2]) + float(p[3])) / 2))
        elif k == "pm" and p[2] in pm:
            pm[p[2]].append((t, float(p[3]) if p[3] else 0.0, float(p[4]) if p[4] else 0.0))
        elif k == "win":
            wins.append({"up": p[2], "dn": p[3], "open": float(p[4]), "end": float(p[5]) * 1000, "t0": t})
    except Exception:
        continue
okx.sort(); pm["up"].sort(); pm["dn"].sort(); wins.sort(key=lambda w: w["t0"])
ot = [x[0] for x in okx]
pmt = {s: [r[0] for r in pm[s]] for s in pm}
def mid_at(t):
    i = bisect.bisect_right(ot, t) - 1
    return okx[i][1] if i >= 0 else None
def move(t):
    n = mid_at(t); a = mid_at(t - 3000)
    return (n / a - 1) if (n and a) else 0.0
def quote(side, t):  # (bid, ask) at/before t
    rows = pm[side]; i = bisect.bisect_right(pmt[side], t) - 1
    return (rows[i][1], rows[i][2]) if i >= 0 else (0.0, 0.0)

def mid_side(side, t):
    b, a = quote(side, t)
    return (b + a) / 2 if a else b
def outcome_up(w):  # True if Up resolved to ~1 (read the PM price well after the window end)
    return mid_side("up", w["end"] + 45000) >= mid_side("dn", w["end"] + 45000)

HAIRCUT = float(__import__("os").environ.get("EXIT_HAIRCUT", "0.01"))  # exit 1 tick below bid = depth walk-down proxy
def replay(settle, resolve_aware, split=False):
    trades = []; n_resolved = 0
    cont = []; rev = []  # net-EV split: BTC CONTINUED vs REVERTED over the forced hold (adverse-selection test)
    for wi, w in enumerate(wins):
        lo = w["t0"]; hi = wins[wi + 1]["t0"] if wi + 1 < len(wins) else w["end"]
        up_won = outcome_up(w)
        armed = True; last_exit = -1e9; pos = None
        for idx in range(bisect.bisect_left(ot, lo), bisect.bisect_right(ot, w["end"] - 12000)):
            t, m = okx[idx]
            if pos is None:
                mv = move(t)
                if abs(mv) < THRESH * 0.5:
                    armed = True
                if abs(mv) > THRESH and armed and t - last_exit > COOLDOWN_MS:
                    up = mv > 0; side = "up" if up else "dn"
                    _, ask = quote(side, t + LAT)
                    if PMIN < ask < CHEAP_MAX:  # PRICE-BAND: skip near-0.5 (max fee, min gap) — quant iteration #1
                        armed = False
                        pos = {"side": side, "ask": ask, "t": t, "sell": t + max(HOLD_MS, settle), "mv": mv}
            else:
                if t >= pos["sell"]:
                    bid, _ = quote(pos["side"], t)
                    if bid > 0.02:  # a real liquid bid -> sell; HAIRCUT models the depth walk-down (thin cheap book)
                        exitp = max(bid - HAIRCUT, 0.01)
                        net = (exitp - pos["ask"]) - fee(pos["ask"]) - fee(exitp)
                        trades.append(net)
                        # adverse-selection split: did BTC CONTINUE (same sign as entry move) over the hold, or REVERT?
                        br = (mid_at(t) or 1) / (mid_at(pos["t"]) or 1) - 1
                        (cont if (br > 0) == (pos["mv"] > 0) else rev).append(net)
                        last_exit = t; armed = False; pos = None
                    elif not resolve_aware:
                        trades.append((bid - pos["ask"]) - fee(pos["ask"]) - fee(bid))
                        last_exit = t; armed = False; pos = None
        if pos is not None:  # couldn't sell before the window ended -> RESOLVES (winner=1, loser=0) — the user's point
            won = (pos["side"] == "up") == up_won
            net = (1.0 if won else 0.0) - pos["ask"] - fee(pos["ask"])  # redemption: no exit taker fee
            trades.append(net); (cont if won else rev).append(net); n_resolved += 1
    return trades, n_resolved, cont, rev

print("replay on %d windows (faithful C++ exec: 1-pos-at-a-time, edge-trig, cheap, settle-delayed exit, net fees)\n" % len(wins))
print("SETTLE  resolve-aware  n   net/sh   win%   t      total$   maxDD$   nResolved(->0/1)")
SPLIT = None
for s in SETTLES:
    for ra in (False, True):
        tr, nres, cont, rev = replay(s, ra)
        if s == 3500 and ra:
            SPLIT = (cont, rev)
        if not tr:
            continue
        n = len(tr); mean = sum(tr) / n
        sd = (sum((x - mean) ** 2 for x in tr) / (n - 1)) ** 0.5 if n > 1 else 0.0
        se = sd / math.sqrt(n) if n else 0.0
        cum = peak = maxdd = 0.0
        for x in tr:
            cum += x * 5; peak = max(peak, cum); maxdd = max(maxdd, peak - cum)
        print("  %4d     %-4s     %3d  %+.4f  %3d%%  %5.2f  %+7.2f  %6.2f    %d" % (
            s, "YES" if ra else "no", n, mean, 100 * sum(1 for x in tr if x > 0) // n,
            mean / se if se else 0, sum(tr) * 5, maxdd, nres))
print("\n-> SETTLE=0 is the optimistic headline backtest; SETTLE=3500 is the REAL live execution.")
print("   If +EV collapses from SETTLE=0 to 3500, the settlement delay is the edge-killer (live-confirmed).")

if SPLIT:
    cont, rev = SPLIT
    def stat(name, v):
        if v:
            m = sum(v) / len(v)
            print("  %-24s n=%3d  net/sh=%+.4f  win=%d%%  total$=%+.2f" % (
                name, len(v), m, 100 * sum(1 for x in v if x > 0) // len(v), sum(v) * 5))
        else:
            print("  %-24s (none)" % name)
    print("\n=== DECISIVE: adverse-selection split @SETTLE=3500, haircut=%.2f (expert make-or-break) ===" % HAIRCUT)
    stat("BTC CONTINUED over hold", cont)
    stat("BTC REVERTED over hold", rev)
    print("  -> edge only in CONTINUED + REVERTED negative = a continuation-bet in disguise (unpredictable at a")
    print("     momentum trigger -> coin-flip+fees). BOTH positive = PM lag-closure dominates = a real edge.")
