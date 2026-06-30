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
ALATS = [0, 17, 50, 100, 200, 400]    # latencies to probe the favored-side ASK after the trigger (competition proxy)

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
pmt = {s: [r[0] for r in pm[s]] for s in pm}   # precompute time arrays ONCE (perf: was O(n) rebuild per call)
def bidask(side, t):  # latest (bid,ask) for side at/before t
    rows = pm[side]; i = bisect.bisect_right(pmt[side], t) - 1
    return (rows[i][1], rows[i][2]) if i >= 0 else (None, None)
def mid_at(t):
    i = bisect.bisect_right(ot, t) - 1
    return okx[i][1] if i >= 0 else None

scalps = {h: [] for h in HOLDS}
trigs = []   # per-trigger: {wt, ask, <hold>: scalp}
ntrig = 0
BEST = 1500  # the hold to break down by filter
for wi, w in enumerate(wins):
    lo = w["t0"]; hi = wins[wi + 1]["t0"] if wi + 1 < len(wins) else w["end"] + 60000
    last = 0
    for idx in range(bisect.bisect_left(ot, lo), bisect.bisect_right(ot, w["end"] - 20000)):
        t, m = okx[idx]
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
        drift = (mid_at(t) or 0) - w["open"]
        rec = {"wt": (drift > 0) == (mv > 0), "ask": entry_ask, "side": side, "traj": {}}
        for al in ALATS:
            _, a2 = bidask(side, t + al)
            if a2 and a2 > 0:
                rec["traj"][al] = a2
        for h in HOLDS:
            exit_bid, _ = bidask(side, t + LAT + h)     # we exit by selling the bid
            if exit_bid and exit_bid > 0:
                scalps[h].append(exit_bid - entry_ask)  # net scalp per share (spread already in)
                rec[h] = exit_bid - entry_ask
        trigs.append(rec)

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

def show(name, sub):
    s = [r[BEST] for r in sub if BEST in r]
    if not s:
        print("  %-22s (none)" % name); return
    print("  %-22s n=%d  mean=%+.4f  win=%d%%" % (name, len(s), sum(s) / len(s), 100 * sum(1 for x in s if x > 0) // len(s)))
print("\n=== scalp @ %dms hold, by FILTER (does filtering cut the losers?) ===" % BEST)
show("with-trend", [r for r in trigs if r["wt"]])
show("counter-trend", [r for r in trigs if not r["wt"]])
show("ask<0.55 (cheap)", [r for r in trigs if r["ask"] < 0.55])
show("ask>=0.55 (expensive)", [r for r in trigs if r["ask"] >= 0.55])
show("with-trend & ask>=0.55", [r for r in trigs if r["wt"] and r["ask"] >= 0.55])

print("\n=== BOTH SIDES? (did we trade & profit on Up-token AND Dn-token, or one-sided?) ===")
show("BUY up-token (BTC up)", [r for r in trigs if r["side"] == "up"])
show("BUY dn-token (BTC dn)", [r for r in trigs if r["side"] == "dn"])
show("cheap & up-token", [r for r in trigs if r["side"] == "up" and r["ask"] < 0.55])
show("cheap & dn-token", [r for r in trigs if r["side"] == "dn" and r["ask"] < 0.55])

print("\n=== ASK PERSISTENCE after the move (cheap triggers) — does the cheap ask survive our latency? ===")
cheap = [r for r in trigs if r["ask"] < 0.55 and r["traj"]]
for al in ALATS:
    vals = [r["traj"][al] for r in cheap if al in r["traj"]]
    if vals:
        print("  fav ask @ +%3dms : avg=%.4f  n=%d" % (al, sum(vals) / len(vals), len(vals)))
print("  -> flat across 0-200ms = cheap ask persists (we have time); rises fast = faster snipers take it (competition)")

print("\n=== STATISTICAL SIGNIFICANCE (cheap scalp @ %dms, the headline edge) ===" % BEST)
cs = [r[BEST] for r in trigs if r["ask"] < 0.55 and BEST in r]
if cs:
    n = len(cs); mean = sum(cs) / n
    sd = (sum((x - mean) ** 2 for x in cs) / (n - 1)) ** 0.5 if n > 1 else 0.0
    se = sd / math.sqrt(n) if n else 0.0
    t = mean / se if se else 0.0
    wins = sum(1 for x in cs if x > 0); wr = wins / n
    z = 1.96; den = 1 + z * z / n
    cen = (wr + z * z / (2 * n)) / den
    half = z * math.sqrt(wr * (1 - wr) / n + z * z / (4 * n * n)) / den
    avgstake = sum(r["ask"] for r in trigs if r["ask"] < 0.55 and BEST in r) / n
    print("  n=%d trigs  mean=%+.4f/share  sd=%.4f  SE=%.4f" % (n, mean, sd, se))
    print("  t=%.2f  -> %s" % (t, "SIGNIFICANT (|t|>2, p<~0.05)" if abs(t) > 2 else "not yet significant"))
    print("  win=%.0f%%  Wilson95%%CI=[%.0f%%, %.0f%%]  %s" % (
        100 * wr, 100 * (cen - half), 100 * (cen + half), "excludes 50%" if (cen - half) > 0.5 else "includes 50%"))
    print("  per 5-share trade: $%+.3f gross   (mean = %.0f%% of the ~%.2f stake)" % (mean * 5, 100 * mean / avgstake, avgstake))
