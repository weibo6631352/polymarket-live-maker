#!/usr/bin/env python3
# snipe_window_probe.py — robust big-sample test of the stale-quote edge. Streams Kraken BTC + rolls
# through consecutive fresh BTC up/down windows for ~20min, accumulating BTC jumps and the PM ask's
# response (lag + magnitude vs spread) across ALL windows. If, over many jumps, the ask is stale-cheap
# by > the spread for >> our latency, sniping is real; if not, the BTC-lag hypothesis is dead.
# Read-only, no orders, zero money.
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
                cands.append((tau, tks[0]))
    if not cands:
        return None
    cands.sort(reverse=True)
    return cands[0]

RUN_MS = 1200000.0  # 20 min
start = now_ms()
events = []   # btc: (t,'btc',px,0,0) ; pm: (t,'pm',seg,ask,bid)
lock = threading.Lock()

def kraken():  # uses BINANCE (the PM resolution source) bookTicker mid, not Kraken
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
                        with lock:
                            events.append((now_ms(), "btc", mid, 0.0, 0.0))
                except Exception:
                    pass
            ws.close()
        except Exception:
            time.sleep(1)

def pm():
    seg = 0
    while now_ms() - start < RUN_MS:
        picked = discover()
        if not picked:
            time.sleep(6)
            continue
        tau, tok = picked
        seg += 1
        deadline = min(now_ms() + (tau - 40) * 1000.0, start + RUN_MS)
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
                                a = float(pc.get("best_ask", 0) or 0)
                                b = float(pc.get("best_bid", 0) or 0)
                                if a > 0 and b > 0:
                                    with lock:
                                        events.append((now_ms(), "pm", float(seg), a, b))
                except Exception:
                    pass
            ws.close()
        except Exception:
            time.sleep(2)

t1 = threading.Thread(target=kraken); t2 = threading.Thread(target=pm)
t1.start(); t2.start(); t1.join(); t2.join()

evs = sorted(events, key=lambda e: e[0])
btc = [(e[0], e[2]) for e in evs if e[1] == "btc"]
pm_by_seg = {}
for e in evs:
    if e[1] == "pm":
        pm_by_seg.setdefault(int(e[2]), []).append((e[0], e[3], e[4]))  # (t, ask, bid)
nseg = len(pm_by_seg)
npm = sum(len(v) for v in pm_by_seg.values())
print("collected btc=%d pm=%d over %d windows, %.0fs" % (len(btc), npm, nseg, (now_ms() - start) / 1000))
if btc:
    bv = [v for _, v in btc]
    print("BTC range %.1f-%.1f (move %.3f%%)" % (min(bv), max(bv), 100 * (max(bv) / min(bv) - 1)))

# which seg is active at time t (the seg whose PM events bracket t)
seg_spans = []
for s, lst in pm_by_seg.items():
    if lst:
        seg_spans.append((lst[0][0], lst[-1][0], s))
def seg_at(t):
    for a, b, s in seg_spans:
        if a <= t <= b:
            return s
    return None

# BTC jumps (1s return > 0.02%) — O(n) two-pointer (btc is dense from Binance)
jumps = []
j = 0
for i in range(len(btc)):
    t, px = btc[i]
    while j < i and btc[j][0] < t - 1000:
        j += 1
    if btc[j][1] > 0 and t - btc[j][0] >= 800 and abs(px / btc[j][1] - 1) > 0.0002:
        jumps.append((t, 1 if px > btc[j][1] else -1, px / btc[j][1] - 1))
dj = []
for t, d, r in jumps:
    if not dj or t - dj[-1][0] > 2000:
        dj.append((t, d, r))

lags = []; mags = []; spreads_at = []; net = []
for t, d, r in dj:
    s = seg_at(t)
    if s is None:
        continue
    lst = pm_by_seg[s]
    a0 = b0 = None
    for tt, a, b in lst:
        if tt <= t:
            a0 = a; b0 = b
        else:
            break
    if a0 is None:
        continue
    spread = a0 - b0
    if d > 0:  # BTC up -> Up ask should rise; snipe = buy at stale low ask a0
        resp_t = None; a1 = a0
        for tt, a, b in lst:
            if tt <= t:
                continue
            if tt - t > 3000:
                break
            if a > a0 + 0.001 and resp_t is None:
                resp_t = tt
            a1 = max(a1, a)
        if resp_t:
            lags.append(resp_t - t); mags.append(a1 - a0); spreads_at.append(spread)
            net.append((a1 - a0) - spread)

print("=== EVENT-BASED over all windows ===")
print("BTC jumps=%d  up-jumps-with-ask-response=%d" % (len(dj), len(lags)))
if lags:
    sl = sorted(lags); sm = sorted(mags); ss = sorted(spreads_at); sn = sorted(net)
    md = lambda a: a[len(a)//2]
    print("ask response lag ms:   median=%.0f  (need >> ~100ms our latency)" % md(sl))
    print("ask mispricing mag:    median=%.3f" % md(sm))
    print("spread at jump:        median=%.3f" % md(ss))
    print("NET (mag - spread):    median=%.3f  mean=%.3f  >0frac=%.0f%%" % (md(sn), sum(net)/len(net), 100*sum(1 for x in net if x > 0)/len(net)))
    print("-> profitable if NET>0 consistently AND lag >> our latency")

    # CRITICAL: at OUR latency L we fill at the ask a_at(t+L) (already partly risen as faster snipers
    # take it); captured net = (a1 - a_fill) - spread. This is what WE actually get, not the total edge.
    def ask_at_seg(lst, t):
        v = None
        for tt, a, b in lst:
            if tt <= t:
                v = a
            else:
                break
        return v
    print("=== CAPTURE vs OUR latency (the real go/no-go) ===")
    for L in [50, 100, 150, 200, 300, 500]:
        nets = []
        for t, d, r in dj:
            if d <= 0:
                continue
            s = seg_at(t)
            if s is None:
                continue
            lst = pm_by_seg[s]
            a0 = b0 = None
            for tt, a, b in lst:
                if tt <= t:
                    a0 = a; b0 = b
                else:
                    break
            if a0 is None:
                continue
            a1 = a0
            for tt, a, b in lst:
                if t < tt <= t + 3000:
                    a1 = max(a1, a)
            if a1 <= a0 + 0.001:
                continue
            a_fill = ask_at_seg(lst, t + L)
            if a_fill is None:
                continue
            spread = (a0 - b0) if b0 else 0.01
            nets.append((a1 - a_fill) - spread)
        if nets:
            sn = sorted(nets)
            print("  L=%4dms: n=%d median_net=%+.4f mean=%+.4f >0frac=%.0f%%" % (
                L, len(nets), sn[len(sn)//2], sum(nets)/len(nets), 100*sum(1 for x in nets if x > 0)/len(nets)))
else:
    print("no up-jumps with a detectable ask response -> no BTC-lag snipe signal in this sample")
