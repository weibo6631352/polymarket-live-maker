#!/usr/bin/env python3
# okx_channel_test.py — measure OKX channel update density: tickers (throttled ~100ms) vs bbo-tbt (tick-by-tick
# best bid/offer) vs trades (every trade), with Binance bookTicker as the reference. Answers: how much faster
# is bbo-tbt than the 6/s tickers we've been using? Read-only, zero money.
import json, ssl, time, threading, websocket

RUN = 60
start = time.time()
counts = {}
errors = []
lock = threading.Lock()
def bump(k):
    with lock:
        counts[k] = counts.get(k, 0) + 1

def okx():
    try:
        ws = websocket.create_connection("wss://ws.okx.com:8443/ws/v5/public", sslopt={"cert_reqs": ssl.CERT_NONE})
        for ch in ["tickers", "bbo-tbt", "trades"]:
            ws.send(json.dumps({"op": "subscribe", "args": [{"channel": ch, "instId": "BTC-USDT"}]}))
        ws.settimeout(5)
        while time.time() - start < RUN:
            try:
                m = ws.recv()
            except Exception:
                continue
            try:
                j = json.loads(m)
                if j.get("event") == "error":
                    with lock:
                        errors.append(j.get("msg", "") + " / " + str(j.get("arg", "")))
                elif j.get("arg") and j.get("data"):
                    bump("okx:" + j["arg"]["channel"])
            except Exception:
                pass
        ws.close()
    except Exception as e:
        with lock:
            errors.append("okx conn: " + str(e))

def binance():
    try:
        ws = websocket.create_connection("wss://stream.binance.com:9443/ws/btcusdt@bookTicker", sslopt={"cert_reqs": ssl.CERT_NONE})
        ws.settimeout(5)
        while time.time() - start < RUN:
            try:
                m = ws.recv()
            except Exception:
                continue
            bump("binance:bookTicker")
        ws.close()
    except Exception as e:
        with lock:
            errors.append("binance conn: " + str(e))

ts = [threading.Thread(target=okx), threading.Thread(target=binance)]
for t in ts:
    t.start()
for t in ts:
    t.join()

print("=== updates in %ds ===" % RUN)
for k in sorted(counts):
    print("  %-22s %6d  (%.1f/s)" % (k, counts[k], counts[k] / RUN))
if errors:
    print("=== errors/notes ===")
    for e in set(errors):
        print("  " + e)
