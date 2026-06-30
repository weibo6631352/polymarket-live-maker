#!/usr/bin/env python3
# strategy_compare.py — COMPREHENSIVE OKX vs Binance signal comparison on the SAME windows.
# Streams OKX + Binance + PM book; on each >THRESH/3s move (either feed) fires a tagged trigger; fills at
# the PM ask at our latency; resolves via Binance end>open. Captures FULL granularity per entry and prints a
# multi-dimensional breakdown EVERY HOUR (and at the end): by latency / tau / direction / entry-price;
# OKX/BIN trigger OVERLAP + union ("both"); risk (edge dispersion, max drawdown, max losing streak); capital
# efficiency (ROI). Hourly so you see the comprehensive picture early and it refines. Read-only, zero money.
import json, ssl, time, threading, calendar, urllib.request, websocket, bisect, statistics

THRESH = 0.0003
LATS = [0, 25, 50, 100, 200, 400]
HL = 0          # headline latency for slice tables (latency is ~flat, so L=0 is representative)
HOLDBACK = 20
RUN_MS = 7200000.0  # 2 hr — validate the (mid-late tau + cheap ask) edge pattern across multiple periods

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
entries = []                       # granular: dict(src, t, tau, up, won, fills{LAT:price})
all_trig = {"okx": [], "bin": []}
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
        triggers = []
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
                            drift = latest(bhist, blk) - opn          # window drift since open (Binance)
                            wt = (drift > 0) == (mv > 0)              # with-trend if the move agrees with the drift
                            triggers.append((now_ms(), up_tok if mv > 0 else dn_tok, src, mv > 0, end - time.time(), wt))
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
        for trig_t, fav, src, up, tau, wt in triggers:
            won = 1 if ((fav == up_tok and up_won) or (fav == dn_tok and not up_won)) else 0
            fills = {}
            for Lv in LATS:
                fill = ask_at(ah[fav], trig_t + Lv)
                if fill and 0.03 < fill < 0.97:
                    fills[Lv] = fill
            if HL in fills:
                with res_lock:
                    entries.append({"src": src, "t": trig_t, "tau": tau, "up": up, "won": won, "fills": fills, "wt": wt})
                    all_trig[src].append(trig_t)

def pct(x, n):
    return 100 * x // max(n, 1)
def edge_of(e, L=HL):
    return e["won"] - e["fills"].get(L, e["fills"][HL])

