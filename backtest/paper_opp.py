#!/usr/bin/env python3
"""Multi-hour DRY paper opportunity quantifier for crypto price-latency scalping. ZERO real money —
read-only WS, NO orders, PM_TRADER_LIVE untouched.

Streams BTC spot (Binance trade feed, dense + accurate match-ts) + the live PM 'Bitcoin Up or Down'
micro-market book, across consecutive windows for hours. When BTC moves and PM's quote is STALE, it
flags a mispricing: fair P(Up) (from BTC vs the window-open price, a short-horizon GBM) vs PM's quote.
Each flagged opportunity is marked to the ACTUAL window settlement (BTC_close vs BTC_open) = model-free
ground-truth P&L. Reports: opportunity frequency/hr, mispricing magnitude (c), depth at the stale quote,
the edge window (BTC-move -> PM-reprice lag, ms), and the GROSS optimistic $/day (assume-we-get-the-fill).

Run on the box (background, hours): python3 backtest/paper_opp.py --hours 3 [--asset BTC]
Writes a rolling aggregate to /tmp/paper_opp.agg every ~60s; final aggregate to stdout.
"""
import sys
import re
import json
import math
import time
import asyncio
import datetime as dt

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import curated_backtest as cb  # noqa: E402
import websockets  # noqa: E402

BIN = {"BTC": "wss://stream.binance.com:9443/ws/btcusdt@trade",
       "ETH": "wss://stream.binance.com:9443/ws/ethusdt@trade"}
PM_WS = "wss://ws-subscriptions-clob.polymarket.com/ws/market"
NAME = {"BTC": "Bitcoin", "ETH": "Ethereum"}
AGG = "/tmp/paper_opp.agg"


def get(u):
    try:
        return cb.http_get(u)
    except Exception:  # noqa: BLE001
        return None


