#!/usr/bin/env python3
"""Calibration/Brier of PM slow crypto date markets at T-7d/T-3d/T-1d,
by price bucket and family; plus retrospective realized-vol fair for terminal dailies."""
import json, math, os, sys, time
from collections import defaultdict
import requests

HERE = os.path.dirname(os.path.abspath(__file__))
rows = [json.loads(l) for l in open(os.path.join(HERE, "bt_rows.jsonl"))]
# dedupe by tok
seen = {}
for r in rows:
    seen[r["tok"]] = r
rows = list(seen.values())
print(f"rows {len(rows)}")

# join book-birth time from event cache; a snapshot price is only valid if the
# book is at least MIN_AGE old at snapshot time (else it's the open-dust quote)
from datetime import datetime
birth = {}
ev = json.load(open(os.path.join(HERE, "bt_events.json")))
for sl, e in ev.items():
    if not e:
        continue
    for m in e.get("markets", []):
        try:
            tok = json.loads(m["clobTokenIds"])[0]
        except Exception:
            continue
        ts = m.get("acceptingOrdersTimestamp") or m.get("startDate") or m.get("createdAt")
        if ts:
            try:
                birth[tok] = datetime.fromisoformat(ts.replace("Z", "+00:00")).timestamp()
            except Exception:
                pass

MIN_AGE = 24 * 3600
nkill = 0
for r in rows:
    b = birth.get(r["tok"])
    for lbl, dt in (("p7", 7), ("p3", 3), ("p1", 1)):
        if r.get(lbl) is None:
            continue
        snap = r["T"] - dt * 86400
        if b is None or snap - b < MIN_AGE:
            r[lbl] = None
            nkill += 1
print(f"snapshots dropped by 24h book-age filter: {nkill}")

# ---- Binance hourly klines for spot at snapshot times ----
KL = os.path.join(HERE, "klines.json")
if os.path.exists(KL):
    kl = json.load(open(KL))
else:
    kl = {}
    for coin, sym in (("BTC", "BTCUSDT"), ("ETH", "ETHUSDT")):
        bars = []
        start = int(time.time() * 1000) - 120 * 86400 * 1000
        while True:
            r = requests.get("https://data-api.binance.vision/api/v3/klines",
                             params=dict(symbol=sym, interval="1h", startTime=start, limit=1000),
                             timeout=30).json()
            if not r:
                break
            bars.extend(r)
            start = r[-1][6] + 1
            if len(r) < 1000:
                break
            time.sleep(0.2)
        kl[coin] = [[b[0] / 1000, float(b[1]), float(b[2]), float(b[3]), float(b[4])] for b in bars]
    json.dump(kl, open(KL, "w"))
print("klines", {k: len(v) for k, v in kl.items()})

def spot_at(coin, ts):
    bars = kl[coin]
    lo, hi = 0, len(bars) - 1
    while lo < hi:
        mid = (lo + hi) // 2
        if bars[mid][0] < ts:
            lo = mid + 1
        else:
            hi = mid
    return bars[max(0, lo - 1)][4]

def rv30(coin, ts):
    """trailing 30d annualized realized vol from hourly closes."""
    bars = [b for b in kl[coin] if ts - 30 * 86400 <= b[0] < ts]
    if len(bars) < 200:
        return None
    lr = [math.log(b2[4] / b1[4]) for b1, b2 in zip(bars, bars[1:])]
    m = sum(lr) / len(lr)
    v = sum((x - m) ** 2 for x in lr) / (len(lr) - 1)
    return math.sqrt(v * 24 * 365.25)

# ---- calibration tables ----
BUCKETS = [(0, .01), (.01, .02), (.02, .05), (.05, .10), (.10, .25), (.25, .5),
           (.5, .75), (.75, .90), (.90, .95), (.95, .98), (.98, .99), (.99, 1.001)]

def calib(rows, lbl, fam_filter=None):
    agg = defaultdict(lambda: [0, 0.0, 0.0, 0.0])  # n, sum_p, sum_y, brier
    for r in rows:
        p = r.get(lbl)
        if p is None:
            continue
        if fam_filter and r["fam"] not in fam_filter:
            continue
        for lo, hi in BUCKETS:
            if lo <= p < hi:
                a = agg[(lo, hi)]
                a[0] += 1; a[1] += p; a[2] += r["outcome"]; a[3] += (p - r["outcome"]) ** 2
                break
    return agg

def show(agg, title):
    print(f"\n== {title} ==")
    print(f"{'bucket':>12} {'n':>5} {'avg_p':>7} {'freq_y':>7} {'bias':>7} {'brier':>7}")
    tn = tb = 0
    for (lo, hi) in BUCKETS:
        if (lo, hi) not in agg:
            continue
        n, sp, sy, br = agg[(lo, hi)]
        tn += n; tb += br
        print(f"{lo:>5.2f}-{hi:<6.2f} {n:>5} {sp/n:>7.3f} {sy/n:>7.3f} {sy/n-sp/n:>+7.3f} {br/n:>7.4f}")
    if tn:
        print(f"{'TOTAL':>12} {tn:>5} {'':>7} {'':>7} {'':>7} {tb/tn:>7.4f}")

