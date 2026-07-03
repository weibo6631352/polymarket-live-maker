#!/usr/bin/env python3
# analyze_raw.py — OFFLINE analysis of raw_ticks.csv. Uses the REAL PM-resolved outcome (Chainlink truth,
# read from the PM price converging to 1/0 after the window end), NOT a Binance guess. Reconstructs windows,
# replays OKX & Binance seconds-move triggers, fills at the PM ask at our latency, and breaks down win/edge
# by latency / tau / direction / entry-price / with-vs-counter-trend. Run: python3 analyze_raw.py [csv]
import sys, statistics, bisect

CSV = sys.argv[1] if len(sys.argv) > 1 else "/tmp/raw_ticks.csv"
THRESH = 0.0003
LATS = [0, 50, 100, 200, 400]
HL = 0

okx = []   # (t, mid)
binb = []  # (t, mid)
wins = []  # dict: up_tok, dn_tok, open, end, t0 (first win-row time)
pm_rows = []  # (t, side, bid, ask)  (segmented to windows after parse)

for line in open(CSV):
    p = line.rstrip("\n").split(",")
    if len(p) < 3:
        continue
    try:
        t = float(p[0]); kind = p[1]
        if kind == "okx":
            okx.append((t, (float(p[2]) + float(p[3])) / 2))
        elif kind == "bin":
            binb.append((t, (float(p[2]) + float(p[3])) / 2))
        elif kind == "pm":
            pm_rows.append((t, p[2], float(p[3]) if p[3] else 0.0, float(p[4]) if p[4] else 0.0))
        elif kind == "win":
            wins.append({"up": p[2], "dn": p[3], "open": float(p[4]), "end": float(p[5]) * 1000, "t0": t})
    except Exception:
        continue

print("parsed: okx=%d bin=%d pm=%d windows=%d" % (len(okx), len(binb), len(pm_rows), len(wins)))
if len(wins) < 1:
    print("not enough windows yet"); sys.exit()

okx.sort(); binb.sort(); pm_rows.sort()
ot = [x[0] for x in okx]; bt = [x[0] for x in binb]

def at(series, times, t):
    i = bisect.bisect_right(times, t) - 1
    return series[i][1] if i >= 0 else None
def move(series, times, t, win=3000):
    n = at(series, times, t); a = at(series, times, t - win)
    return (n / a - 1) if (n and a) else 0.0
def ask_side(rows, side, t):  # latest ask for that side at or before t
    best = None
    for r in rows:
        if r[0] <= t and r[1] == side:
            best = r[3]
        elif r[0] > t:
            break
    return best
def price_side(rows, side, t):  # latest mid for that side at/before t (for outcome read)
    best = None
    for r in rows:
        if r[0] <= t and r[1] == side:
            best = (r[2] + r[3]) / 2 if r[3] else r[2]
    return best

# segment pm_rows by window [t0, next t0)
wins.sort(key=lambda w: w["t0"])
entries = []
for wi, w in enumerate(wins):
    lo = w["t0"]; hi = wins[wi + 1]["t0"] if wi + 1 < len(wins) else 1e20
    rows = [r for r in pm_rows if lo <= r[0] < hi]
    if not rows:
        continue
    # REAL outcome: the side whose price -> ~1 by the LAST tick after the end
    up_last = price_side(rows, "up", 1e20); dn_last = price_side(rows, "dn", 1e20)
    if up_last is None and dn_last is None:
        continue
    up_won = (up_last or 0) >= (dn_last or 0) if (up_last and dn_last) else ((up_last or 0) > 0.5)
    # only trust the outcome if it actually resolved (a side near 0/1)
    resolved = max(up_last or 0, dn_last or 0) > 0.9 or min(up_last or 1, dn_last or 1) < 0.1
    # replay triggers during the window (up to end - 20s)
    for src, series, times in (("okx", okx, ot), ("bin", binb, bt)):
        last_trig = 0
        ts = [x[0] for x in series if lo <= x[0] <= w["end"] - 20000]
        for t in ts:
            if t - last_trig < 2000:
                continue
            mv = move(series, times, t)
            if abs(mv) <= THRESH:
                continue
            last_trig = t
            up = mv > 0
            side = "up" if up else "dn"
            fav_ask = ask_side(rows, side, t)
            if not fav_ask or not (0.03 < fav_ask < 0.97):
                continue
            tau = (w["end"] - t) / 1000
            drift = (at(binb, bt, t) or 0) - w["open"]
            wt = (drift > 0) == up
            won = 1 if (up == up_won) else 0
            # fills at each latency
            fills = {}
            for L in LATS:
                a = ask_side(rows, side, t + L)
                if a and 0.03 < a < 0.97:
                    fills[L] = a
            if HL in fills and resolved:
                entries.append({"src": src, "tau": tau, "up": up, "won": won, "wt": wt, "fills": fills})