def ninv(p):  # inverse normal CDF (Acklam)
    if p <= 0:
        return -10.0
    if p >= 1:
        return 10.0
    a = [-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
         1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00]
    b = [-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
         6.680131188771972e+01, -1.328068155288572e+01]
    c = [-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
         -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00]
    d = [7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00]
    pl = 0.02425
    if p < pl:
        q = math.sqrt(-2 * math.log(p))
        return (((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) / ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1)
    if p > 1 - pl:
        q = math.sqrt(-2 * math.log(1 - p))
        return -(((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) / ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1)
    q = p - 0.5
    r = q * q
    return (((((a[0]*r+a[1])*r+a[2])*r+a[3])*r+a[4])*r+a[5])*q / (((((b[0]*r+b[1])*r+b[2])*r+b[3])*r+b[4])*r+1)


def normcdf(x):
    return 0.5 * (1 + math.erf(x / math.sqrt(2)))


def _ts(date_s, time_s):
    try:
        naive = dt.datetime.strptime(f"{date_s} {dt.datetime.utcnow().year} {time_s}", "%B %d %Y %I:%M%p")
        return (naive + dt.timedelta(hours=4)).replace(tzinfo=dt.timezone.utc).timestamp()  # ET(EDT)->UTC
    except Exception:  # noqa: BLE001
        return None


def parse_end(title):
    m = re.search(r'-\s*([A-Za-z]+\s+\d+),.*?-(\d+:\d+[AP]M)\s*ET', title)
    return _ts(m.group(1), m.group(2)) if m else None


def parse_start(title):
    m = re.search(r'-\s*([A-Za-z]+\s+\d+),\s*(\d+:\d+[AP]M)-', title)
    return _ts(m.group(1), m.group(2)) if m else None


def s_at(s, ts):
    return next((px for (t, px) in reversed(s.btc) if t <= ts), None)


class St:
    def __init__(self):
        self.btc = []          # (ts, px) rolling ~10min
        self.sigma = 4e-5      # per-sqrt-sec vol (BTC ~ moderate); updated from data
        self.pm_bid = self.pm_ask = self.pm_depth = None
        self.pm_upd_ts = 0.0
        self.s_at_pm = None    # BTC price when PM last quoted
        self.win = None        # {cond, s_open, end, token}
        self.opps = []         # flagged opportunities
        self.lags = []         # edge-window samples
        self.misp_start = None
        self.windows = {}      # cond -> {s_open, end, settled, flagged:[...]}


def update_sigma(s):
    if len(s.btc) < 30:
        return
    xs = s.btc[-200:]
    rs = []
    for i in range(1, len(xs)):
        dts = xs[i][0] - xs[i-1][0]
        if dts > 0 and xs[i-1][1] > 0:
            rs.append(math.log(xs[i][1]/xs[i-1][1]) / math.sqrt(dts))
    if len(rs) > 10:
        mean = sum(rs)/len(rs)
        var = sum((r-mean)**2 for r in rs)/len(rs)
        s.sigma = max(1e-6, math.sqrt(var))


def fair_up(s, S, now):
    if not s.win or not s.win.get("s_open"):
        return None
    tau = s.win["end"] - now
    if tau < 3:
        return None
    z = math.log(S / s.win["s_open"]) / (s.sigma * math.sqrt(tau))
    return normcdf(z)


async def binance_task(asset, s, stop):
    while time.time() < stop:
        try:
            async with websockets.connect(BIN[asset], ping_interval=10) as ws:
                while time.time() < stop:
                    msg = await asyncio.wait_for(ws.recv(), timeout=20)
                    d = json.loads(msg)
                    if d.get("e") == "trade":
                        t = float(d["T"]) / 1000.0
                        S = float(d["p"])
                        s.btc.append((t, S))
                        if len(s.btc) > 600:
                            s.btc = s.btc[-400:]
                        analyze(s, S, time.time())
        except Exception:  # noqa: BLE001
            await asyncio.sleep(1)


def analyze(s, S, now):
    update_sigma(s)
    if s.pm_bid is None or not s.win or not s.win.get("s_open"):
        return
    fp = fair_up(s, S, now)
    if fp is None:
        return
    edge, side = 0.0, None
    if fp > s.pm_ask + 0.005:
        edge, side = fp - s.pm_ask, "UP"          # fair Up > PM ask -> buy Up cheap
    elif fp < s.pm_bid - 0.005:
        edge, side = s.pm_bid - fp, "DOWN"        # fair Up < PM bid -> buy Down cheap
    if side and edge > 0.01:
        if s.misp_start is None:                  # NEW episode -> flag ONCE
            s.misp_start = now
            rec = {"t": now, "side": side, "edge": edge, "fair": fp, "ask": s.pm_ask, "bid": s.pm_bid,
                   "depth": s.pm_depth, "cond": s.win["cond"],
                   "entry": s.pm_ask if side == "UP" else (1 - s.pm_bid)}
            s.opps.append(rec)
            s.windows.setdefault(s.win["cond"], {"s_open": s.win["s_open"], "end": s.win["end"],
                                                 "settled": None, "flagged": []})["flagged"].append(rec)
    else:
        if s.misp_start is not None:              # mispricing cleared (BTC reverted) before PM repriced
            s.misp_start = None


async def pm_task(asset, s, stop):
    loop = asyncio.get_event_loop()
    while time.time() < stop:
        r = await loop.run_in_executor(None, find_token, asset)   # blocking gamma OFF the loop
        if not r:
            await asyncio.sleep(5); continue
        cond, token, end, q, start = r
        s_open = s_at(s, start)                    # BTC price at the window OPEN (true reference)
        if s_open is None:                         # joined too late / no series coverage -> skip window
            await asyncio.sleep(min(20, max(2, end - time.time() - 4))); continue
        s.win = {"cond": cond, "token": token, "end": end, "s_open": s_open}
        bids, asks = {}, {}
        try:
            async with websockets.connect(PM_WS, ping_interval=10, max_size=None) as ws:
                await ws.send(json.dumps({"assets_ids": [token], "type": "market"}))
                while time.time() < min(stop, end - 4):
                    msg = await asyncio.wait_for(ws.recv(), timeout=8)
                    for ev in (json.loads(msg) if msg.strip().startswith("[") else [json.loads(msg)]):
                        if ev.get("bids") is not None or ev.get("asks") is not None:
                            bids = {float(l["price"]): float(l["size"]) for l in (ev.get("bids") or [])}
                            asks = {float(l["price"]): float(l["size"]) for l in (ev.get("asks") or [])}
                        else:
                            for c in ev.get("changes", []):
                                try:
                                    p, sz, sd = float(c["price"]), float(c["size"]), str(c.get("side", "")).upper()
                                    (bids if sd in ("BUY", "BID") else asks)[p] = sz
                                except Exception:  # noqa: BLE001
                                    pass
                        bb = max((p for p, z in bids.items() if z > 0), default=None)
                        ba = min((p for p, z in asks.items() if z > 0), default=None)
                        if bb is not None and ba is not None:
                            now = time.time()
                            if s.misp_start is not None and (bb, ba) != (s.pm_bid, s.pm_ask):
                                s.lags.append((now - s.misp_start) * 1000)   # mispricing persisted until PM moved
                                s.misp_start = None
                            s.pm_bid, s.pm_ask = bb, ba
                            s.pm_depth = bids.get(bb, 0) + asks.get(ba, 0)
                            s.pm_upd_ts = now
                            s.s_at_pm = s.btc[-1][1] if s.btc else None
        except Exception:  # noqa: BLE001
            await asyncio.sleep(1)


def find_token(asset):
    now = time.time()
    ms = get("https://gamma-api.polymarket.com/markets?closed=false&limit=100&order=startDate&ascending=false") or []
    best = None
    for m in (ms if isinstance(ms, list) else []):
        q = str(m.get("question", ""))
        if NAME[asset] not in q or "Up or Down" not in q:
            continue
        end = parse_end(q)
        start = parse_start(q)
        tk = json.loads(m.get("clobTokenIds") or "[]")
        if end and start and end > now + 30 and len(tk) == 2:
            if best is None or end < best[2]:        # current window = soonest valid end
                best = (m.get("conditionId"), tk[0], end, q[:46], start)
    return best


async def settle_task(s, stop):
    loop = asyncio.get_event_loop()
    while time.time() < stop:
        await asyncio.sleep(60)
        # settle windows past close using BTC series (close vs open) = ground truth
        for cond, w in s.windows.items():
            if w["settled"] is None and time.time() > w["end"] + 2 and w["s_open"]:
                close = next((px for (t, px) in reversed(s.btc) if t <= w["end"]), None)
                if close is not None:
                    w["settled"] = 1 if close > w["s_open"] else 0
        write_agg(s)


def write_agg(s):
    settled = [w for w in s.windows.values() if w["settled"] is not None]
    flagged = [r for w in settled for r in w["flagged"]]
    realized = 0.0
    wins = 0
    for w in settled:
        up = w["settled"]
        for r in w["flagged"]:
            payoff = (1 if up else 0) if r["side"] == "UP" else (1 if not up else 0)
            pnl = payoff - r["entry"]
            realized += pnl
            wins += 1 if pnl > 0 else 0
    n = len(flagged)
    with open(AGG, "w") as f:
        f.write(f"windows_settled={len(settled)} flagged_opps={n} "
                f"median_edge_c={(sorted(r['edge'] for r in flagged)[n//2]*100 if n else 0):.1f} "
                f"median_depth={(sorted(r['depth'] for r in flagged)[n//2] if n else 0):.0f} "
                f"edge_window_ms={(sorted(s.lags)[len(s.lags)//2] if s.lags else 0):.0f} "
                f"realized_settle_pnl_per_share=${realized:.2f} win_rate={(wins/n if n else 0):.2f} "
                f"total_opps_seen={len(s.opps)} sigma={s.sigma:.2e}\n")


async def run(asset, hours):
    s = St()
    stop = time.time() + hours * 3600
    print(f"# paper opportunity quantifier: {asset}, {hours}h, dry/read-only", flush=True)
    await asyncio.gather(binance_task(asset, s, stop), pm_task(asset, s, stop), settle_task(s, stop))
    write_agg(s)
    print("# FINAL:", open(AGG).read().strip(), flush=True)


def main():
    asset = sys.argv[sys.argv.index("--asset") + 1] if "--asset" in sys.argv else "BTC"
    hours = float(sys.argv[sys.argv.index("--hours") + 1]) if "--hours" in sys.argv else 3.0
    asyncio.get_event_loop().run_until_complete(run(asset, hours))


if __name__ == "__main__":
    main()