for lbl in ("p7", "p3", "p1"):
    show(calib(rows, lbl), f"ALL families @ {lbl}")
show(calib(rows, "p3", {"daily_terminal"}), "daily_terminal @ p3")
show(calib(rows, "p3", {"daily_touch"}), "daily_touch @ p3")
show(calib(rows, "p3", {"weekly_touch", "monthly_touch"}), "weekly+monthly touch @ p3")

# ---- retrospective RV-model fair for daily terminal markets @ p3 ----
SQ2 = math.sqrt(2.0)
N = lambda x: 0.5 * math.erfc(-x / SQ2)
print("\n== RV-retro model vs PM @ p3 (daily_terminal only; trailing 30d RV, zero drift) ==")
print("NOTE: inferior to true historical IV: RV lacks the vol risk premium (IV~1.1-1.4x RV) and skew.")
edges = []
for r in rows:
    if r["fam"] != "daily_terminal" or r.get("p3") is None:
        continue
    ts = r["T"] - 3 * 86400
    Sx = spot_at(r["coin"], ts)
    sig = rv30(r["coin"], ts)
    if not Sx or not sig:
        continue
    tau = 3 * 86400 / (365.25 * 86400)
    d2 = (math.log(Sx / r["K"]) - 0.5 * sig * sig * tau) / (sig * math.sqrt(tau))
    pf = N(d2)
    r["rv_fair_p3"] = pf
    edges.append((pf - r["p3"], pf, r))

# does trading model-vs-PM disagreement make money? buy YES if fair>pm+thr, NO if fair<pm-thr, at p3 price (mid proxy)
for thr in (0.02, 0.04, 0.08):
    pnl = n = 0.0
    for e, pf, r in edges:
        p = r["p3"]; y = r["outcome"]
        if e > thr:
            pnl += (y - p); n += 1
        elif e < -thr:
            pnl += (p - y); n += 1
    print(f"thr={thr:.2f}: trades={n:.0f} pnl/trade={pnl/max(n,1):+.4f} (at PM p3 price, no spread/fee)")

# ---- tail deep-dive: pooled expected-vs-observed with direction + clustering ----
print("\n== TAIL calibration (p in (0,0.05]) pooled, by direction & family ==")
import itertools
def tail_stats(sel, lbl):
    exp = sum(r[lbl] for r in sel)
    obs = sum(r["outcome"] for r in sel)
    # cluster by coin-week (correlated crypto moves)
    clus = defaultdict(lambda: [0.0, 0.0])
    for r in sel:
        wk = int(r["T"] // (7 * 86400))
        c = clus[(r["coin"], wk)]
        c[0] += r[lbl]; c[1] += r["outcome"]
    return exp, obs, len(sel), len(clus)

for lbl in ("p3", "p1"):
    sel = [r for r in rows if r.get(lbl) is not None and 0 < r[lbl] <= 0.05]
    if not sel:
        continue
    for direc, g in (("up (reach/above)", [r for r in sel if r["kind"] in ("terminal_gt", "touch_up")]),
                     ("down (dip)", [r for r in sel if r["kind"] == "touch_dn"])):
        if not g:
            continue
        exp, obs, n, ncl = tail_stats(g, lbl)
        print(f"  {lbl} {direc:<18} n={n:>4} clusters={ncl:>3} priced-in hits={exp:6.1f} realized hits={obs:3.0f}")

# sell-all-tails P&L: sell YES at (p - 1 tick) for every tail snapshot
for lbl in ("p3", "p1"):
    sel = [r for r in rows if r.get(lbl) is not None and 0.005 < r[lbl] <= 0.05]
    if not sel:
        continue
    pnl = sum((r[lbl] - 0.005) - r["outcome"] for r in sel)  # per-share, capital ~ (1-p)
    cap = sum(1 - r[lbl] for r in sel)
    print(f"  SELL-TAILS @{lbl}: n={len(sel)} pnl/share_sum={pnl:+.1f} on capital {cap:.0f} -> {100*pnl/cap:+.2f}% per cycle")

# per-bucket bias of RV model vs PM to see who is off
agg = defaultdict(lambda: [0, 0.0, 0.0, 0.0])
for e, pf, r in edges:
    p = r["p3"]
    for lo, hi in BUCKETS:
        if lo <= p < hi:
            a = agg[(lo, hi)]
            a[0] += 1; a[1] += pf; a[2] += r["outcome"]; a[3] += (pf - r["outcome"]) ** 2
            break
show(agg, "RV-model fair by PM-p3 bucket (avg_p = model, freq_y = realized)")
json.dump(rows, open(os.path.join(HERE, "bt_rows_enriched.json"), "w"))
