#!/usr/bin/env python3
# temporal_analyze.py — measure the PHASE LAG between BTC moving and the PM ask repricing. This is the
# sniper's ACTUAL edge: if PM lags BTC by L, you buy the stale PM price the instant BTC moves and the PM
# price catches up L later (a scalp), independent of the window outcome. Per window, cross-correlate BTC
# returns (OKX, fastest) vs PM Up-ask returns at lags; the peak-correlation lag = PM's phase lag behind BTC.
# Also: after a sharp BTC move, how fast + how far does the PM ask follow (the capturable window).
import sys, math, bisect
CSV = sys.argv[1] if len(sys.argv) > 1 else "/tmp/raw_ticks.csv"
GRID = 100                       # ms bins
LAGS = list(range(-20, 41))      # -2s .. +4s in 100ms steps (positive = BTC LEADS PM = exploitable)

okx = []; pm_up = []; pm_dn = []; wins = []
for line in open(CSV):
    p = line.rstrip("\n").split(",")
    if len(p) < 3:
        continue
    try:
        t = float(p[0]); k = p[1]
        if k == "okx":
            okx.append((t, (float(p[2]) + float(p[3])) / 2))
        elif k == "pm":
            mid = (float(p[3]) + float(p[4])) / 2 if p[4] else float(p[3])
            (pm_up if p[2] == "up" else pm_dn).append((t, mid))
        elif k == "win":
            wins.append({"up": p[2], "dn": p[3], "open": float(p[4]), "end": float(p[5]) * 1000, "t0": t})
    except Exception:
        continue
okx.sort(); pm_up.sort(); pm_dn.sort(); wins.sort(key=lambda w: w["t0"])
print("parsed okx=%d pm_up=%d pm_dn=%d windows=%d" % (len(okx), len(pm_up), len(pm_dn), len(wins)))

def gridify(series, t0, t1):
    g = []; i = 0; last = None; t = t0
    while t < t1:
        while i < len(series) and series[i][0] <= t:
            last = series[i][1]; i += 1
        g.append(last); t += GRID
    return g
def rets(g):
    return [(g[i] / g[i - 1] - 1) if (g[i] and g[i - 1]) else 0.0 for i in range(1, len(g))]

peaks = []
for wi, w in enumerate(wins):
    lo = w["t0"]; hi = wins[wi + 1]["t0"] if wi + 1 < len(wins) else w["end"] + 75000
    b = gridify([x for x in okx if lo <= x[0] < hi], lo, hi)
    u = gridify([x for x in pm_up if lo <= x[0] < hi], lo, hi)
    if sum(1 for x in u if x) < 100:
        continue
    rb = rets(b); ru = rets(u)
    best = (0, 0.0)
    for lag in LAGS:
        x = []; y = []
        for i in range(len(rb)):
            j = i - lag                       # BTC at i vs PM at i-lag => positive lag: BTC leads PM
            if 0 <= j < len(ru):
                x.append(rb[i]); y.append(ru[j])
        if len(x) < 80:
            continue
        mx = sum(x) / len(x); my = sum(y) / len(y)
        sxy = sum((x[k] - mx) * (y[k] - my) for k in range(len(x)))
        sxx = sum((v - mx) ** 2 for v in x); syy = sum((v - my) ** 2 for v in y)
        c = sxy / math.sqrt(sxx * syy) if sxx > 0 and syy > 0 else 0.0
        if abs(c) > abs(best[1]):
            best = (lag * GRID, c)
    peaks.append(best)
    print("  win%2d: peak corr %+.3f at lag %+dms  (%s)" % (
        wi, best[1], best[0], "BTC LEADS PM by %dms" % best[0] if best[0] > 0 else "no lead / PM leads"))

if peaks:
    pos = [p[0] for p in peaks if p[1] > 0.05]
    print("\nwindows with real positive corr (>0.05): %d/%d" % (len(pos), len(peaks)))
    if pos:
        pos.sort()
        print("PM phase-lag behind BTC: median=%dms  min=%dms max=%dms" % (pos[len(pos) // 2], pos[0], pos[-1]))
        print("-> if median lag > our latency, there's a window to snipe the stale PM price (scalp the catch-up)")
    else:
        print("-> NO consistent positive lag: PM is not lagging BTC in a capturable way (efficient)")
