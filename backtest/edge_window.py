#!/usr/bin/env python3
"""Measure the crypto-scalping EDGE WINDOW + depth using SERVER-SIDE timestamps. ZERO real money —
read-only WS, no orders.

Lag = PM book-update server timestamp - exchange match timestamp (both server-side, NTP-comparable) =
the TRUE time PM's quote stays stale after spot moves, independent of OUR client latency. Uses Binance
trade feed for dense, accurately-timestamped BTC moves (its high feed latency is irrelevant: we use the
match timestamp, not receive time). Also reports the DEPTH (size resting at the stale quote before it
moves). Rolls across consecutive micro-markets to catch volatility.
Run on the box: python3 backtest/edge_window.py [--probe] [--secs 300] [--asset BTC|ETH] [--thresh 0.3]
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
BINANCE_WS = "wss://stream.binance.com:9443/ws/btcusdt@trade"
BINANCE_ETH = "wss://stream.binance.com:9443/ws/ethusdt@trade"


def get(u):
    try:
        return cb.http_get(u)
    except Exception:  # noqa: BLE001
        return None


def find_live_token(asset):
    name = "Bitcoin" if asset == "BTC" else "Ethereum"
    now = time.time()
    ms = get("https://gamma-api.polymarket.com/markets?closed=false&limit=100&order=startDate&ascending=false") or []
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
        if end > now + 30 and len(tk) == 2:
            cands.append((end - start, end, tk[0], q[:42]))
    if not cands:
        return None
    cands.sort(key=lambda c: c[0])     # shortest window = most actively scalped
    return cands[0][2], cands[0][3], cands[0][1]


def best_of(levels):
    out = {}
    for lv in levels or []:
        try:
            out[float(lv["price"])] = float(lv["size"])
        except Exception:  # noqa: BLE001
            pass
    return out


async def stream_pm(tok, pm_evt, stop, raw):
    """Static token (no blocking calls in the loop). Records (pm_server_ts, mid, best_size)."""
    bids, asks = {}, {}
    try:
        async with websockets.connect(PM_WS, ping_interval=10, max_size=None) as ws:
            await ws.send(json.dumps({"assets_ids": [tok], "type": "market"}))
            nraw = 0
            while time.time() < stop:
                try:
                    msg = await asyncio.wait_for(ws.recv(), timeout=max(0.1, stop - time.time()))
                except Exception:  # noqa: BLE001
                    break
                if raw and nraw < 3:
                    print("PM>", msg[:200], flush=True); nraw += 1
                try:
                    data = json.loads(msg)
                except Exception:  # noqa: BLE001
                    continue
                for ev in (data if isinstance(data, list) else [data]):
                    try:
                        sts = float(ev.get("timestamp", 0)) / 1000.0
                    except Exception:  # noqa: BLE001
                        sts = time.time()
                    if sts < 1e9:
                        sts = time.time()
                    if ev.get("bids") is not None or ev.get("asks") is not None:
                        bids = best_of(ev.get("bids")); asks = best_of(ev.get("asks"))
                    else:
                        for c in ev.get("changes", []):
                            try:
                                p, s, side = float(c["price"]), float(c["size"]), str(c.get("side", "")).upper()
                                (bids if side in ("BUY", "BID") else asks)[p] = s
                            except Exception:  # noqa: BLE001
                                pass
                    bb = max((p for p, s in bids.items() if s > 0), default=None)
                    ba = min((p for p, s in asks.items() if s > 0), default=None)
                    if bb is not None and ba is not None:
                        depth = bids.get(bb, 0) + asks.get(ba, 0)
                        pm_evt.append((sts, (bb + ba) / 2.0, depth))
    except Exception:  # noqa: BLE001
        pass


async def stream_binance(asset, btc_evt, stop, raw):
    url = BINANCE_WS if asset == "BTC" else BINANCE_ETH
    async with websockets.connect(url, ping_interval=10) as ws:
        nraw = 0
        while time.time() < stop:
            try:
                msg = await asyncio.wait_for(ws.recv(), timeout=max(0.1, stop - time.time()))
            except Exception:  # noqa: BLE001
                break
            if raw and nraw < 3:
                print("BIN>", msg[:160], flush=True); nraw += 1
            try:
                d = json.loads(msg)
                if d.get("e") == "trade":
                    btc_evt.append((float(d["T"]) / 1000.0, float(d["p"])))   # match server ts, price
            except Exception:  # noqa: BLE001
                pass


async def one_burst(asset, dur, probe, pm_evt, btc_evt):
    r = find_live_token(asset)              # SYNC find, OUTSIDE the event loop
    if not r:
        return None
    tok, q, end = r
    dur = min(dur, max(15, end - time.time() - 6))
    stop = time.time() + dur
    await asyncio.gather(stream_pm(tok, pm_evt, stop, probe),
                         stream_binance(asset, btc_evt, stop, probe))
    return q


async def run(asset, secs, probe, thresh):
    pm_evt, btc_evt = [], []
    if probe:
        q = await one_burst(asset, 15, True, pm_evt, btc_evt)
        print(f"# market: {q}", flush=True)
    else:
        t_end = time.time() + secs
        n = 0
        while time.time() < t_end:
            q = await one_burst(asset, 80, False, pm_evt, btc_evt)
            n += 1
            print(f"# burst {n}: {q}  cumulative PM={len(pm_evt)} BTC={len(btc_evt)}", flush=True)
            if q is None:
                await asyncio.sleep(3)
    print(f"# captured: PM book updates={len(pm_evt)}  Binance trades={len(btc_evt)}", flush=True)
    if probe or len(pm_evt) < 5 or len(btc_evt) < 20:
        print("# (probe or too few PM updates — market quiet)"); return
    pm_evt.sort(); btc_evt.sort()
    lags, depths = [], []
    for i in range(1, len(btc_evt)):
        if abs(btc_evt[i][1] - btc_evt[i - 1][1]) >= thresh:
            tbtc = btc_evt[i][0]
            base = next((m for (ts, m, d) in reversed(pm_evt) if ts <= tbtc), None)
            if base is None:
                continue
            for (ts, m, d) in pm_evt:
                if ts > tbtc and abs(m - base) > 1e-9:
                    lag = (ts - tbtc) * 1000
                    if -500 < lag < 10000:
                        lags.append(lag); depths.append(d)
                    break
    if lags:
        lags.sort()
        print(f"# *** EDGE WINDOW (server-ts): PM reprices {lags[len(lags)//2]:.0f}ms after a "
              f">=${thresh} BTC move  (p25={lags[len(lags)//4]:.0f} p75={lags[3*len(lags)//4]:.0f}ms, "
              f"n={len(lags)}) ***")
        ds = sorted(depths)
        print(f"# DEPTH at the stale quote (best bid+ask size): median={ds[len(ds)//2]:.0f} shares")
    else:
        print(f"# no >=${thresh} BTC-move -> PM-reprice pairs captured (BTC quiet this run)")


def main():
    probe = "--probe" in sys.argv
    secs = int(sys.argv[sys.argv.index("--secs") + 1]) if "--secs" in sys.argv else 300
    asset = sys.argv[sys.argv.index("--asset") + 1] if "--asset" in sys.argv else "BTC"
    thresh = float(sys.argv[sys.argv.index("--thresh") + 1]) if "--thresh" in sys.argv else 0.3
    asyncio.get_event_loop().run_until_complete(run(asset, secs, probe, thresh))


if __name__ == "__main__":
    main()
