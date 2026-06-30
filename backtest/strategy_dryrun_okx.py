#!/usr/bin/env python3
# strategy_dryrun_okx.py — does the edge HOLD with the FASTER OKX signal? OKX leads Binance ~200ms.
# Signal = OKX seconds-move (faster); resolution = Binance end>open (faithful, PM resolves on Binance);
# fill = PM ask at our latency. If win-rate/edge stays + (ideally better), switch the live sniper to OKX.
import json, ssl, time, threading, calendar, urllib.request, websocket

THRESH = 0.0003
LATS = [0, 10, 25, 50, 100, 200]   # probe the EXTREME-low-latency regime our speed build targets
HOLDBACK = 20

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
        return None

RUN_MS = 3600000.0  # 60 min (bigger OKX sample + sub-50ms latency curve)
start = now_ms()
bhist = []; ohist = []
blk = threading.Lock(); olk = threading.Lock()
results = {L: [] for L in LATS}
res_lock = threading.Lock()

def binance_feed():  # for window open + resolution
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
                        with blk:
                            bhist.append((now_ms(), (float(j["b"]) + float(j["a"])) / 2.0))
                            if len(bhist) > 6000:
                                del bhist[:2000]
                except Exception:
                    pass
            ws.close()
        except Exception:
            time.sleep(1)

def okx_feed():  # the FAST signal feed
    while now_ms() - start < RUN_MS:
        try:
            ws = websocket.create_connection("wss://ws.okx.com:8443/ws/v5/public", sslopt={"cert_reqs": ssl.CERT_NONE})
            ws.send(json.dumps({"op": "subscribe", "args": [{"channel": "tickers", "instId": "BTC-USDT"}]}))
            ws.settimeout(5)
            while now_ms() - start < RUN_MS:
                try:
                    m = ws.recv()
                except Exception:
                    continue
                try:
                    j = json.loads(m)
                    if j.get("data"):
                        d = j["data"][0]
                        with olk:
                            ohist.append((now_ms(), (float(d["bidPx"]) + float(d["askPx"])) / 2.0))
                            if len(ohist) > 6000:
                                del ohist[:2000]
                except Exception:
                    pass
            ws.close()
        except Exception:
            time.sleep(1)

def b_now():
    with blk:
        return bhist[-1][1] if bhist else 0.0
def o_now():
    with olk:
        return ohist[-1][1] if ohist else 0.0
def o_ago(ms):
    tgt = now_ms() - ms
    with olk:
        v = None
        for t, p in ohist:
            if t <= tgt:
                v = p
            else:
                break
        return v
def ask_at(hist, t):
    v = None
    for tt, a in hist:
        if tt <= t:
            v = a
        else:
            break
    return v

def pm():
    while now_ms() - start < RUN_MS:
        picked = discover()
        if not picked:
            time.sleep(6); continue
        tau0, tks, end = picked
        up_tok, dn_tok = tks[0], tks[1]
        opn = binance_open(end)
        if not opn:
            time.sleep(2); continue
        ah = {up_tok: [], dn_tok: []}
        triggers = []
        last_trig = 0
        try:
            ws = websocket.create_connection("wss://ws-subscriptions-clob.polymarket.com/ws/market", sslopt={"cert_reqs": ssl.CERT_NONE})
            ws.send(json.dumps({"assets_ids": [up_tok, dn_tok], "type": "market"}))
            ws.settimeout(2)
            lastping = now_ms()
            while time.time() < end - HOLDBACK and now_ms() - start < RUN_MS:
                if now_ms() - lastping > 9000:
                    try:
                        ws.send("PING")
                    except Exception:
                        pass
                    lastping = now_ms()
                pn = o_now(); pa = o_ago(3000)   # OKX signal
                if pn > 0 and pa and now_ms() - last_trig > 2000:
                    mv = pn / pa - 1
                    if abs(mv) > THRESH:
                        triggers.append((now_ms(), up_tok if mv > 0 else dn_tok))
                        last_trig = now_ms()
                try:
                    m = ws.recv()
                except Exception:
                    continue
                try:
                    j = json.loads(m)
                    ev = j[0] if isinstance(j, list) and j else j
                    et = ev.get("event_type")
                    if et == "book":
                        aid = ev.get("asset_id"); asks = ev.get("asks") or []
                        if aid in ah and asks:
                            ah[aid].append((now_ms(), min(float(x["price"]) for x in asks)))
                    elif et == "price_change":
                        for pc in ev.get("price_changes", []):
                            aid = pc.get("asset_id")
                            if aid in ah:
                                ba = float(pc.get("best_ask", 0) or 0)
                                if ba > 0:
                                    ah[aid].append((now_ms(), ba))
                except Exception:
                    pass
            ws.close()
        except Exception:
            pass
        while time.time() < end + 1 and now_ms() - start < RUN_MS + 60000:
            time.sleep(0.5)
        end_btc = b_now()   # Binance resolution
        if end_btc <= 0:
            continue
        up_won = end_btc > opn
        for trig_t, fav in triggers:
            won = (fav == up_tok and up_won) or (fav == dn_tok and not up_won)
            for Lv in LATS:
                fill = ask_at(ah[fav], trig_t + Lv)
                if fill and 0.03 < fill < 0.97:
                    with res_lock:
                        results[Lv].append((1 if won else 0, (1.0 if won else 0.0) - fill))

ts = [threading.Thread(target=binance_feed), threading.Thread(target=okx_feed), threading.Thread(target=pm)]
for t in ts:
    t.start()
for t in ts:
    t.join()

print("OKX-signal dry-run (resolution via Binance):")
if results[LATS[0]]:
    print("LAT_ms   n   win%%   avg_edge/entry   total")
    for Lv in LATS:
        r = results[Lv]
        if not r:
            continue
        wins = sum(w for w, _ in r); pnl = sum(p for _, p in r)
        print("  %4d  %3d   %3d%%    %+.4f       $%.2f" % (Lv, len(r), 100 * wins // len(r), pnl / len(r), pnl))
    print("-> edge HOLDS with the faster OKX feed if win-rate/edge stay positive")
else:
    print("no entries — run longer")
