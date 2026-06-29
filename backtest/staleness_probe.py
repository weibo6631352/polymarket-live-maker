#!/usr/bin/env python3
# staleness_probe.py — THE make-or-break measurement for crypto-micro sniping.
# Synchronously logs Kraken BTC spot (WSS) + PM CLOB best_ask/best_bid (WSS price_change carries
# best_bid/best_ask directly). Then cross-correlates BTC returns vs PM-mid returns across lags to find
# HOW LONG PM's quote lags a BTC move (= the stale-quote window). If that window >> our order latency
# we can snipe; if PM tracks BTC within a few ms, we can't. Read-only, no orders, zero money.
import json, ssl, time, threading, urllib.request, websocket

def hg(u):
    return urllib.request.urlopen(urllib.request.Request(u, headers={"User-Agent": "Mozilla/5.0"}), timeout=10).read().decode()
def now_ms():
    return time.time() * 1000.0

RUN_MS = 180000.0  # 3 min — stay within the fresh window's responsive life
start = now_ms()
events = []           # (t_ms, kind, a, b)   kind 'btc': a=px ; 'pm': a=ask b=bid
lock = threading.Lock()

def kraken():
    while now_ms() - start < RUN_MS:
        try:
            ws = websocket.create_connection("wss://ws.kraken.com/v2", sslopt={"cert_reqs": ssl.CERT_NONE})
            ws.send(json.dumps({"method": "subscribe", "params": {"channel": "ticker", "symbol": ["BTC/USD"]}}))
            ws.settimeout(5)
            while now_ms() - start < RUN_MS:
                try:
                    m = ws.recv()
                except Exception:
                    continue
                try:
                    j = json.loads(m)
                    if j.get("channel") == "ticker":
                        px = float(j["data"][0]["last"])
                        with lock:
                            events.append((now_ms(), "btc", px, 0.0))
                except Exception:
                    pass
            ws.close()
        except Exception:
            time.sleep(1)

def pm():
    import calendar
    iso = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    nowu = time.time()
    cands = []
    for m in json.loads(hg("https://gamma-api.polymarket.com/markets?closed=false&limit=200&order=endDate&ascending=true&end_date_min=" + iso)):
        ql = (m.get("question") or "").lower()
        if "up or down" in ql and "bitcoin" in ql and m.get("acceptingOrders"):
            try:
                end = calendar.timegm(time.strptime(m["endDate"], "%Y-%m-%dT%H:%M:%SZ"))
            except Exception:
                continue
            tau = end - nowu
            tks = json.loads(m.get("clobTokenIds") or "[]")
            if tks and 195 <= tau <= 295:  # fresh-ish, responsive window (not about to resolve)
                cands.append((tau, tks[0], m.get("question")))
    if not cands:
        print("PM: no fresh BTC window (tau 195-295s) right now")
        return
    cands.sort(reverse=True)  # freshest first
    tok = cands[0][1]
    print("PM picked tau=%ds %s" % (cands[0][0], cands[0][2]))
    while now_ms() - start < RUN_MS:
        try:
            ws = websocket.create_connection("wss://ws-subscriptions-clob.polymarket.com/ws/market", sslopt={"cert_reqs": ssl.CERT_NONE})
            ws.send(json.dumps({"assets_ids": [tok], "type": "market"}))
            ws.settimeout(5)
            lastping = now_ms()
            while now_ms() - start < RUN_MS:
                if now_ms() - lastping > 9000:
                    try:
                        ws.send("PING")
                    except Exception:
                        pass
                    lastping = now_ms()
                try:
                    m = ws.recv()
                except Exception:
                    continue
                try:
                    j = json.loads(m)
                    ev = j[0] if isinstance(j, list) and j else j
                    if ev.get("event_type") == "price_change":
                        for pc in ev.get("price_changes", []):
                            if pc.get("asset_id") == tok:
                                a = float(pc.get("best_ask", 0) or 0)
                                b = float(pc.get("best_bid", 0) or 0)
                                if a > 0 and b > 0:
                                    with lock:
                                        events.append((now_ms(), "pm", a, b))
                except Exception:
                    pass
            ws.close()
        except Exception:
            time.sleep(1)

t1 = threading.Thread(target=kraken)
t2 = threading.Thread(target=pm)
t1.start(); t2.start(); t1.join(); t2.join()

evs = sorted(events, key=lambda e: e[0])
btc = [(e[0], e[2]) for e in evs if e[1] == "btc"]
pm_ = [(e[0], (e[2] + e[3]) / 2.0) for e in evs if e[1] == "pm"]
print("collected btc=%d pm=%d over %.0fs" % (len(btc), len(pm_), (now_ms() - start) / 1000))
if btc:
    bv = [v for _, v in btc]
    print("BTC range: %.1f - %.1f  (move %.3f%%)" % (min(bv), max(bv), 100 * (max(bv) / min(bv) - 1)))
if pm_:
    pv = [v for _, v in pm_]
    print("PM mid range: %.3f - %.3f  (distinct mids=%d)" % (min(pv), max(pv), len(set(round(v, 4) for v in pv))))
if len(btc) < 30 or len(pm_) < 30:
    print("too few samples for lag analysis")
else:
    BIN = 500.0  # ms (coarser than 100ms to cut tick/quote noise)
    t0 = evs[0][0]; tN = evs[-1][0]
    nb = int((tN - t0) / BIN) + 1
    def grid(series):
        g = [None] * nb
        for t, v in series:
            g[min(int((t - t0) / BIN), nb - 1)] = v
        last = None
        for i in range(nb):
            if g[i] is None:
                g[i] = last
            else:
                last = g[i]
        return g
    gb = grid(btc); gp = grid(pm_)
    rb = [(gb[i] / gb[i-1] - 1) if (gb[i] and gb[i-1]) else 0.0 for i in range(1, nb)]
    rp = [(gp[i] / gp[i-1] - 1) if (gp[i] and gp[i-1]) else 0.0 for i in range(1, nb)]
    import math
    def corr(x, y):
        n = len(x)
        mx = sum(x) / n; my = sum(y) / n
        sxy = sum((x[i]-mx)*(y[i]-my) for i in range(n))
        sxx = sum((v-mx)**2 for v in x); syy = sum((v-my)**2 for v in y)
        return sxy / math.sqrt(sxx*syy) if sxx > 0 and syy > 0 else 0.0
    print("=== BTC->PM lag cross-correlation (bin=%dms) ===" % int(BIN))
    print("lag_ms  corr(BTC_ret[t], PM_ret[t+lag])")
    best = (0, 0.0)
    for lag in range(0, 13):
        x = rb[:len(rb)-lag] if lag > 0 else rb
        y = rp[lag:]
        n = min(len(x), len(y))
        c = corr(x[:n], y[:n])
        if abs(c) > abs(best[1]):
            best = (int(lag*BIN), c)
        print("  %4d   %+.3f" % (int(lag*BIN), c))
    print("PEAK correlation at lag=%dms (corr=%+.3f)  <- PM's typical response lag to BTC" % best)
