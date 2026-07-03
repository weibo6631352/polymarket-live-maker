#!/usr/bin/env python3
"""Measure TRUE crypto feed latency from the box = (our receive time - exchange match timestamp), which
is the signal latency that matters (NOT the TCP-edge RTT, which is just a CDN PoP). Box clock is
NTP-disciplined to ~us, so recv-minus-event-ts is an accurate one-way feed latency. ZERO real money.

Connects to each exchange's TRADE WS, captures event_ts vs recv for ~15s, reports median feed latency.
The fastest from Ireland is whichever exchange's MATCHING ENGINE is nearest (EU > US-east > US-west > Asia).
Run on the box: python3 backtest/feed_latency.py
"""
import sys
import json
import time
import asyncio

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import websockets  # noqa: E402


async def measure(name, url, sub, extract, secs=15, subprotocols=None):
    lat = []
    px = None
    try:
        async with websockets.connect(url, ping_interval=8, max_size=None) as ws:
            await ws.send(json.dumps(sub))
            stop = time.time() + secs
            while time.time() < stop:
                try:
                    msg = await asyncio.wait_for(ws.recv(), timeout=max(0.1, stop - time.time()))
                except Exception:  # noqa: BLE001
                    break
                t = time.time()
                try:
                    data = json.loads(msg)
                except Exception:  # noqa: BLE001
                    continue
                for ev_ts, p in extract(data):
                    lat.append((t - ev_ts) * 1000)
                    px = p
    except Exception as e:  # noqa: BLE001
        print(f"  {name:10s} CONNECT-FAIL: {str(e)[:50]}")
        return
    if not lat:
        print(f"  {name:10s} no trades in {secs}s (sparse)")
        return
    lat.sort()
    print(f"  {name:10s} feed latency (match->recv)  median={lat[len(lat)//2]:.0f}ms  "
          f"min={lat[0]:.0f}  p90={lat[min(len(lat)-1,int(0.9*len(lat)))]:.0f}ms  (n={len(lat)}, last px={px})")


def ex_kraken(d):
    if isinstance(d, list) and len(d) > 1 and isinstance(d[1], list):
        for tr in d[1]:
            try:
                yield (float(tr[2]), float(tr[0]))
            except Exception:  # noqa: BLE001
                pass


def ex_okx(d):
    if isinstance(d, dict) and d.get("data"):
        for x in d["data"]:
            try:
                yield (float(x["ts"]) / 1000.0, float(x["px"]))
            except Exception:  # noqa: BLE001
                pass


def ex_coinbase(d):
    if isinstance(d, dict) and d.get("type") in ("match", "last_match") and d.get("time"):
        try:
            import datetime as dt
            ts = dt.datetime.fromisoformat(d["time"].replace("Z", "+00:00")).timestamp()
            yield (ts, float(d["price"]))
        except Exception:  # noqa: BLE001
            pass


def ex_bitstamp(d):
    if isinstance(d, dict) and d.get("event") == "trade" and d.get("data"):
        try:
            yield (float(d["data"]["microtimestamp"]) / 1e6, float(d["data"]["price"]))
        except Exception:  # noqa: BLE001
            pass


def ex_binance(d):
    if isinstance(d, dict) and d.get("e") == "trade":
        try:
            yield (float(d["T"]) / 1000.0, float(d["p"]))
        except Exception:  # noqa: BLE001
            pass


async def main():
    print("# TRUE crypto feed latency from box (match-timestamp -> our recv; clock NTP-perfect)\n")
    await measure("Bitstamp", "wss://ws.bitstamp.net",
                  {"event": "bts:subscribe", "data": {"channel": "live_trades_btcusd"}}, ex_bitstamp)
    await measure("Kraken", "wss://ws.kraken.com",
                  {"event": "subscribe", "pair": ["XBT/USD"], "subscription": {"name": "trade"}}, ex_kraken)
    await measure("Coinbase", "wss://ws-feed.exchange.coinbase.com",
                  {"type": "subscribe", "product_ids": ["BTC-USD"], "channels": ["matches"]}, ex_coinbase)
    await measure("OKX", "wss://ws.okx.com:8443/ws/v5/public",
                  {"op": "subscribe", "args": [{"channel": "trades", "instId": "BTC-USDT"}]}, ex_okx)
    await measure("Binance", "wss://stream.binance.com:9443/ws/btcusdt@trade",
                  {"method": "SUBSCRIBE", "params": ["btcusdt@trade"], "id": 1}, ex_binance)


if __name__ == "__main__":
    asyncio.get_event_loop().run_until_complete(main())
