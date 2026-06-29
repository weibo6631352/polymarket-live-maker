#!/usr/bin/env python3
"""Measure the crypto-scalping EDGE WINDOW: ms-lag between BTC spot moving (Kraken WS) and the PM
'Bitcoin Up or Down [short window]' order book repricing. ZERO real money — read-only WS, no orders.

Streams concurrently, timestamps locally, maintains the PM book (mid on every change), uses Kraken
'trade' channel (every tick), then cross-correlates PM-mid vs spot for the lead-lag = the lag scalpers
exploit. Also reports Kraken feed latency (event ts vs our recv) + PM update cadence.
Run on the box: python3 backtest/edge_window.py [--probe] [--secs 70] [--asset BTC|ETH]
"""
import sys
import json
import time
import asyncio
import datetime as dt

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import curated_backtest as cb  # noqa: E402
import websockets  # noqa: E402

PM_WS = "wss://ws-subscriptions-clob.polymarket.com/ws/market"
KRAKEN_WS = "wss://ws.kraken.com"


def get(u):
    try:
        return cb.http_get(u)
    except Exception:  # noqa: BLE001
        return None


def find_live_market(asset):
    name = "Bitcoin" if asset == "BTC" else "Ethereum"
    now = time.time()
    ms = get(f"https://gamma-api.polymarket.com/markets?closed=false&limit=100&order=startDate&ascending=false") or []
    cands = []
    for m in (ms if isinstance(ms, list) else []):
        q = str(m.get("question", ""))
        if name not in q or "Up or Down" not in q:
            continue
        try:
            end = dt.datetime.fromisoformat(str(m.get("endDate", "")).replace("Z", "+00:00")).timestamp()
            start = dt.datetime.fromisoformat(str(m.get("startDate", "")).replace("Z", "+00:00")).timestamp()
        except Exception:  # noqa: BLE001
            continue
        tk = json.loads(m.get("clobTokenIds") or "[]")
        rem, dur = end - now, end - start
        if rem > 45 and len(tk) == 2:
            cands.append((dur, rem, m.get("conditionId"), tk[0], q[:46], end))
    if not cands:
        return None
    # prefer the SHORTEST-window (most actively-scalped micro-market) with enough runway
    cands.sort(key=lambda c: (c[0], -c[1]))
    d, rem, cond, tok, q, end = cands[0]
    return (cond, tok, q, end, dur)


def best_of(levels):
    out = {}
    for lv in levels or []:
        try:
            out[float(lv["price"])] = float(lv["size"])
        except Exception:  # noqa: BLE001
            pass
    return out


async def stream_pm(token, store, stop, raw):
    bids, asks = {}, {}

    def mid():
        bb = max((p for p, s in bids.items() if s > 0), default=None)
        ba = min((p for p, s in asks.items() if s > 0), default=None)
        return (bb + ba) / 2.0 if (bb is not None and ba is not None) else None

    async with websockets.connect(PM_WS, ping_interval=10, max_size=None) as ws:
        await ws.send(json.dumps({"assets_ids": [token], "type": "market"}))
        nraw = 0
        while time.time() < stop:
            try:
                msg = await asyncio.wait_for(ws.recv(), timeout=max(0.1, stop - time.time()))
            except Exception:  # noqa: BLE001
                break
            t = time.time()
            if raw and nraw < 4:
                print("PM>", msg[:260], flush=True); nraw += 1
            try:
                data = json.loads(msg)
            except Exception:  # noqa: BLE001
                continue
            for ev in (data if isinstance(data, list) else [data]):
                et = ev.get("event_type") or ev.get("type")
                if ev.get("bids") is not None or ev.get("asks") is not None:   # full book
                    bids.clear(); bids.update(best_of(ev.get("bids")))
                    asks.clear(); asks.update(best_of(ev.get("asks")))
                elif et in ("price_change", "agg_orderbook") or ev.get("changes"):
                    for c in ev.get("changes", []):
                        try:
                            p, s, side = float(c["price"]), float(c["size"]), str(c.get("side", "")).upper()
                        except Exception:  # noqa: BLE001
                            continue
                        (bids if side in ("BUY", "BID") else asks)[p] = s
                m = mid()
                if m is not None:
                    store.append((t, "PM", m))