def analyze(tag):
    with res_lock:
        es_all = list(entries); ot = sorted(all_trig["okx"]); bt = sorted(all_trig["bin"])
    print("\n" + "=" * 66)
    print("COMPREHENSIVE OKX-vs-Binance  @ %s   (total entries=%d)" % (tag, len(es_all)))
    print("=" * 66)
    for src in ("okx", "bin"):
        es = [e for e in es_all if e["src"] == src]
        print("\n######## %s ########" % src.upper())
        if not es:
            print("  (no entries yet)"); continue
        n = len(es); wins = sum(e["won"] for e in es)
        eg = [edge_of(e) for e in es]; tot = sum(eg); dep = sum(e["fills"][HL] for e in es)
        print("  OVERALL n=%d  win=%d%%  edge/order=%+.4f  total=$%.2f  deployed=$%.2f  ROI=%+.1f%%" % (
            n, pct(wins, n), tot / n, tot, dep, 100 * tot / max(dep, 1)))
        print("  LATENCY:  " + "  ".join("L%d:%d%%/%+.3f" % (
            Lv, pct(sum(e["won"] for e in es if Lv in e["fills"]), max(sum(1 for e in es if Lv in e["fills"]), 1)),
            (sum(edge_of(e, Lv) for e in es if Lv in e["fills"]) / max(sum(1 for e in es if Lv in e["fills"]), 1)))
            for Lv in LATS))
        print("  TAU:      " + "  ".join("%s:n%d/%d%%/%+.3f" % (
            lab, len([e for e in es if lo <= e["tau"] < hi]),
            pct(sum(e["won"] for e in es if lo <= e["tau"] < hi), max(len([e for e in es if lo <= e["tau"] < hi]), 1)),
            (sum(edge_of(e) for e in es if lo <= e["tau"] < hi) / max(len([e for e in es if lo <= e["tau"] < hi]), 1)))
            for lo, hi, lab in [(0, 90, "<90s"), (90, 180, "90-180"), (180, 320, ">180s")]))
        print("  DIR:      " + "  ".join("%s:n%d/%d%%/%+.3f" % (
            lab, len([e for e in es if e["up"] == u]),
            pct(sum(e["won"] for e in es if e["up"] == u), max(len([e for e in es if e["up"] == u]), 1)),
            (sum(edge_of(e) for e in es if e["up"] == u) / max(len([e for e in es if e["up"] == u]), 1)))
            for u, lab in [(True, "Up"), (False, "Dn")]))
        print("  TREND:    " + "  ".join("%s:n%d/%d%%/%+.3f" % (
            lab, len([e for e in es if e.get("wt") == w]),
            pct(sum(e["won"] for e in es if e.get("wt") == w), max(len([e for e in es if e.get("wt") == w]), 1)),
            (sum(edge_of(e) for e in es if e.get("wt") == w) / max(len([e for e in es if e.get("wt") == w]), 1)))
            for w, lab in [(True, "with-trend"), (False, "counter")]))
        cheap = [e for e in es if e["fills"][HL] < 0.55]
        print("  CHEAP(ask<0.55) x TREND:  " + "  ".join("%s:n%d/%d%%/%+.3f" % (
            lab, len([e for e in cheap if e.get("wt") == w]),
            pct(sum(e["won"] for e in cheap if e.get("wt") == w), max(len([e for e in cheap if e.get("wt") == w]), 1)),
            (sum(edge_of(e) for e in cheap if e.get("wt") == w) / max(len([e for e in cheap if e.get("wt") == w]), 1)))
            for w, lab in [(True, "with"), (False, "counter")]) +
            "   <-- if cheap wins are mostly 'counter', it's the bounce TRAP (drop ask<0.55)")
        print("  ASK:      " + "  ".join("%.2f-%.2f:n%d/%d%%/%+.3f" % (
            lo, hi, len([e for e in es if lo <= e["fills"][HL] < hi]),
            pct(sum(e["won"] for e in es if lo <= e["fills"][HL] < hi), max(len([e for e in es if lo <= e["fills"][HL] < hi]), 1)),
            (sum(edge_of(e) for e in es if lo <= e["fills"][HL] < hi) / max(len([e for e in es if lo <= e["fills"][HL] < hi]), 1)))
            for lo, hi in [(0.03, 0.35), (0.35, 0.55), (0.55, 0.75), (0.75, 0.97)]))
        cum = 0.0; peak = 0.0; dd = 0.0; st = 0; mxst = 0
        for x in eg:
            cum += x; peak = max(peak, cum); dd = min(dd, cum - peak)
            st = st + 1 if x < 0 else 0; mxst = max(mxst, st)
        print("  RISK: edge_std=%.3f worst=%.3f maxDrawdown=$%.2f maxLosingStreak=%d" % (
            statistics.pstdev(eg) if n > 1 else 0.0, min(eg), dd, mxst))

    def near(times, t, w=2500):
        i = bisect.bisect_left(times, t - w)
        return i < len(times) and times[i] <= t + w
    okx_only = sum(1 for t in ot if not near(bt, t))
    bin_only = sum(1 for t in bt if not near(ot, t))
    both = len(ot) - okx_only
    print("\n######## OVERLAP / UNION ########")
    print("  OKX=%d BIN=%d | OKX-only=%d BIN-only=%d overlap=%d UNION(both)=%d" % (
        len(ot), len(bt), okx_only, bin_only, both, okx_only + bin_only + both))
    print("  -> 'both' would add %d Binance-only orders over OKX (+%d%%)" % (bin_only, pct(bin_only, len(ot))))

    oe = [e for e in es_all if e["src"] == "okx"]; be = [e for e in es_all if e["src"] == "bin"]
    if oe and be:
        o = sum(edge_of(e) for e in oe); b = sum(edge_of(e) for e in be)
        print("\n######## VERDICT ########")
        print("  higher TOTAL: %s | higher per-order: %s | more orders: %s" % (
            "OKX" if o > b else "BIN", "OKX" if o / len(oe) > b / len(be) else "BIN", "OKX" if len(oe) > len(be) else "BIN"))

def snapshot():
    last_full = now_ms()
    while now_ms() - start < RUN_MS:
        time.sleep(90)
        with res_lock:
            no = sum(1 for e in entries if e["src"] == "okx")
            nb = sum(1 for e in entries if e["src"] == "bin")
        print("[hb %.0fmin] entries=%d  okx=%d bin=%d" % ((now_ms() - start) / 60000, no + nb, no, nb), flush=True)
        if now_ms() - last_full > 600000:  # full comprehensive analysis every 10min (not just at the end)
            analyze("%.0fmin" % ((now_ms() - start) / 60000))
            last_full = now_ms()

ts = [threading.Thread(target=feed, args=("wss://stream.binance.com:9443/ws/btcusdt@bookTicker", None, p_bin, bhist, blk)),
      threading.Thread(target=feed, args=("wss://ws.okx.com:8443/ws/v5/public",
          json.dumps({"op": "subscribe", "args": [{"channel": "tickers", "instId": "BTC-USDT"}]}), p_okx, ohist, olk)),
      threading.Thread(target=snapshot, daemon=True),
      threading.Thread(target=pm)]
for t in ts:
    t.start()
ts[0].join(); ts[1].join(); ts[3].join()
analyze("FINAL %.1fh" % (RUN_MS / 3600000))
