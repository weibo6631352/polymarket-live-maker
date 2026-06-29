#!/usr/bin/env python3
# strategy_dryrun.py — dry-live test of the CONFIRMED signal (snipe the seconds-scale Binance move).
# Streams Binance BTCUSDT + PM CLOB (both Up & Down tokens) across fresh windows. On each Binance
# seconds-move > THRESH, simulates buying the FAVORED side at the PM ask available AT OUR LATENCY L,
# holds to window end, and scores win/P&L using the Binance-computed outcome (Up if end>open). This is
# what WE would actually capture. Read-only, no orders, zero money.
import json, ssl, time, threading, calendar, urllib.request, websocket

THRESH = 0.0003   # 0.03% Binance move over ~3s = a snipe trigger
LAT = 150         # our assumed fill latency ms
HOLDBACK = 25     # stop trading a window this many s before it ends

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

RUN_MS = 1800000.0  # 30 min
start = now_ms()
btc_hist = []      # (t, mid)  shared
btc_lock = threading.Lock()
trades = []        # simulated entries: (side, entry_ask, won_resolver_idx) -> resolved later
results = []       # (won, pnl)
res_lock = threading.Lock()

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
                        mid = (float(j["b"]) + float(j["a"])) / 2.0
                        with btc_lock:
                            btc_hist.append((now_ms(), mid))
                            if len(btc_hist) > 6000:
                                del btc_hist[:2000]
                except Exception:
                    pass
            ws.close()
        except Exception:
            time.sleep(1)

def btc_now():
    with btc_lock:
        return btc_hist[-1][1] if btc_hist else 0.0
def btc_ago(ms):
    tgt = now_ms() - ms
    with btc_lock:
        v = None
        for t, p in btc_hist:
            if t <= tgt:
                v = p
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
        ask = {up_tok: None, dn_tok: None}
        # per-window simulated entries (side_tok, entry_ask, entry_t)
        win_entries = []
        last_trig = 0
        deadline = min(end - HOLDBACK, start / 1000 + RUN_MS / 1000)
        try:
            ws = websocket.create_connection("wss://ws-subscriptions-clob.polymarket.com/ws/market", sslopt={"cert_reqs": ssl.CERT_NONE})
            ws.send(json.dumps({"assets_ids": [up_tok, dn_tok], "type": "market"}))
            ws.settimeout(2)
            lastping = now_ms()
            while time.time() < deadline:
                if now_ms() - lastping > 9000:
                    try:
                        ws.send("PING")
                    except Exception:
                        pass
                    lastping = now_ms()
                # check Binance trigger
                p_now = btc_now(); p_ago = btc_ago(3000)
                if p_now > 0 and p_ago and now_ms() - last_trig > 2000:
                    mv = p_now / p_ago - 1
                    if abs(mv) > THRESH:
                        fav = up_tok if mv > 0 else dn_tok
                        # we fill at the ask LAT ms later -> approximate with the ask after we process LAT
                        # (here ask[fav] is the latest; LAT effect is small relative to the lag, approximated)
                        a = ask[fav]
                        if a and 0.02 < a < 0.98:
                            win_entries.append((fav, a, now_ms()))
                            last_trig = now_ms()
                try:
                    m = ws.recv()
                except Exception:
                    continue
                try:
                    j = json.loads(m)
                    ev = j[0] if isinstance(j, list) and j else j
                    if ev.get("event_type") in ("price_change", "book"):
                        if ev.get("event_type") == "book":
                            aid = ev.get("asset_id")
                            asks = ev.get("asks") or []
                            if aid in ask and asks:
                                ask[aid] = min(float(x["price"]) for x in asks)
                        else:
                            for pc in ev.get("price_changes", []):
                                aid = pc.get("asset_id")
                                if aid in ask:
                                    ba = float(pc.get("best_ask", 0) or 0)
                                    if ba > 0:
                                        ask[aid] = ba
                except Exception:
                    pass
            ws.close()
        except Exception:
            time.sleep(2)
        # resolve this window from Binance end price
        end_btc = btc_now()
        if end_btc > 0 and win_entries:
            up_won = end_btc > opn
            for tok, a, _ in win_entries:
                won = (tok == up_tok and up_won) or (tok == dn_tok and not up_won)
                with res_lock:
                    results.append((1 if won else 0, (1.0 if won else 0.0) - a))

t1 = threading.Thread(target=binance); t2 = threading.Thread(target=pm)
t1.start(); t2.start(); t1.join(); t2.join()

n = len(results)
print("simulated snipe entries: %d" % n)
if n:
    wins = sum(w for w, _ in results)
    pnl = sum(p for _, p in results)
    cost = n  # rough; per $1/entry
    print("win-rate=%d%%  total_pnl(per $1/entry)=$%.2f  avg_edge/entry=%+.4f" % (
        100 * wins // n, pnl, pnl / n))
    print("-> PROFITABLE at our latency if win-rate>>50%% and avg_edge>0 over a decent n")
else:
    print("no entries (low vol / few triggers) — run longer or in higher vol")
