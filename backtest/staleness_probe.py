#!/usr/bin/env python3
# staleness_probe.py — THE make-or-break measurement for crypto-micro sniping.
# Picks the FRESHEST responsive BTC up/down window, streams Kraken BTC (WSS) + PM CLOB best_ask/bid
# (WSS price_change carries best_bid/best_ask), then measures how long PM's quote lags a BTC move
# (cross-correlation lag) AND, per BTC jump, the ask mispricing magnitude vs the spread (the profit
# number). Read-only, no orders, zero money.
import json, ssl, time, threading, calendar, urllib.request, websocket

def hg(u):
    return urllib.request.urlopen(urllib.request.Request(u, headers={"User-Agent": "Mozilla/5.0"}), timeout=10).read().decode()
def now_ms():
    return time.time() * 1000.0

def discover():
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
            if tks and 120 <= tau <= 300:
                cands.append((tau, tks[0], m.get("question")))
    if not cands:
        return None
    cands.sort(reverse=True)  # freshest (largest tau) first
    return cands[0]

picked = None
for attempt in range(22):  # wait out the inter-window gap (a window starts every ~5min)
    picked = discover()
    if picked:
        break
    print("waiting for a fresh window (attempt %d)..." % (attempt + 1))
    time.sleep(8)
if not picked:
    print("no fresh BTC window after waiting — re-run later")
    raise SystemExit
TAU, TOK, Q = picked
RUN_MS = min(180000.0, (TAU - 25) * 1000.0)
print("picked tau=%ds run=%.0fs | %s" % (TAU, RUN_MS / 1000, Q))

start = now_ms()
events = []
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
                        with lock:
                            events.append((now_ms(), "btc", float(j["data"][0]["last"]), 0.0))
                except Exception:
                    pass
            ws.close()
        except Exception:
            time.sleep(1)

def pm():
    while now_ms() - start < RUN_MS:
        try:
            ws = websocket.create_connection("wss://ws-subscriptions-clob.polymarket.com/ws/market", sslopt={"cert_reqs": ssl.CERT_NONE})
            ws.send(json.dumps({"assets_ids": [TOK], "type": "market"}))
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
                            if pc.get("asset_id") == TOK:
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

t1 = threading.Thread(target=kraken); t2 = threading.Thread(target=pm)
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

if len(btc) >= 30 and len(pm_) >= 30:
    import math
    BIN = 500.0
    t0 = evs[0][0]; tN = evs[-1][0]; nb = int((tN - t0) / BIN) + 1
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
    rb = [(gb[i]/gb[i-1]-1) if (gb[i] and gb[i-1]) else 0.0 for i in range(1, nb)]
    rp = [(gp[i]/gp[i-1]-1) if (gp[i] and gp[i-1]) else 0.0 for i in range(1, nb)]
    def corr(x, y):
        n = len(x); mx = sum(x)/n; my = sum(y)/n
        sxy = sum((x[i]-mx)*(y[i]-my) for i in range(n))
        sxx = sum((v-mx)**2 for v in x); syy = sum((v-my)**2 for v in y)
        return sxy/math.sqrt(sxx*syy) if sxx > 0 and syy > 0 else 0.0
    print("=== BTC->PM lag cross-correlation (bin=500ms) ===")
    best = (0, 0.0)
    for lag in range(0, 13):
        x = rb[:len(rb)-lag] if lag > 0 else rb
        y = rp[lag:]; n = min(len(x), len(y)); c = corr(x[:n], y[:n])
        if abs(c) > abs(best[1]):
            best = (int(lag*BIN), c)
        print("  lag %4dms  %+.3f" % (int(lag*BIN), c))
    print("PEAK lag=%dms corr=%+.3f" % best)

    # EVENT-BASED: per BTC up-jump, PM ask response lag + mispricing magnitude vs spread
    pm_ask = [(e[0], e[2]) for e in evs if e[1] == "pm"]
    spreads = sorted(e[2]-e[3] for e in evs if e[1] == "pm" and e[2] > e[3])
    med_spread = spreads[len(spreads)//2] if spreads else 0.0
    jumps = []
    for i in range(len(btc)):
        t, px = btc[i]; j = i
        while j > 0 and t - btc[j][0] < 1000:
            j -= 1
        if j < i and btc[j][1] > 0 and abs(px/btc[j][1]-1) > 0.0002:
            jumps.append((t, 1 if px > btc[j][1] else -1))
    dj = []
    for t, d in jumps:
        if not dj or t - dj[-1][0] > 2000:
            dj.append((t, d))
    def ask_at(t):
        v = None
        for tt, a in pm_ask:
            if tt <= t:
                v = a
            else:
                break
        return v
    lags = []; mags = []
    for t, d in dj:
        if d <= 0:
            continue
        a0 = ask_at(t)
        if a0 is None:
            continue
        resp_t = None; a1 = a0
        for tt, a in pm_ask:
            if tt <= t:
                continue
            if tt - t > 3000:
                break
            if a > a0 + 0.001 and resp_t is None:
                resp_t = tt
            a1 = max(a1, a)
        if resp_t:
            lags.append(resp_t - t); mags.append(a1 - a0)
    print("=== EVENT-BASED (BTC up-jumps >0.02%/1s) ===")
    print("jumps=%d up-jumps-with-ask-response=%d median_spread=%.3f" % (len(dj), len(lags), med_spread))
    if lags:
        sl = sorted(lags); sm = sorted(mags)
        print("ask response lag ms median=%.0f" % sl[len(sl)//2])
        print("ask mispricing magnitude median=%.3f (vs spread %.3f)" % (sm[len(sm)//2], med_spread))
        print("-> profitable-snipeable if magnitude > spread AND lag >> our ~100ms latency")
else:
    print("too few samples")
