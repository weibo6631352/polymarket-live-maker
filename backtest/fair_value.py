#!/usr/bin/env python3
# fair_value.py — compute the model fair value P_fair(Up) on the recorded data AND CALIBRATE sigma (the #1
# make-or-break per the quant expert: does P_fair=0.7 actually win ~70%?). Model: z = (ln(S/K) - 0.5*sig^2*tau)
# / sqrt(sig^2*tau + sigb^2); P_fair = Phi(z) (Gaussian first — if it's over-confident at the extremes that
# CONFIRMS the fat-tail need). sig = realized 1s vol of the window; we SWEEP a scale c on sig and pick the c
# that minimizes the Brier score = best-calibrated. Outcome = the REAL PM-resolved (Chainlink) winner.
# S,K from Binance (the better Chainlink proxy). Run: python3 fair_value.py [csv]
import sys, math, bisect
CSV = sys.argv[1] if len(sys.argv) > 1 else "/tmp/raw_ticks.csv"
SAMPLE = 8000          # ms between calibration samples within a window
SIGB = 0.0             # chainlink-basis stdev (price fraction); start 0, raise if endgame miscalibrates
CSCAN = [0.5, 0.7, 0.85, 1.0, 1.2, 1.5, 2.0, 3.0]

binb = []; pm_rows = []; wins = []
for line in open(CSV):
    p = line.rstrip("\n").split(",")
    if len(p) < 3:
        continue
    try:
        t = float(p[0]); k = p[1]
        if k == "bin":
            binb.append((t, (float(p[2]) + float(p[3])) / 2))
        elif k == "pm":
            pm_rows.append((t, p[2], float(p[3]) if p[3] else 0.0, float(p[4]) if p[4] else 0.0))
        elif k == "win":
            wins.append({"up": p[2], "dn": p[3], "open": float(p[4]), "end": float(p[5]) * 1000, "t0": t})
    except Exception:
        continue
binb.sort(); pm_rows.sort(); wins.sort(key=lambda w: w["t0"])
bt = [x[0] for x in binb]
def Phi(x):
    return 0.5 * (1 + math.erf(x / math.sqrt(2)))
def bat(t):
    i = bisect.bisect_right(bt, t) - 1
    return binb[i][1] if i >= 0 else None
def price_side(rows, side, t):
    best = None
    for r in rows:
        if r[0] <= t and r[1] == side:
            best = (r[2] + r[3]) / 2 if r[3] else r[2]
    return best

samples = {c: [] for c in CSCAN}   # list of (P_fair, real_outcome)
nwin = 0
for wi, w in enumerate(wins):
    lo = w["t0"]; hi = wins[wi + 1]["t0"] if wi + 1 < len(wins) else 1e20
    rows = [r for r in pm_rows if lo <= r[0] < hi]
    if not rows or w["open"] <= 0:
        continue
    up_last = price_side(rows, "up", 1e20); dn_last = price_side(rows, "dn", 1e20)
    if up_last is None or dn_last is None:
        continue
    if not (max(up_last, dn_last) > 0.9 or min(up_last, dn_last) < 0.1):   # resolved only
        continue
    up_won = 1 if up_last >= dn_last else 0
    # realized 1s sigma of Binance mid over the window
    lr = []; lt = None; lp = None
    for t, m in binb:
        if lo <= t <= w["end"]:
            if lt is None or t - lt >= 1000:
                if lp:
                    lr.append(math.log(m / lp))
                lt = t; lp = m
    if len(lr) < 5:
        continue
    mu = sum(lr) / len(lr); sig1 = (sum((x - mu) ** 2 for x in lr) / len(lr)) ** 0.5
    if sig1 <= 0:
        continue
    nwin += 1
    t = lo + 5000
    while t < w["end"] - 15000:
        S = bat(t); tau = (w["end"] - t) / 1000.0
        if S and tau > 10:
            for c in CSCAN:
                sig = sig1 * c
                var = sig * sig * tau + SIGB * SIGB
                if var > 0:
                    z = (math.log(S / w["open"]) - 0.5 * sig * sig * tau) / math.sqrt(var)
                    z = max(-6.0, min(6.0, z))
                    samples[c].append((Phi(z), up_won))
        t += SAMPLE

print("calibrated on %d resolved windows, %d samples/c" % (nwin, len(samples[CSCAN[0]])))
if not samples[CSCAN[0]]:
    print("no samples yet — let the recorder capture more"); sys.exit()
print("\nsig-scale c   Brier(↓ better)   meanP_fair   actual_up_rate")
best_c = None; best_b = 9
for c in CSCAN:
    s = samples[c]
    if not s:
        continue
    brier = sum((pf - o) ** 2 for pf, o in s) / len(s)
    mp = sum(pf for pf, o in s) / len(s); ar = sum(o for pf, o in s) / len(s)
    print("   %.2f         %.4f          %.3f        %.3f" % (c, brier, mp, ar))
    if brier < best_b:
        best_b = brier; best_c = c
print("\nBEST c=%.2f (Brier %.4f). RELIABILITY (does P_fair match realized?):" % (best_c, best_b))
s = samples[best_c]
for a, b in [(0, .1), (.1, .2), (.2, .3), (.3, .4), (.4, .5), (.5, .6), (.6, .7), (.7, .8), (.8, .9), (.9, 1.01)]:
    bk = [o for pf, o in s if a <= pf < b]
    if bk:
        print("  P_fair %.1f-%.1f : n=%4d  realized=%3.0f%%  (ideal %2.0f%%)  %s" % (
            a, b, len(bk), 100 * sum(bk) / len(bk), 100 * (a + b) / 2,
            "OK" if abs(sum(bk) / len(bk) - (a + b) / 2) < 0.15 else "<-- off"))
print("\n-> calibrated if realized≈ideal across buckets. If extremes are over-confident (P_fair 0.9 wins <90%%)")
print("   that's the FAT TAIL -> Student-t needed. c!=1 means raw realized-vol sigma is biased.")
