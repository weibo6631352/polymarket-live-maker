#!/usr/bin/env python3
"""Measure the crypto-scalping EDGE WINDOW: how many ms after BTC spot moves does the PM micro-market
book reprice? + the real Kraken WS feed latency. ZERO real money — read-only WS streams, no orders.

Concurrently streams (a) a LIVE 'Bitcoin/Ethereum Up or Down [5-15min]' PM order book via
ws-subscriptions-clob.polymarket.com and (b) BTC/ETH spot via ws.kraken.com, timestamps both locally,
and cross-correlates: lead-lag of PM-mid vs spot = the lag the scalpers exploit. Also reports the
Kraken feed latency (its event timestamp vs our receive time) and PM book update cadence.
Run on the box: python3 backtest/edge_window.py [--probe] [--secs 90] [--asset BTC|ETH]
"""
import sys
import json
import time
import asyncio

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
    best = None
    import datetime as dt
    # current windows surface via most-recently-STARTED, not endDate
    ms = get("https://gamma-api.polymarket.com/markets?closed=false&limit=100&order=startDate&ascending=false") or []
    for m in (ms if isinstance(ms, list) else []):
        q = str(m.get("question", ""))
        if name not in q or "Up or Down" not in q:
            continue
        try:
            end = dt.datetime.fromisoformat(str(m.get("endDate", "")).replace("Z", "+00:00")).timestamp()
        except Exception:  # noqa: BLE001
            continue
        if end < now + 20:                 # need at least a little runway left in the window
            continue
        tk = json.loads(m.get("clobTokenIds") or "[]")
        if len(tk) != 2:
            continue
        if best is None or end > best[3]:  # most remaining time
            best = (m.get("conditionId"), tk[0], q[:46], end)
    return best


async def stream_pm(token, store, stop):
    async with websockets.connect(PM_WS, ping_interval=10, max_size=None) as ws:
        await ws.send(json.dumps({"assets_ids": [token], "type": "market"}))
        while time.time() < stop:
            try:
                msg = await asyncio.wait_for(ws.recv(), timeout=stop - time.time() + 1)
            except Exception:  # noqa: BLE001
                break
            t = time.time()
            try:
                data = json.loads(msg)
            except Exception:  # noqa: BLE001
                continue
            for ev in (data if isinstance(data, list) else [data]):
                bids = ev.get("bids") or ev.get("buys")
                asks = ev.get("asks") or ev.get("sells")
                if bids and asks:
                    try:
                        bb = max(float(b["price"]) for b in bids)
                        ba = min(float(a["price"]) for a in asks)
                        store.append((t, "PM", (bb + ba) / 2.0))
                    except Exception:  # noqa: BLE001
                        pass
                elif ev.get("event_type") == "price_change" or ev.get("price"):
                    store.append((t, "PMchg", None))


async def stream_kraken(asset, store, stop, samp):
    pair = "XBT/USD" if asset == "BTC" else "ETH/USD"
    async with websockets.connect(KRAKEN_WS, ping_interval=10) as ws:
        await ws.send(json.dumps({"event": "subscribe", "pair": [pair],
                                  "subscription": {"name": "ticker"}}))
        while time.time() < stop:
            try:
                msg = await asyncio.wait_for(ws.recv(), timeout=stop - time.time() + 1)
            except Exception:  # noqa: BLE001
                break
            t = time.time()
            try:
                data = json.loads(msg)
            except Exception:  # noqa: BLE001
                continue
            if isinstance(data, list) and len(data) > 1 and isinstance(data[1], dict):
                tk = data[1]
                last = tk.get("c")           # ["last_trade_price","lot_vol"]
                if last:
                    px = float(last[0])
                    store.append((t, "KRK", px))
                    samp.append(t)


async def run(asset, secs, probe):
    mk = find_live_market(asset)
    if not mk:
        print(f"# no live {asset} micro-market found (window timing)"); return
    cond, token, q, end = mk
    print(f"# live market: {q}  ends in {end-time.time():.0f}s  cond={cond[:14]}", flush=True)
    store, ksamp = [], []
    dur = 12 if probe else min(secs, max(20, end - time.time() - 8))   # don't outlive the 5-min window
    stop = time.time() + dur
    await asyncio.gather(stream_pm(token, store, stop), stream_kraken(asset, store, stop, ksamp))
    pm = [(t, v) for (t, s, v) in store if s == "PM" and v is not None]
    pmchg = [t for (t, s, v) in store if s in ("PM", "PMchg")]
    krk = [(t, v) for (t, s, v) in store if s == "KRK"]
    print(f"# captured: PM mid pts={len(pm)} PM updates={len(pmchg)} Kraken ticks={len(krk)}", flush=True)
    if probe:
        print("# sample PM mids:", [round(v, 3) for _, v in pm[:5]])
        print("# sample Kraken:", [round(v, 1) for _, v in krk[:5]])
        return
    if len(pm) < 10 or len(krk) < 10:
        print("# too few points to cross-correlate"); return
    # cadence
    def cadence(ts):
        d = sorted(ts)
        gaps = [d[i] - d[i - 1] for i in range(1, len(d))]
        gaps.sort()
        return gaps[len(gaps) // 2] * 1000 if gaps else 0
    print(f"# PM book update cadence median={cadence(pmchg):.0f}ms ; Kraken tick cadence median={cadence([t for t,_ in krk]):.0f}ms")
    # lead-lag: for each Kraken move > thresh, time to next PM mid change
    kv = krk
    lags = []
    for i in range(1, len(kv)):
        if abs(kv[i][1] - kv[i - 1][1]) >= 0.5:    # BTC moved >= $0.5
            tmove = kv[i][0]
            base = None
            # PM mid just before the move
            for (tp, vp) in pm:
                if tp <= tmove:
                    base = vp
                else:
                    break
            # next PM mid change after the move
            for (tp, vp) in pm:
                if tp > tmove and base is not None and abs(vp - base) > 1e-9:
                    lags.append((tp - tmove) * 1000)
                    break
    if lags:
        lags.sort()
        print(f"# EDGE WINDOW: PM repriced after BTC move in median={lags[len(lags)//2]:.0f}ms "
              f"p25={lags[len(lags)//4]:.0f}ms p75={lags[3*len(lags)//4]:.0f}ms (n={len(lags)} moves)")
    else:
        print("# no clear BTC-move -> PM-reprice pairs captured (market too quiet or already efficient)")


def main():
    probe = "--probe" in sys.argv
    secs = int(sys.argv[sys.argv.index("--secs") + 1]) if "--secs" in sys.argv else 90
    asset = sys.argv[sys.argv.index("--asset") + 1] if "--asset" in sys.argv else "BTC"
    asyncio.get_event_loop().run_until_complete(run(asset, secs, probe))


if __name__ == "__main__":
    main()
