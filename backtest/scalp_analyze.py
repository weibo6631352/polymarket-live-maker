#!/usr/bin/env python3
# scalp_analyze.py — measure the REAL scalp edge of the lag (no resolution bet, no sigma model). On each sharp
# BTC move we'd BUY the favored side at the PM ASK; PM lags ~200ms then catches up. We EXIT by selling at the
# PM BID. Net scalp = exit_bid(after the lag) - entry_ask. We must clear the spread to profit. Measures the
# distribution of this scalp at several hold times. Uses the raw recorder CSV + OUR local timestamps.
import sys, math, bisect
CSV = sys.argv[1] if len(sys.argv) > 1 else "/tmp/raw_ticks.csv"
THRESH = 0.0003          # BTC move trigger (0.03%/3s)
LAT = 50                 # our latency ms before we fill
HOLDS = [200, 400, 800, 1500, 3000]   # ms to hold before exiting (sell the bid)

okx = []; pm = {"up": [], "dn": []}; wins = []   # pm rows keep (t, bid, ask)
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
def bidask(side, t):  # latest (bid,ask) for side at/before t
    rows = pm[side]; i = bisect.bisect_right([r[0] for r in rows], t) - 1
    return (rows[i][1], rows[i][2]) if i >= 0 else (None, None)
def mid_at(t):
    i = bisect.bisect_right(ot, t) - 1
    return okx[i][1] if i >= 0 else None

scalps = {h: [] for h in HOLDS}
ntrig = 0
for wi, w in enumerate(wins):
    lo = w["t0"]; hi = wins[wi + 1]["t0"] if wi + 1 < len(wins) else w["end"] + 60000
    last = 0
    for t, m in okx:
        if not (lo <= t <= w["end"] - 20000):
            continue
        if t - last < 2000:
            continue
        a = mid_at(t - 3000)
        if not a:
            continue
        mv = m / a - 1
        if abs(mv) <= THRESH:
            continue
        last = t
        side = "up" if mv > 0 else "dn"
        _, entry_ask = bidask(side, t + LAT)            # we pay the ask, LAT later
        if not entry_ask or not (0.03 < entry_ask < 0.97):
            continue
        ntrig += 1
        for h in HOLDS:
            exit_bid, _ = bidask(side, t + LAT + h)     # we exit by selling the bid
            if exit_bid and exit_bid > 0:
                scalps[h].append(exit_bid - entry_ask)  # net scalp per share (spread already in)

print("parsed okx=%d pm_up=%d windows=%d ; scalp triggers=%d" % (len(okx), len(pm["up"]), len(wins), ntrig))
print("\nNET SCALP per share = exit_bid(after hold) - entry_ask  (must be >0 to beat the spread)")
print("hold_ms   n   mean      win%   median    p25     p75")
for h in HOLDS:
    s = scalps[h]
    if not s:
        continue
    s2 = sorted(s); n = len(s2)
    mean = sum(s2) / n
    wr = 100 * sum(1 for x in s2 if x > 0) // n
    print("  %4d  %3d  %+.4f   %3d%%   %+.4f  %+.4f  %+.4f" % (
        h, n, mean, wr, s2[n // 2], s2[n // 4], s2[3 * n // 4]))
print("\n-> if mean>0 at some hold, the lag-scalp beats the spread; if all <=0, the spread eats the lag edge")