async def stream_kraken(asset, store, stop, raw):
    pair = "XBT/USD" if asset == "BTC" else "ETH/USD"
    async with websockets.connect(KRAKEN_WS, ping_interval=10) as ws:
        await ws.send(json.dumps({"event": "subscribe", "pair": [pair], "subscription": {"name": "trade"}}))
        nraw = 0
        while time.time() < stop:
            try:
                msg = await asyncio.wait_for(ws.recv(), timeout=max(0.1, stop - time.time()))
            except Exception:  # noqa: BLE001
                break
            t = time.time()
            if raw and nraw < 4:
                print("KRK>", msg[:200], flush=True); nraw += 1
            try:
                data = json.loads(msg)
            except Exception:  # noqa: BLE001
                continue
            if isinstance(data, list) and len(data) > 1 and isinstance(data[1], list):
                for tr in data[1]:                       # [price, vol, time, side, ordtype, misc]
                    try:
                        px, evt = float(tr[0]), float(tr[2])
                        store.append((t, "KRK", px))
                        store.append((evt, "KRKevt", px))   # exchange event time (feed latency)
                    except Exception:  # noqa: BLE001
                        pass


async def run(asset, secs, probe):
    mk = find_live_market(asset)
    if not mk:
        print(f"# no live {asset} micro-market found"); return
    cond, token, q, end, dur = mk
    print(f"# live market: {q}  window={dur/60:.0f}min  ends in {end-time.time():.0f}s", flush=True)
    store = []
    d = 12 if probe else min(secs, max(20, end - time.time() - 8))
    stop = time.time() + d
    await asyncio.gather(stream_pm(token, store, stop, probe), stream_kraken(asset, store, stop, probe))
    pm = [(t, v) for (t, s, v) in store if s == "PM"]
    krk = [(t, v) for (t, s, v) in store if s == "KRK"]
    kevt = [(t, v) for (t, s, v) in store if s == "KRKevt"]
    print(f"# captured over {d:.0f}s: PM mid updates={len(pm)} Kraken trades={len(krk)}", flush=True)
    if probe:
        print("# PM mids:", [round(v, 3) for _, v in pm[-5:]], " Kraken:", [round(v, 1) for _, v in krk[-5:]])
        return
    if len(pm) < 8 or len(krk) < 8:
        print("# too few updates (market quiet) — retry in an active window"); return
    # Kraken feed latency: our recv time - exchange event time
    fl = sorted((rt - et) * 1000 for (rt, _), (et, _) in zip(krk, kevt))
    if fl:
        print(f"# Kraken feed latency (recv - exchange ts): median={fl[len(fl)//2]:.0f}ms")
    def cad(ts):
        d = sorted(t for t, _ in ts); g = sorted(d[i] - d[i-1] for i in range(1, len(d)))
        return g[len(g)//2]*1000 if g else 0
    print(f"# cadence: PM mid update median={cad(pm):.0f}ms ; Kraken trade median={cad(krk):.0f}ms")
    # edge window: each Kraken move >= $0.5 -> time to next PM mid change
    lags = []
    pm_s = sorted(pm)
    for i in range(1, len(krk)):
        if abs(krk[i][1] - krk[i-1][1]) >= 0.5:
            tm = krk[i][0]
            base = next((v for (tp, v) in reversed(pm_s) if tp <= tm), None)
            if base is None:
                continue
            nxt = next(((tp - tm) * 1000 for (tp, v) in pm_s if tp > tm and abs(v - base) > 1e-9), None)
            if nxt is not None and nxt < 8000:
                lags.append(nxt)
    if lags:
        lags.sort()
        print(f"# *** EDGE WINDOW: PM reprices after BTC move in median={lags[len(lags)//2]:.0f}ms "
              f"p25={lags[len(lags)//4]:.0f} p75={lags[3*len(lags)//4]:.0f}ms (n={len(lags)}) ***")
    else:
        print("# no BTC-move->PM-reprice pairs (quiet/efficient)")


def main():
    probe = "--probe" in sys.argv
    secs = int(sys.argv[sys.argv.index("--secs") + 1]) if "--secs" in sys.argv else 70
    asset = sys.argv[sys.argv.index("--asset") + 1] if "--asset" in sys.argv else "BTC"
    asyncio.get_event_loop().run_until_complete(run(asset, secs, probe))


if __name__ == "__main__":
    main()
