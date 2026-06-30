#!/usr/bin/env python3
# raw_recorder.py — record EVERY raw tick to a CSV, nothing thrown away. For offline analysis + seeing the
# actual waveforms. Streams OKX BTC (bbo-tbt bid/ask), Binance BTC (bookTicker bid/ask), and PM CLOB book
# (best bid+ask for the active window's Up & Down tokens), plus window metadata (tokens, open, end).
# CSV cols: t_ms,kind,a,b,c,d   kind in {okx,bin,pm,win}:
#   okx/bin : a=bid b=ask           (BTC)
#   pm      : a=side(up/dn) b=bid c=ask
#   win     : a=up_tok b=dn_tok c=open d=end_unix
import json, ssl, time, threading, websocket, calendar, urllib.request

OUT = "/tmp/raw_ticks.csv"
RUN = float(__import__("os").environ.get("REC_SECS", "3600"))  # default 1hr
start = time.time()
# APPEND so a restart keeps accumulating into one growing CSV (we need 100+ windows for calibration)
f = open(OUT, "a", buffering=1 << 20)
flock = threading.Lock()
rows = [0]
def w(row):
    with flock:
        f.write(row + "\n")
        rows[0] += 1
def now_ms():
    return time.time() * 1000.0
def hg(u):
    return urllib.request.urlopen(urllib.request.Request(u, headers={"User-Agent": "Mozilla/5.0"}), timeout=10).read().decode()

def okx():
    while time.time() - start < RUN:
        try:
            ws = websocket.create_connection("wss://ws.okx.com:8443/ws/v5/public", sslopt={"cert_reqs": ssl.CERT_NONE})
            ws.send(json.dumps({"op": "subscribe", "args": [{"channel": "bbo-tbt", "instId": "BTC-USDT"}]}))
            ws.settimeout(5)
            while time.time() - start < RUN:
                try:
                    m = ws.recv()
                except Exception:
                    continue
                try:
                    j = json.loads(m)
                    if j.get("data"):
                        d = j["data"][0]
                        if d.get("bids") and d.get("asks"):
                            w("%.0f,okx,%s,%s" % (now_ms(), d["bids"][0][0], d["asks"][0][0]))
                except Exception:
                    pass
            ws.close()
        except Exception:
            time.sleep(1)

def binance():
    while time.time() - start < RUN:
        try:
            ws = websocket.create_connection("wss://stream.binance.com:9443/ws/btcusdt@bookTicker", sslopt={"cert_reqs": ssl.CERT_NONE})
            ws.settimeout(5)
            while time.time() - start < RUN:
                try:
                    m = ws.recv()
                except Exception:
                    continue
                try:
                    j = json.loads(m)
                    if "b" in j and "a" in j:
                        w("%.0f,bin,%s,%s" % (now_ms(), j["b"], j["a"]))
                except Exception:
                    pass
            ws.close()
        except Exception:
            time.sleep(1)

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
            if tks and len(tks) >= 2 and 120 <= tau <= 300:
                cands.append((tau, tks, end))
    if not cands:
        return None
    cands.sort(reverse=True)
    return cands[0]

def binance_open(end_unix):
    try:
        kl = json.loads(hg("https://api.binance.com/api/v3/klines?symbol=BTCUSDT&interval=1m&startTime=%d&limit=1" % int((end_unix - 300) * 1000)))
        return float(kl[0][1])
    except Exception:
        return 0.0

def pm():
    while time.time() - start < RUN:
        picked = discover()
        if not picked:
            time.sleep(6); continue
        tau, tks, end = picked
        up, dn = tks[0], tks[1]
        opn = binance_open(end)
        w("%.0f,win,%s,%s,%.1f,%d" % (now_ms(), up, dn, opn, end))
        try:
            ws = websocket.create_connection("wss://ws-subscriptions-clob.polymarket.com/ws/market", sslopt={"cert_reqs": ssl.CERT_NONE})
            ws.send(json.dumps({"assets_ids": [up, dn], "type": "market"}))
            ws.settimeout(2)
            lp = now_ms()
            # record PAST the end so we capture the actual resolution (winner PM price -> ~1, loser -> ~0)
            while time.time() < end + 75 and time.time() - start < RUN:
                if now_ms() - lp > 9000:
                    try:
                        ws.send("PING")
                    except Exception:
                        pass
                    lp = now_ms()
                try:
                    m = ws.recv()
                except Exception:
                    continue
                try:
                    j = json.loads(m)
                    ev = j[0] if isinstance(j, list) and j else j
                    et = ev.get("event_type")
                    if et == "price_change":
                        for pc in ev.get("price_changes", []):
                            aid = pc.get("asset_id")
                            side = "up" if aid == up else ("dn" if aid == dn else None)
                            if side:
                                w("%.0f,pm,%s,%s,%s" % (now_ms(), side, pc.get("best_bid", ""), pc.get("best_ask", "")))
                    elif et == "book":
                        aid = ev.get("asset_id")
                        side = "up" if aid == up else ("dn" if aid == dn else None)
                        if side:
                            bids = ev.get("bids") or []; asks = ev.get("asks") or []
                            bb = max((float(x["price"]) for x in bids), default=0.0)
                            ba = min((float(x["price"]) for x in asks), default=0.0)
                            w("%.0f,pm,%s,%.4f,%.4f" % (now_ms(), side, bb, ba))
                except Exception:
                    pass
            ws.close()
        except Exception:
            pass

def hb():
    while time.time() - start < RUN:
        time.sleep(60)
        print("[rec %.0fmin] rows=%d (%.1f MB)" % ((time.time() - start) / 60, rows[0], f.tell() / 1e6), flush=True)

ts = [threading.Thread(target=okx), threading.Thread(target=binance), threading.Thread(target=pm), threading.Thread(target=hb, daemon=True)]
for t in ts:
    t.start()
for t in ts[:3]:
    t.join()
f.close()
print("DONE recorded %d rows to %s" % (rows[0], OUT), flush=True)
