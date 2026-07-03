#!/usr/bin/env python3
# scalp_calib.py — does our BTC-implied fair beat PM's quote on 5-min "BTC Up or Down" windows?
# Pulls recent RESOLVED windows (gamma) + BTC klines (Binance) + PM quote path (clob prices-history),
# compares calibration (Brier) of OUR fair vs PM's quote vs the actual outcome, and sims the strategy P&L.
# Read-only, zero money.
import json, math, urllib.request, urllib.parse, time, sys

SIGMA = float(sys.argv[1]) if len(sys.argv) > 1 else 0.0025
MARGIN, PMIN, SPREAD = 0.04, 0.15, 0.01  # 1c spread observed on 5-min windows; entry pays ~half-spread vs mid

def get(url):
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=15) as r:
            return json.load(r)
    except Exception:
        return None

def norm_cdf(z):
    return 0.5 * math.erfc(-z / math.sqrt(2))

def iso_to_unix(s):
    return int(time.mktime(time.strptime(s, "%Y-%m-%dT%H:%M:%SZ")) - time.timezone)

now = time.gmtime()
now_iso = time.strftime("%Y-%m-%dT%H:%M:%SZ", now)
past_iso = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(time.time() - 14*3600))  # last 6h
base = ("https://gamma-api.polymarket.com/markets?closed=true&limit=500&order=endDate&ascending=false"
        f"&end_date_min={past_iso}&end_date_max={now_iso}")
wins = []
for off in range(0, 10000, 500):  # paginate past the 500-cap until we have enough BTC windows
    mk = get(base + f"&offset={off}") or []
    if not mk:
        break
    for m in mk:
        q = (m.get("question") or "").lower()
        if "up or down" not in q or "bitcoin" not in q:
            continue
        try:
            end = iso_to_unix(m["endDate"])
        except Exception:
            continue
        op = m.get("outcomePrices")
        if isinstance(op, str):
            op = json.loads(op)
        toks = m.get("clobTokenIds")
        if isinstance(toks, str):
            toks = json.loads(toks)
        if not op or not toks or len(toks) < 2:
            continue
        wins.append({"end": end, "start": end - 300, "up_won": 1 if str(op[0]) == "1" else 0, "up_tok": toks[0]})
    if len(wins) >= 200:
        break
wins = wins[:200]
print(f"resolved BTC up/down windows pulled: {len(wins)} (sigma={SIGMA})")

n=0; my_brier=0.0; pm_brier=0.0; both=0
# strategy sim
pnl=0.0; trades=0; wins_n=0
samples=[]  # (t_frac, my_fair, pm_q, outcome)
for w in wins:
    # BTC klines 1m for [start-60, end+60]
    kl = get(f"https://api.binance.com/api/v3/klines?symbol=BTCUSDT&interval=1m&startTime={(w['start']-60)*1000}&endTime={(w['end']+60)*1000}")
    if not kl:
        continue
    # open price = close of the candle at/just before window start
    opens = [(int(k[0])//1000, float(k[4])) for k in kl]  # (close_time? use open_time t, close price)
    btc_open = None
    for t,p in opens:
        if t <= w["start"]:
            btc_open = p
    if btc_open is None:
        btc_open = opens[0][1]
    # PM Up-token quote path
    ph = get(f"https://clob.polymarket.com/prices-history?market={w['up_tok']}&startTs={w['start']}&endTs={w['end']}&fidelity=1")
    hist = (ph or {}).get("history") or []
    if not hist:
        continue
    # sample at each PM history point: our fair vs PM quote
    for h in hist:
        t = int(h["t"]); pm_q = float(h["p"])
        tau = w["end"] - t
        if tau < 5 or tau > 300:
            continue
        # BTC price at time t (latest kline <= t)
        btc_t = btc_open
        for tt,pp in opens:
            if tt <= t:
                btc_t = pp
        z = math.log(btc_t / btc_open) / (SIGMA * math.sqrt(max(tau,1)/300.0))
        my = norm_cdf(z)
        o = w["up_won"]
        my_brier += (my-o)**2; pm_brier += (pm_q-o)**2; both += 1
        samples.append((1-tau/300.0, my, pm_q, o))
    n+=1
    # strategy: at each PM point, take underpriced side (our fair vs PM), filter [PMIN,1-PMIN]+margin, hold->settle
    for h in hist:
        t=int(h["t"]); pm_up=float(h["p"]); tau=w["end"]-t
        if tau<5 or tau>300: continue
        btc_t=btc_open
        for tt,pp in opens:
            if tt<=t: btc_t=pp
        z=math.log(btc_t/btc_open)/(SIGMA*math.sqrt(max(tau,1)/300.0)); my=norm_cdf(z)
        up_ask=pm_up; dn_ask=1-pm_up  # approx (no spread data here)
        o=w["up_won"]
        buy_up = PMIN<up_ask<1-PMIN and up_ask < my-MARGIN
        buy_dn = PMIN<dn_ask<1-PMIN and dn_ask < (1-my)-MARGIN
        if buy_up:
            pnl += (1.0 if o==1 else 0.0) - (up_ask + SPREAD/2); trades+=1; wins_n+= (o==1)  # pay ask=mid+half-spread
        elif buy_dn:
            pnl += (1.0 if o==0 else 0.0) - (dn_ask + SPREAD/2); trades+=1; wins_n+= (o==0)
        break  # one trade per window (first qualifying point) to mimic the serial bot

print(f"\n=== CALIBRATION (n={both} sample-points across {n} windows) ===")
if both:
    print(f"  OUR fair  Brier = {my_brier/both:.4f}")
    print(f"  PM quote  Brier = {pm_brier/both:.4f}   (lower=better-calibrated to the outcome)")
    print(f"  -> {'OUR fair beats PM' if my_brier<pm_brier else 'PM beats OUR fair (PM efficient, no edge)'}")
print(f"\n=== STRATEGY SIM (1 trade/window, hold to settle, no spread) ===")
print(f"  trades={trades} wins={wins_n} winrate={wins_n/trades*100 if trades else 0:.0f}% gross_pnl(per $1 stake)=${pnl:.3f}")
# baseline: how often does Up win, and is PM's quote near-calibrated on average
if both:
    avg_my=sum(s[1] for s in samples)/both; avg_pm=sum(s[2] for s in samples)/both; avg_o=sum(s[3] for s in samples)/both
    print(f"\n  avg OUR fair={avg_my:.3f}  avg PM quote={avg_pm:.3f}  actual Up-rate={avg_o:.3f}")
