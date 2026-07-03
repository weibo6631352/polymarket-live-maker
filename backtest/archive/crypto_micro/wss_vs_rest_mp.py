#!/usr/bin/env python3
# wss_vs_rest_mp.py — TRUE ~140/s REST via multiprocessing (bypass the json-parse GIL) vs PM WSS.
# 6 separate processes each poll /book as fast as they can (no GIL contention) -> combined ~140/s; the WSS
# runs in the main process. Then merge and compare: does WSS still see ask-changes first + catch ones REST
# misses, when REST is genuinely maxed? Definitive answer to "is PM WSS faster+fresher than 149Hz REST".
import json, ssl, time, threading, calendar, urllib.request, websocket, statistics, bisect, multiprocessing

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
            if tks and 60 <= tau <= 295:
                if not best or abs(tau - 200) < abs(best[0] - 200):
                    best = (tau, tks[0])
    return best[1] if best else None

def rest_proc(token, run, start, outfile):
    url = "https://clob.polymarket.com/book?token_id=" + token
    rows = []
    while now_ms() - start < run * 1000:
        try:
            j = json.loads(hg(url))
            asks = j.get("asks") or []
            if asks:
                rows.append((now_ms(), min(float(x["price"]) for x in asks)))
        except Exception:
            pass
    with open(outfile, "w") as f:
        for t, a in rows:
            f.write("%f %.6f\n" % (t, a))

RUN = 120.0
NPROC = 6
wss_evs = []
lock = threading.Lock()

def wss_thread(token, start):
    while now_ms() - start < RUN * 1000:
        try:
            ws = websocket.create_connection("wss://ws-subscriptions-clob.polymarket.com/ws/market", sslopt={"cert_reqs": ssl.CERT_NONE})
            ws.send(json.dumps({"assets_ids": [token], "type": "market"}))
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
                            if pc.get("asset_id") == token:
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

if __name__ == "__main__":
    TOKEN = None
    for _ in range(24):
        TOKEN = discover()
        if TOKEN:
            break
        time.sleep(5)
    print("token:", (TOKEN[:18] if TOKEN else None))
    if not TOKEN:
        raise SystemExit("no active window")
    start = now_ms()
    procs = [multiprocessing.Process(target=rest_proc, args=(TOKEN, RUN, start, "/tmp/rest_%d.txt" % i)) for i in range(NPROC)]
    wt = threading.Thread(target=wss_thread, args=(TOKEN, start))
    wt.start()
    for p in procs:
        p.start()
    wt.join()
    for p in procs:
        p.join()

    rest_evs = []
    for i in range(NPROC):
        try:
            with open("/tmp/rest_%d.txt" % i) as f:
                for line in f:
                    t, a = line.split()
                    rest_evs.append((float(t), float(a)))
        except Exception:
            pass
    rest_evs.sort()
    print("WSS updates: %d (%.1f/s)   REST polls: %d (%.1f/s, %d procs)" % (
        len(wss_evs), len(wss_evs) / RUN, len(rest_evs), len(rest_evs) / RUN, NPROC))

    def changes(evs):
        out = []; prev = None
        for t, a in sorted(evs):
            if a != prev:
                out.append((t, a)); prev = a
        return out
    wc = changes(wss_evs)
    rc = rest_evs
    rts = [t for t, _ in rc]
    print("WSS distinct ask-changes: %d   REST distinct: %d" % (len(wc), len(changes(rest_evs))))
    leads = []; missed = 0
    for Tw, V in wc:
        i = bisect.bisect_left(rts, Tw - 300)
        found = None
        while i < len(rc) and rc[i][0] <= Tw + 3000:
            if abs(rc[i][1] - V) < 1e-9:
                found = rc[i][0]; break
            i += 1
        if found is None:
            missed += 1
        else:
            leads.append(found - Tw)
    if leads:
        leads.sort(); n = len(leads)
        print("median lead=%.0fms  mean=%.0fms  p10=%.0fms  p90=%.0fms" % (
            statistics.median(leads), sum(leads) / n, leads[int(0.1 * n)], leads[min(int(0.9 * n), n - 1)]))
        print("WSS led (saw change first): %d/%d = %d%%" % (
            sum(1 for x in leads if x > 0), n, 100 * sum(1 for x in leads if x > 0) // n))
    print("WSS changes maxed-REST NEVER caught: %d/%d = %d%%" % (missed, len(wc), 100 * missed // max(len(wc), 1)))
