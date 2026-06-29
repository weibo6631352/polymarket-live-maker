#!/usr/bin/env python3
# reverse_0xf418.py — reverse-engineer the broad-edge crypto winners' signal. For each of their crypto
# BUYs, fetch Binance aggTrades (tick) in the seconds BEFORE the trade and the window-open price, and ask:
# did Binance BTC just move (last ~5s) in the direction of the side they bought (Up vs Down)? And did it
# drift that way since the window open? High seconds-scale alignment => they snipe the seconds-scale Binance
# move (replicable if we're fast); low => their edge is something else (opaque). Read-only, zero money.
import json, urllib.request, time, calendar, collections

def get(u):
    try:
        return json.load(urllib.request.urlopen(urllib.request.Request(u, headers={"User-Agent": "Mozilla/5.0"}), timeout=20))
    except Exception:
        return None

ACCTS = ["0xf418d3a1a941", "0xf3531b23b504", "0xf6ab262d360c", "0x674887d1ac"]
# resolve full addresses from the recent tape
full = {}
for off in range(0, 4000, 500):
    d = get("https://data-api.polymarket.com/trades?limit=500&offset=%d" % off) or []
    for t in d:
        for pre in ACCTS:
            if t["proxyWallet"].startswith(pre):
                full[pre] = t["proxyWallet"]
    if len(full) == len(ACCTS):
        break

def binance_price_at(ms):
    # last aggTrade price at or before ms (1s window lookback)
    at = get("https://api.binance.com/api/v3/aggTrades?symbol=BTCUSDT&startTime=%d&endTime=%d" % (ms - 1500, ms))
    if at:
        return float(at[-1]["p"])
    return None

def kline_open(end_unix):
    kl = get("https://api.binance.com/api/v3/klines?symbol=BTCUSDT&interval=1m&startTime=%d&limit=1" % (int((end_unix - 300) * 1000)))
    return float(kl[0][1]) if kl else None

drift_aligned = drift_tot = 0
sec_aligned = sec_tot = 0
for pre, addr in full.items():
    tr = get("https://data-api.polymarket.com/trades?user=%s&limit=200" % addr) or []
    cbuys = [t for t in tr if t.get("side") == "BUY" and "up or down" in (t.get("title") or "").lower()]
    # need each market's end time
    conds = list(set(t["conditionId"] for t in cbuys))
    endmap = {}
    for i in range(0, len(conds), 20):
        q = "&".join("condition_ids=%s" % c for c in conds[i:i+20])
        for mm in (get("https://gamma-api.polymarket.com/markets?limit=100&" + q) or []):
            try:
                endmap[mm.get("conditionId")] = calendar.timegm(time.strptime(mm["endDate"], "%Y-%m-%dT%H:%M:%SZ"))
            except Exception:
                pass
    for t in cbuys[:40]:
        c = t["conditionId"]
        if c not in endmap:
            continue
        end = endmap[c]
        tt = int(t.get("timestamp", 0))
        side = t.get("outcome", "")  # 'Up' or 'Down'
        opn = kline_open(end)
        p_now = binance_price_at(tt * 1000)
        p_5s = binance_price_at((tt - 5) * 1000)
        if not opn or not p_now or not p_5s:
            continue
        # drift since open
        drift = p_now - opn
        if abs(drift) > 1:
            drift_tot += 1
            drift_aligned += ((side == "Up" and drift > 0) or (side == "Down" and drift < 0))
        # seconds-scale move (last 5s)
        sec = p_now - p_5s
        if abs(sec) > 1:
            sec_tot += 1
            sec_aligned += ((side == "Up" and sec > 0) or (side == "Down" and sec < 0))
print("accounts:", list(full.keys()))
print("DRIFT-since-open alignment: %d/%d = %d%% (their side matches BTC drift)" % (
    drift_aligned, drift_tot, 100 * drift_aligned // max(drift_tot, 1)))
print("SECONDS-scale (last 5s) alignment: %d/%d = %d%% (>>50%% = they snipe the seconds move)" % (
    sec_aligned, sec_tot, 100 * sec_aligned // max(sec_tot, 1)))
