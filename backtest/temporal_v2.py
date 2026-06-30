#!/usr/bin/env python3
# temporal_v2.py — compare BTC and PM in the SAME UNIT. BTC is a price; PM Up-mid is P(Up). So convert BTC to
# the BTC-IMPLIED P(Up) = Phi( ln(btc_now/open) / (sigma*sqrt(tau)) ), with sigma estimated from the window's
# own realized vol. Then both are probabilities -> compare apples-to-apples: do they track? how big is the
# GAP (btc-implied minus PM = the mispricing PM hasn't caught up to)? what's the LAG (PM catching up)?
import sys, math, bisect
CSV = sys.argv[1] if len(sys.argv) > 1 else "/tmp/raw_ticks.csv"
GRID = 200  # ms

okx = []; pm_up = []; wins = []
for line in open(CSV):
    p = line.rstrip("\n").split(",")
    if len(p) < 3:
        continue
    try:
        t = float(p[0]); k = p[1]
        if k == "okx":
            okx.append((t, (float(p[2]) + float(p[3])) / 2))
        elif k == "pm" and p[2] == "up":
            pm_up.append((t, (float(p[3]) + float(p[4])) / 2 if p[4] else float(p[3])))
        elif k == "win":
            wins.append({"up": p[2], "dn": p[3], "open": float(p[4]), "end": float(p[5]) * 1000, "t0": t})
    except Exception:
        continue
okx.sort(); pm_up.sort(); wins.sort(key=lambda w: w["t0"])
def Phi(x):
    return 0.5 * (1 + math.erf(x / math.sqrt(2)))
def at(series, t):
    i = bisect.bisect_right([s[0] for s in series], t) - 1
    return series[i][1] if i >= 0 else None
print("windows=%d okx=%d pm_up=%d" % (len(wins), len(okx), len(pm_up)))

all_lags = []; all_gaps = []
for wi, w in enumerate(wins):
    lo = w["t0"]; hi = wins[wi + 1]["t0"] if wi + 1 < len(wins) else w["end"] + 60000
    ob = [(t, m) for t, m in okx if lo <= t <= w["end"]]
    pu = [(t, m) for t, m in pm_up if lo <= t <= w["end"]]
    if len(ob) < 50 or len(pu) < 50:
        continue
    # sigma per sqrt-second from 1s log-returns of BTC in this window
    lr = []
    last_t = None; last_p = None
    for t, m in ob:
        if last_t is None or t - last_t >= 1000:
            if last_p:
                lr.append(math.log(m / last_p))
            last_t = t; last_p = m
    if len(lr) < 5:
        continue
    mean = sum(lr) / len(lr)
    sig_sec = (sum((x - mean) ** 2 for x in lr) / len(lr)) ** 0.5  # per ~1s
    if sig_sec <= 0:
        continue
    # build same-grid series: btc-implied P(Up) and PM P(Up)
    bi = []; pm = []; ts = []
    t = lo
    while t < w["end"] - 10000:
        bp = at(ob, t); pp = at(pu, t)
        tau = (w["end"] - t) / 1000.0
        if bp and pp and tau > 5:
            z = math.log(bp / w["open"]) / (sig_sec * math.sqrt(tau))
            bi.append(Phi(z)); pm.append(pp); ts.append(t)
        t += GRID
    if len(bi) < 50:
        continue
    gap = sum(bi[k] - pm[k] for k in range(len(bi))) / len(bi)  # avg (btc-implied - PM)
    corr0 = None
    # cross-correlate CHANGES of btc-implied vs PM, find lag (positive => PM lags btc-implied)
    dbi = [bi[k] - bi[k - 1] for k in range(1, len(bi))]
    dpm = [pm[k] - pm[k - 1] for k in range(1, len(pm))]
    best = (0, 0.0)
    for lag in range(-10, 26):  # -2s .. +5s in 200ms
        x = []; y = []
        for i in range(len(dbi)):
            j = i - lag
            if 0 <= j < len(dpm):
                x.append(dbi[i]); y.append(dpm[j])
        if len(x) < 40:
            continue
        mx = sum(x) / len(x); my = sum(y) / len(y)
        sxy = sum((x[k] - mx) * (y[k] - my) for k in range(len(x)))
        sxx = sum((v - mx) ** 2 for v in x); syy = sum((v - my) ** 2 for v in y)
        c = sxy / math.sqrt(sxx * syy) if sxx > 0 and syy > 0 else 0
        if abs(c) > abs(best[1]):
            best = (-lag * GRID, c)   # PM lag = -code_lag*GRID
    all_gaps.append(gap); all_lags.append(best)
    print("  win%2d sig/s=%.5f  avg(BTCimplied-PM)=%+.3f  corr=%+.3f  PMlag=%+dms" % (
        wi, sig_sec, gap, best[1], best[0]))

if all_lags:
    good = [(l, c) for l, c in all_lags if abs(c) > 0.1]
    print("\nwindows with corr>0.1 (BTC-implied & PM actually track): %d/%d" % (len(good), len(all_lags)))
    if good:
        ls = sorted(l for l, c in good if l > 0)
        print("avg |corr| (when tracking)=%.2f" % (sum(abs(c) for l, c in good) / len(good)))
        if ls:
            print("PM lag behind BTC-implied prob: median=%dms (n=%d positive)" % (ls[len(ls) // 2], len(ls)))
        print("avg GAP (BTC-implied - PM, same unit)=%+.3f  -> + means PM under-prices the up-side" % (
            sum(all_gaps) / len(all_gaps)))
    else:
        print("-> in the SAME unit, BTC-implied prob and PM prob do NOT track well -> relationship is weak")
