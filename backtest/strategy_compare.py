#!/usr/bin/env python3
# strategy_compare.py — APPLES-TO-APPLES OKX vs Binance signal, SAME windows (controls for vol/period).
# Streams OKX + Binance + PM book over the same fresh windows. Fires a trigger when EITHER feed makes a
# >THRESH/3s move (separate cooldowns), tags it 'okx' or 'bin', fills at the PM ask at our latency, resolves
# via Binance end>open. Then compares the two signals' win-rate + edge/entry at each latency, on the same data.
# Big sample (3h) so the comparison is statistically meaningful, not small-sample noise. Read-only, zero money.
import json, ssl, time, threading, calendar, urllib.request, websocket

THRESH = 0.0003
LATS = [0, 25, 50, 100, 200, 400]
HOLDBACK = 20
RUN_MS = 10800000.0  # 3 hr

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

start = now_ms()
bhist = []; ohist = []
blk = threading.Lock(); olk = threading.Lock()
results = {"okx": {L: [] for L in LATS}, "bin": {L: [] for L in LATS}}
res_lock = threading.Lock()

def feed(url, sub, parse, hist, lk):
    while now_ms() - start < RUN_MS:
        try:
            ws = websocket.create_connection(url, sslopt={"cert_reqs": ssl.CERT_NONE})
            if sub:
                ws.send(sub)
            ws.settimeout(5)
            while now_ms() - start < RUN_MS:
                try:
                    m = ws.recv()
                except Exception:
                    continue
                try:
                    mid = parse(json.loads(m))
                    if mid:
                        with lk:
                            hist.append((now_ms(), mid))
                            if len(hist) > 6000:
                                del hist[:2000]
                except Exception:
                    pass
            ws.close()
        except Exception:
            time.sleep(1)

def p_bin(j):
    if "b" in j and "a" in j:
        return (float(j["b"]) + float(j["a"])) / 2.0
def p_okx(j):
    if j.get("data"):
        d = j["data"][0]
        return (float(d["bidPx"]) + float(d["askPx"])) / 2.0

def latest(hist, lk):
    with lk:
        return hist[-1][1] if hist else 0.0
def ago(hist, lk, ms):
    tgt = now_ms() - ms
    with lk:
        v = None
        for t, p in hist:
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
        triggers = []  # (t, fav, source)
        lt = {"okx": 0, "bin": 0}
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
                for src, hist, lk in (("okx", ohist, olk), ("bin", bhist, blk)):
                    pn = latest(hist, lk); pa = ago(hist, lk, 3000)
                    if pn > 0 and pa and now_ms() - lt[src] > 2000:
                        mv = pn / pa - 1
                        if abs(mv) > THRESH:
                            triggers.append((now_ms(), up_tok if mv > 0 else dn_tok, src))
                            lt[src] = now_ms()
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
        end_btc = latest(bhist, blk)
        if end_btc <= 0:
            continue
        up_won = end_btc > opn
        for trig_t, fav, src in triggers:
            won = (fav == up_tok and up_won) or (fav == dn_tok and not up_won)
            for Lv in LATS:
                fill = ask_at(ah[fav], trig_t + Lv)
                if fill and 0.03 < fill < 0.97:
                    with res_lock:
                        results[src][Lv].append((1 if won else 0, (1.0 if won else 0.0) - fill))

ts = [threading.Thread(target=feed, args=("wss://stream.binance.com:9443/ws/btcusdt@bookTicker", None, p_bin, bhist, blk)),
      threading.Thread(target=feed, args=("wss://ws.okx.com:8443/ws/v5/public",
          json.dumps({"op": "subscribe", "args": [{"channel": "tickers", "instId": "BTC-USDT"}]}), p_okx, ohist, olk)),
      threading.Thread(target=pm)]
for t in ts:
    t.start()
for t in ts:
    t.join()

print("SAME-WINDOW OKX vs Binance signal comparison (resolution via Binance):")
for src in ("okx", "bin"):
    r0 = results[src][LATS[0]]
    print("\n=== %s signal ===" % src.upper())
    if not r0:
        print("  no entries"); continue
    print("  LAT_ms   n   win%%   avg_edge/entry   total")
    for Lv in LATS:
        r = results[src][Lv]
        if not r:
            continue
        wins = sum(w for w, _ in r); pnl = sum(p for _, p in r)
        print("    %4d  %3d   %3d%%    %+.4f       $%.2f" % (Lv, len(r), 100 * wins // len(r), pnl / len(r), pnl))
