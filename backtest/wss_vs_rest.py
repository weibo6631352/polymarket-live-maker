#!/usr/bin/env python3
# wss_vs_rest.py — EMPIRICALLY verify: is PM CLOB WSS actually faster + fresher than maxed REST (~140/s)?
# Picks an active BTC up/down token; runs the WSS feed (records every best_ask CHANGE) and a REST /book
# poller maxed at ~140/s IN PARALLEL. Then for each WSS ask-change, finds when the REST poller first
# reported that same new value -> the lead (ms). Answers the question with data, not assumption.
import json, ssl, time, threading, calendar, urllib.request, websocket, statistics, bisect

def hg(u):
    return urllib.request.urlopen(urllib.request.Request(u, headers={"User-Agent": "Mozilla/5.0"}), timeout=10).read().decode()
def now_ms():
    return time.time() * 1000.0

def discover():
    iso = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    nowu = time.time()
    best = None
    for m in json.loads(hg("https://gamma-api.polymarket.com/markets?closed=false&limit=200&order=endDate&ascending=true&end_date_min=" + iso)):
        ql = (m.get("question") or "").lower()
        if "up or down" in ql and "bitcoin" in ql and m.get("acceptingOrders"):
            try:
                end = calendar.timegm(time.strptime(m["endDate"], "%Y-%m-%dT%H:%M:%SZ"))
            except Exception:
                continue
            tau = end - nowu
            tks = json.loads(m.get("clobTokenIds") or "[]")
            if tks and 150 <= tau <= 280:  # active, not about-to-resolve
                if not best or abs(tau - 210) < abs(best[0] - 210):
                    best = (tau, tks[0])
    return best[1] if best else None

TOKEN = discover()
print("token:", (TOKEN[:18] if TOKEN else None))
if not TOKEN:
    raise SystemExit("no active window")
RUN = 120.0
start = now_ms()
wss_evs = []
rest_evs = []
lock = threading.Lock()
rest_429 = [0]

def wss_thread():
    while now_ms() - start < RUN * 1000:
        try:
            ws = websocket.create_connection("wss://ws-subscriptions-clob.polymarket.com/ws/market", sslopt={"cert_reqs": ssl.CERT_NONE})
            ws.send(json.dumps({"assets_ids": [TOKEN], "type": "market"}))
            ws.settimeout(2)
            lp = now_ms()
            while now_ms() - start < RUN * 1000:
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
                    a = None
                    if et == "book":
                        asks = ev.get("asks") or []
                        if asks:
                            a = min(float(x["price"]) for x in asks)
                    elif et == "price_change":
                        for pc in ev.get("price_changes", []):
                            if pc.get("asset_id") == TOKEN:
                                ba = float(pc.get("best_ask", 0) or 0)
                                if ba > 0:
                                    a = ba
                    if a is not None:
                        with lock:
                            wss_evs.append((now_ms(), a))
                except Exception:
                    pass
            ws.close()
        except Exception:
            time.sleep(0.5)

def rest_thread():
    url = "https://clob.polymarket.com/book?token_id=" + TOKEN
    while now_ms() - start < RUN * 1000:
        t0 = now_ms()
        try:
            body = hg(url)
            j = json.loads(body)
            asks = j.get("asks") or []
            if asks:
                a = min(float(x["price"]) for x in asks)
                with lock:
                    rest_evs.append((now_ms(), a))
        except urllib.error.HTTPError as e:
            if e.code == 429:
                rest_429[0] += 1
        except Exception:
            pass
        dt = now_ms() - t0
        if dt < 7:
            time.sleep((7 - dt) / 1000.0)  # ~140/s cap

ts = [threading.Thread(target=wss_thread), threading.Thread(target=rest_thread)]
for t in ts:
    t.start()
for t in ts:
    t.join()

print("WSS updates: %d (%.1f/s)   REST polls: %d (%.1f/s, 429s=%d)" % (
    len(wss_evs), len(wss_evs) / RUN, len(rest_evs), len(rest_evs) / RUN, rest_429[0]))

def changes(evs):
    out = []; prev = None
    for t, a in sorted(evs):
        if a != prev:
            out.append((t, a)); prev = a
    return out
wc = changes(wss_evs)
rc = sorted(rest_evs)
rts = [t for t, _ in rc]
print("WSS distinct ask-changes: %d   REST distinct ask-changes: %d" % (len(wc), len(changes(rest_evs))))

leads = []
rest_missed = 0
for Tw, V in wc:
    i = bisect.bisect_left(rts, Tw - 300)
    found = None
    while i < len(rc) and rc[i][0] <= Tw + 3000:
        if abs(rc[i][1] - V) < 1e-9:
            found = rc[i][0]; break
        i += 1
    if found is None:
        rest_missed += 1
    else:
        leads.append(found - Tw)
if leads:
    leads.sort()
    n = len(leads)
    print("\n=== for each WSS ask-change, when did maxed-REST first show it? (+ = WSS saw it FIRST) ===")
    print("median lead=%.0fms  mean=%.0fms  p10=%.0fms  p90=%.0fms" % (
        statistics.median(leads), sum(leads) / n, leads[int(0.1 * n)], leads[min(int(0.9 * n), n - 1)]))
    print("WSS led (saw change before REST): %d/%d = %d%%" % (
        sum(1 for x in leads if x > 0), n, 100 * sum(1 for x in leads if x > 0) // n))
print("WSS changes REST NEVER caught (changed again between polls): %d/%d" % (rest_missed, len(wc)))
print("\n-> if WSS median lead > 0 and REST missed some changes, WSS IS faster + fresher than maxed REST")