print("replayed entries (REAL PM-resolved outcome): %d" % len(entries))
if not entries:
    print("no resolved entries yet — let the recorder capture more windows"); sys.exit()

def pct(x, n):
    return 100 * x // max(n, 1)
def eg(e, L=HL):
    return e["won"] - e["fills"].get(L, e["fills"][HL])
for src in ("okx", "bin"):
    es = [e for e in entries if e["src"] == src]
    print("\n######## %s (REAL outcome) ########" % src.upper())
    if not es:
        print("  none"); continue
    n = len(es); w = sum(e["won"] for e in es); tot = sum(eg(e) for e in es)
    print("  OVERALL n=%d win=%d%% edge/order=%+.4f total=$%.2f" % (n, pct(w, n), tot / n, tot))
    print("  TAU:   " + "  ".join("%s:n%d/%d%%/%+.3f" % (lab, len([e for e in es if a <= e["tau"] < b]),
        pct(sum(e["won"] for e in es if a <= e["tau"] < b), max(len([e for e in es if a <= e["tau"] < b]), 1)),
        sum(eg(e) for e in es if a <= e["tau"] < b) / max(len([e for e in es if a <= e["tau"] < b]), 1))
        for a, b, lab in [(0, 90, "<90s"), (90, 180, "90-180"), (180, 320, ">180")]))
    print("  ASK:   " + "  ".join("%.2f-%.2f:n%d/%d%%/%+.3f" % (a, b, len([e for e in es if a <= e["fills"][HL] < b]),
        pct(sum(e["won"] for e in es if a <= e["fills"][HL] < b), max(len([e for e in es if a <= e["fills"][HL] < b]), 1)),
        sum(eg(e) for e in es if a <= e["fills"][HL] < b) / max(len([e for e in es if a <= e["fills"][HL] < b]), 1))
        for a, b in [(0.03, 0.35), (0.35, 0.55), (0.55, 0.75), (0.75, 0.97)]))
    print("  TREND: " + "  ".join("%s:n%d/%d%%/%+.3f" % (lab, len([e for e in es if e["wt"] == v]),
        pct(sum(e["won"] for e in es if e["wt"] == v), max(len([e for e in es if e["wt"] == v]), 1)),
        sum(eg(e) for e in es if e["wt"] == v) / max(len([e for e in es if e["wt"] == v]), 1))
        for v, lab in [(True, "with-trend"), (False, "counter")]))
    cheap = [e for e in es if e["fills"][HL] < 0.55]
    print("  CHEAP x TREND: " + "  ".join("%s:n%d/%d%%/%+.3f" % (lab, len([e for e in cheap if e["wt"] == v]),
        pct(sum(e["won"] for e in cheap if e["wt"] == v), max(len([e for e in cheap if e["wt"] == v]), 1)),
        sum(eg(e) for e in cheap if e["wt"] == v) / max(len([e for e in cheap if e["wt"] == v]), 1))
        for v, lab in [(True, "with"), (False, "counter")]))
