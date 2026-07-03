#!/usr/bin/env python3
# feed_race.py — which BTC feed is FASTEST to our box? Connects Binance (the PM resolution source, engine
# in Tokyo ~240ms away), OKX, and Kraken (European, ~10-21ms) simultaneously, records (t, feed, mid), then
# cross-correlates returns to find which feed LEADS and by how many ms. The fastest feed that still tracks
# Binance is what we should snipe on (for the lowest signal latency). Read-only, zero money.
import json, ssl, time, threading, math, websocket

RUN = 180
start = time.time()
events = []
lock = threading.Lock()
def rec(feed, mid):
    with lock:
        events.append((time.time() * 1000.0, feed, mid))

def run_feed(name, url, sub, parse):
    while time.time() - start < RUN:
        try:
            ws = websocket.create_connection(url, sslopt={"cert_reqs": ssl.CERT_NONE})
            if sub:
                ws.send(sub)
            ws.settimeout(5)
            while time.time() - start < RUN:
                try:
                    m = ws.recv()
                except Exception:
                    continue
                try:
                    mid = parse(json.loads(m))
                    if mid:
                        rec(name, mid)
                except Exception:
                    pass
            ws.close()
        except Exception:
            time.sleep(1)

def p_binance(j):
    if "b" in j and "a" in j:
        return (float(j["b"]) + float(j["a"])) / 2.0
def p_okx(j):
    if j.get("data"):
        d = j["data"][0]
        return (float(d["bidPx"]) + float(d["askPx"])) / 2.0
def p_kraken(j):
    if j.get("channel") == "ticker" and j.get("data"):
        d = j["data"][0]
        return (float(d["bid"]) + float(d["ask"])) / 2.0

feeds = [
    ("binance", "wss://stream.binance.com:9443/ws/btcusdt@bookTicker", None, p_binance),
    ("okx", "wss://ws.okx.com:8443/ws/v5/public",
     json.dumps({"op": "subscribe", "args": [{"channel": "tickers", "instId": "BTC-USDT"}]}), p_okx),
    ("kraken", "wss://ws.kraken.com/v2",
     json.dumps({"method": "subscribe", "params": {"channel": "ticker", "symbol": ["BTC/USD"]}}), p_kraken),
]
ts = [threading.Thread(target=run_feed, args=f) for f in feeds]
for t in ts:
    t.start()
for t in ts:
    t.join()

evs = sorted(events, key=lambda e: e[0])
names = ["binance", "okx", "kraken"]
series = {n: [(t, m) for t, f, m in evs if f == n] for n in names}
for n in names:
    print("%s: %d updates (%.1f/s)" % (n, len(series[n]), len(series[n]) / RUN))

# grid to 50ms, fill-forward; cross-correlate returns to find lead/lag vs binance
if all(len(series[n]) > 50 for n in names):
    BIN = 50.0
    t0 = evs[0][0]; tN = evs[-1][0]; nb = int((tN - t0) / BIN) + 1
    def grid(s):
        g = [None] * nb
        for t, m in s:
            g[min(int((t - t0) / BIN), nb - 1)] = m
        last = None
        for i in range(nb):
            if g[i] is None:
                g[i] = last
            else:
                last = g[i]
        return g
    G = {n: grid(series[n]) for n in names}
    R = {n: [(G[n][i] / G[n][i-1] - 1) if (G[n][i] and G[n][i-1]) else 0.0 for i in range(1, nb)] for n in names}
    def corr(x, y):
        k = len(x); mx = sum(x)/k; my = sum(y)/k
        sxy = sum((x[i]-mx)*(y[i]-my) for i in range(k))
        sxx = sum((v-mx)**2 for v in x); syy = sum((v-my)**2 for v in y)
        return sxy/math.sqrt(sxx*syy) if sxx > 0 and syy > 0 else 0.0
    print("\n=== cross-correlation vs BINANCE (peak lag<0 => that feed LEADS binance) ===")
    rb = R["binance"]
    for n in ["okx", "kraken"]:
        rn = R[n]
        best = (0, 0.0)
        for lag in range(-20, 21):  # +-1000ms in 50ms steps
            if lag >= 0:
                x = rb[lag:]; y = rn[:len(rn)-lag] if lag > 0 else rn
            else:
                x = rb[:len(rb)+lag]; y = rn[-lag:]
            k = min(len(x), len(y))
            c = corr(x[:k], y[:k])
            if abs(c) > abs(best[1]):
                best = (lag * 50, c)
        lead = "%s LEADS binance by %dms" % (n, -best[0]) if best[0] < 0 else \
               ("binance leads %s by %dms" % (n, best[0]) if best[0] > 0 else "synchronous")
        print("  %s: peak corr %+.3f at lag %dms -> %s" % (n, best[1], best[0], lead))
