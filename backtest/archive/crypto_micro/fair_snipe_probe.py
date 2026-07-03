#!/usr/bin/env python3
# fair_snipe_probe.py — the RIGHT snipe-signal measurement. Streams Binance BTCUSDT (PM resolution source)
# + PM CLOB quote per fresh window. For each window it fetches the true window-OPEN from Binance klines,
# continuously computes the BTC-implied fair P(Up) = Phi(ln(btc/open)/(sigma*sqrt(tau/300))), and compares
# to the PM best_ask. A snipe opportunity = ask < fair - MARGIN (buy Up cheap). Measures opportunity
# frequency, gap magnitude, persistence, and the capturable gap at OUR latency. Read-only, zero money.
import json, ssl, time, threading, calendar, math, urllib.request, websocket

SIGMA = 0.0016
MARGIN = 0.02

def hg(u):
    return urllib.request.urlopen(urllib.request.Request(u, headers={"User-Agent": "Mozilla/5.0"}), timeout=10).read().decode()
def now_ms():
    return time.time() * 1000.0
def ncdf(z):
    return 0.5 * math.erfc(-z / math.sqrt(2))

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
                cands.append((tau, tks[0], end))
    if not cands:
        return None
    cands.sort(reverse=True)
    return cands[0]

def binance_open(end_unix):
    start_ms = int((end_unix - 300) * 1000)
    try:
        kl = json.loads(hg("https://api.binance.com/api/v3/klines?symbol=BTCUSDT&interval=1m&startTime=%d&limit=1" % start_ms))
        return float(kl[0][1])  # open of the 1m candle at the window start
    except Exception:
        return None

RUN_MS = 1800000.0  # 30 min
start = now_ms()
g_btc = [0.0]
rows = []   # (t, gap, ask, fair, tau)  gap = fair - ask (only when >0 i.e. a snipe opp)
lock = threading.Lock()

def binance():
    while now_ms() - start < RUN_MS:
        try:
            ws = websocket.create_connection("wss://stream.binance.com:9443/ws/btcusdt@bookTicker", sslopt={"cert_reqs": ssl.CERT_NONE})
            ws.settimeout(5)
            while now_ms() - start < RUN_MS:
                try:
                    m = ws.recv()
                except Exception:
                    continue
                try:
                    j = json.loads(m)
                    if "b" in j and "a" in j:
                        g_btc[0] = (float(j["b"]) + float(j["a"])) / 2.0
                except Exception:
                    pass
            ws.close()
        except Exception:
            time.sleep(1)

opps = []   # each opportunity sample: (t, gap, ask, tau)
def pm():
    while now_ms() - start < RUN_MS:
        picked = discover()
        if not picked:
            time.sleep(6); continue
        tau0, tok, end = picked
        opn = binance_open(end)
        if not opn or g_btc[0] <= 0:
            time.sleep(2); continue
        deadline = min(now_ms() + (tau0 - 35) * 1000.0, start + RUN_MS)
        try:
            ws = websocket.create_connection("wss://ws-subscriptions-clob.polymarket.com/ws/market", sslopt={"cert_reqs": ssl.CERT_NONE})
            ws.send(json.dumps({"assets_ids": [tok], "type": "market"}))
            ws.settimeout(4)
            lastping = now_ms()
            while now_ms() < deadline:
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
                                ask = float(pc.get("best_ask", 0) or 0)
                                bid = float(pc.get("best_bid", 0) or 0)
                                btc = g_btc[0]
                                tau = (end - time.time())
                                if ask <= 0 or bid <= 0 or btc <= 0 or tau < 20:
                                    continue
                                z = math.log(btc / opn) / (SIGMA * math.sqrt(max(tau, 1) / 300.0))
                                fair = ncdf(z)
                                gap = fair - ask  # >0 => ask is stale-cheap to buy Up
                                with lock:
                                    opps.append((now_ms(), gap, ask, fair, tau))
                except Exception:
                    pass
            ws.close()
        except Exception:
            time.sleep(2)

t1 = threading.Thread(target=binance); t2 = threading.Thread(target=pm)
t1.start(); t2.start(); t1.join(); t2.join()

print("collected quote-samples=%d over %.0fs" % (len(opps), (now_ms() - start) / 1000))
snipe = [o for o in opps if o[1] > MARGIN]   # ask below fair by > margin
print("snipe opportunities (fair - ask > %.2f): %d  (%.1f%% of quotes)" % (MARGIN, len(snipe), 100.0 * len(snipe) / max(len(opps), 1)))
if snipe:
    gaps = sorted(o[1] for o in snipe)
    asks = sorted(o[2] for o in snipe)
    md = lambda a: a[len(a) // 2]
    print("median gap (fair-ask) = %.3f   median ask = %.3f" % (md(gaps), md(asks)))
    print("mean gap = %.3f   max gap = %.3f" % (sum(o[1] for o in snipe) / len(snipe), max(gaps)))
    print("-> a real, frequent snipe edge if gap > spread/fees consistently AND opps persist > our latency")
else:
    print("NO snipe opportunities (ask never stale-cheap vs fair) -> PM tracks Binance fair tightly; no edge here")
