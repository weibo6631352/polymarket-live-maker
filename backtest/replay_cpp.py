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
CHEAP_MAX = 0.55
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

def replay(settle):
    trades = []
    for wi, w in enumerate(wins):
        lo = w["t0"]; hi = wins[wi + 1]["t0"] if wi + 1 < len(wins) else w["end"]
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
                    if 0.03 < ask < CHEAP_MAX:
                        armed = False
                        pos = {"side": side, "ask": ask, "t": t, "sell": t + max(HOLD_MS, settle)}
            else:
                if t >= pos["sell"]:
                    bid, _ = quote(pos["side"], t)
                    if bid > 0:
                        net = (bid - pos["ask"]) - fee(pos["ask"]) - fee(bid)
                        trades.append(net)
                        last_exit = t; armed = False; pos = None
    return trades

print("replay on %d windows (faithful C++ exec: 1-pos-at-a-time, edge-trig, cheap, settle-delayed exit, net fees)\n" % len(wins))
print("SETTLE_MS  n   net/sh   win%   t    total$   maxDD$   maxLoseStreak   (5 shares)")
for s in SETTLES:
    tr = replay(s)
    if not tr:
        print("  %4d   0  (no trades)" % s); continue
    n = len(tr); mean = sum(tr) / n
    sd = (sum((x - mean) ** 2 for x in tr) / (n - 1)) ** 0.5 if n > 1 else 0.0
    se = sd / math.sqrt(n) if n else 0.0
    cum = peak = maxdd = 0.0; streak = maxstreak = 0
    for x in tr:
        cum += x * 5  # $ on 5 shares
        peak = max(peak, cum); maxdd = max(maxdd, peak - cum)
        streak = streak + 1 if x < 0 else 0; maxstreak = max(maxstreak, streak)
    print("  %4d  %3d  %+.4f  %3d%%  %4.2f  %+6.2f   %5.2f       %d" % (
        s, n, mean, 100 * sum(1 for x in tr if x > 0) // n, mean / se if se else 0, sum(tr) * 5, maxdd, maxstreak))
print("\n-> SETTLE=0 is the optimistic headline backtest; SETTLE=3500 is the REAL live execution.")
print("   If +EV collapses from SETTLE=0 to 3500, the settlement delay is the edge-killer (live-confirmed).")
