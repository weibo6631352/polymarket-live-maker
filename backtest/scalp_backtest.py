#!/usr/bin/env python3
"""Historical backtest: does the BTC-implied crypto-scalp strategy have a REAL edge vs PM's quote?
ZERO real money — read-only historical data, no orders, PM_TRADER_LIVE untouched.

Pulls resolved PM 'BTC/ETH Up or Down' 5-min windows (gamma end_date_min), gets each window's true
Binance BTCUSDT open/close/path (1s klines — PM resolves on Binance), and PM's traded P(Up) path
(data-api /trades). Then:
  a) CALIBRATION: Brier/log-loss of our BTC-implied fair P(Up) vs PM's quote vs the actual outcome,
     at checkpoints through the window. Who predicts better?
  b) STRATEGY P&L: take the side our fair says PM underprices (margin + [0.15,0.85] filter) at the PM
     price, hold to resolution; net after a bid-ask spread haircut. +EV or -EV?
  c) ROOT-CAUSE: with the CORRECT window-open price, does our fair give sensible values on big moves
     (~PM's near-certain quote)? Isolates open-price BUG (fixable) vs no-edge (PM efficient).
  d) realized 5-min sigma vs the model's 0.0025.
Run: python3 backtest/scalp_backtest.py [--hours 24] [--sigma 0.0025] [--spread 0.02] [--margin 0.05]
"""
import sys
import re
import json
import math
import time
import datetime as dt

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import curated_backtest as cb  # noqa: E402

GAMMA = "https://gamma-api.polymarket.com"
DA = "https://data-api.polymarket.com"
BINANCE = "https://api.binance.com/api/v3/klines"


def get(u):
    try:
        return cb.http_get(u)
    except Exception:  # noqa: BLE001
        return None


def normcdf(x):
    return 0.5 * (1 + math.erf(x / math.sqrt(2)))


def parse_window(title):
    """'Bitcoin Up or Down - June 29, 8:30AM-8:35AM ET' -> (start_ts, end_ts) UTC, 5-min only."""
    m = re.search(r"-\s*([A-Za-z]+\s+\d+),\s*(\d+:\d+[AP]M)-(\d+:\d+[AP]M)\s*ET", title)
    if not m:
        return None
    yr = dt.datetime.utcnow().year
    try:
        s = dt.datetime.strptime(f"{m.group(1)} {yr} {m.group(2)}", "%B %d %Y %I:%M%p")
        e = dt.datetime.strptime(f"{m.group(1)} {yr} {m.group(3)}", "%B %d %Y %I:%M%p")
    except Exception:  # noqa: BLE001
        return None
    st = (s + dt.timedelta(hours=4)).replace(tzinfo=dt.timezone.utc).timestamp()
    et = (e + dt.timedelta(hours=4)).replace(tzinfo=dt.timezone.utc).timestamp()
    if not (250 <= et - st <= 350):       # 5-min windows only
        return None
    return st, et


def fetch_windows(hours):
    now = time.time()
    out = []
    for back in range(0, hours, 1):       # 1h pages (gamma caps ~100 markets/query -> avoid truncation)
        lo = dt.datetime.utcfromtimestamp(now - (back+1)*3600).strftime("%Y-%m-%dT%H:%M:%SZ")
        hi = dt.datetime.utcfromtimestamp(now - back*3600 + 60).strftime("%Y-%m-%dT%H:%M:%SZ")
        ms = get(f"{GAMMA}/markets?closed=true&limit=500&end_date_min={lo}&end_date_max={hi}&order=endDate&ascending=false") or []
        for m in (ms if isinstance(ms, list) else []):
            q = str(m.get("question", ""))
            if "Up or Down" not in q or not ("Bitcoin" in q or "Ethereum" in q):
                continue
            try:
                op = json.loads(m.get("outcomePrices") or "[]")
                if len(op) != 2 or float(op[0]) not in (0.0, 1.0):
                    continue
            except Exception:  # noqa: BLE001
                continue
            w = parse_window(q)
            if not w:
                continue
            tk = json.loads(m.get("clobTokenIds") or "[]")
            outs = json.loads(m.get("outcomes") or "[]")
            if len(tk) != 2:
                continue
            up_idx = 0 if (outs and str(outs[0]).lower() in ("up", "yes")) else 0
            out.append({"q": q[:42], "cond": m.get("conditionId"), "sym": "BTCUSDT" if "Bitcoin" in q else "ETHUSDT",
                        "start": w[0], "end": w[1], "outcome": 1 if float(op[0]) > 0.5 else 0,
                        "up_token": tk[up_idx]})
        time.sleep(0.1)
    # dedupe by cond
    seen = {}
    for w in out:
        seen[w["cond"]] = w
    return list(seen.values())


def binance_path(sym, start, end):
    k = get(f"{BINANCE}?symbol={sym}&interval=1s&startTime={int(start*1000)}&endTime={int(end*1000)}&limit=600")
    if not isinstance(k, list) or len(k) < 5:
        return None
    pts = [(kl[0]/1000.0, float(kl[1]), float(kl[4])) for kl in k]   # (openTime, open, close)
    s_open = pts[0][1]
    s_close = pts[-1][2]
    series = [(t, c) for (t, o, c) in pts]                          # (ts, close)
    return s_open, s_close, series


def pm_path(cond, up_token):
    rows = []
    off = 0
    while off <= 1500:
        tr = get(f"{DA}/trades?market={cond}&limit=500&offset={off}")
        if not isinstance(tr, list) or not tr:
            break
        for t in tr:
            try:
                ts = int(t.get("timestamp", 0))
                pr = float(t.get("price", 0))
                oc = str(t.get("outcome", "")).lower()
                if pr <= 0 or pr >= 1:
                    continue
                pup = pr if oc in ("up", "yes") else 1 - pr        # normalize to P(Up)
                rows.append((ts, pup))
            except Exception:  # noqa: BLE001
                pass
        if len(tr) < 500:
            break
        off += 500
    rows.sort()
    return rows


def price_at(series, ts):
    v = None
    for (t, p) in series:
        if t <= ts:
            v = p
        else:
            break
    return v


def pm_at(rows, ts):
    v = None
    for (t, p) in rows:
        if t <= ts:
            v = p
        else:
            break
    return v


def main():
    hours = int(sys.argv[sys.argv.index("--hours") + 1]) if "--hours" in sys.argv else 24
    sigma = float(sys.argv[sys.argv.index("--sigma") + 1]) if "--sigma" in sys.argv else 0.0025
    spread = float(sys.argv[sys.argv.index("--spread") + 1]) if "--spread" in sys.argv else 0.02
    margin = float(sys.argv[sys.argv.index("--margin") + 1]) if "--margin" in sys.argv else 0.05
    wins = fetch_windows(hours)
    print(f"# {len(wins)} resolved 5-min BTC/ETH windows over ~{hours}h\n", flush=True)
    CK = [0.2, 0.4, 0.6, 0.8, 0.95]
    cal = []      # (our_fair, pm_quote, outcome)
    realized_sig = []
    pnl = 0.0
    trades = 0
    wins_pnl = 0
    bigmove_check = []   # (move_pct, our_fair_near_end, pm_quote_near_end, outcome)
    used = 0
    for w in wins:
        bp = binance_path(w["sym"], w["start"], w["end"])
        if not bp:
            continue
        s_open, s_close, series = bp
        if s_open <= 0:
            continue
        # realized sigma (std of 1s log-returns scaled to 5min)
        rr = [math.log(series[i][1]/series[i-1][1]) for i in range(1, len(series)) if series[i-1][1] > 0]
        if len(rr) > 30:
            import statistics
            realized_sig.append(statistics.pstdev(rr) * math.sqrt(300))
        rows = pm_path(w["cond"], w["up_token"])
        if len(rows) < 3:
            continue
        used += 1
        entered = False
        for f in CK:
            t = w["start"] + f * (w["end"] - w["start"])
            S = price_at(series, t)
            pmq = pm_at(rows, t)
            if S is None or pmq is None:
                continue
            tau_frac = max(1e-4, 1 - f)
            z = math.log(S / s_open) / (sigma * math.sqrt(tau_frac))
            fair = normcdf(z)
            cal.append((fair, pmq, w["outcome"]))
            if f >= 0.95:
                bigmove_check.append(((s_close-s_open)/s_open*100, fair, pmq, w["outcome"]))
            # strategy: enter once, on first mispricing
            if not entered and 0.15 <= pmq <= 0.85:
                if fair > pmq + margin:        # PM underprices Up -> buy Up at pmq+spread/2
                    entry = pmq + spread/2
                    pnl += w["outcome"] - entry; trades += 1; wins_pnl += 1 if (w["outcome"]-entry) > 0 else 0
                    entered = True
                elif fair < pmq - margin:       # PM overprices Up -> buy Down at (1-pmq)+spread/2
                    entry = (1 - pmq) + spread/2
                    pnl += (1 - w["outcome"]) - entry; trades += 1; wins_pnl += 1 if ((1-w["outcome"])-entry) > 0 else 0
                    entered = True
        time.sleep(0.03)

    print(f"# usable windows (Binance+PM data): {used}")
    if realized_sig:
        import statistics
        print(f"# realized 5-min sigma: median={statistics.median(realized_sig):.4f} (model uses {sigma}) "
              f"-> model is {'OK' if abs(statistics.median(realized_sig)-sigma)<0.0015 else 'MIS-SPECIFIED'}")
    # CALIBRATION
    if cal:
        bo = sum((f-o)**2 for f, p, o in cal)/len(cal)
        bp = sum((p-o)**2 for f, p, o in cal)/len(cal)
        def ll(x, o):
            x = min(max(x, 1e-6), 1-1e-6); return -(o*math.log(x)+(1-o)*math.log(1-x))
        lo = sum(ll(f, o) for f, p, o in cal)/len(cal)
        lp = sum(ll(p, o) for f, p, o in cal)/len(cal)
        print(f"\n# === CALIBRATION (n={len(cal)} checkpoints) — lower is better ===")
        print(f"#   Brier:    our_fair={bo:.4f}   PM_quote={bp:.4f}   -> {'OUR MODEL' if bo<bp else 'PM'} better")
        print(f"#   LogLoss:  our_fair={lo:.4f}   PM_quote={lp:.4f}   -> {'OUR MODEL' if lo<lp else 'PM'} better")
    # ROOT-CAUSE: big-move sanity
    big = [x for x in bigmove_check if abs(x[0]) > 0.15]
    if big:
        avgfair = sum(x[1] for x in big)/len(big)
        avgpm = sum(x[2] for x in big)/len(big)
        agree = sum(1 for x in big if (x[1] > 0.5) == (x[0] > 0))/len(big)
        print(f"\n# === ROOT-CAUSE (big moves >0.15%, near window end, n={len(big)}) ===")
        print(f"#   our_fair avg={avgfair:.3f}  PM_quote avg={avgpm:.3f}  our_fair sign-agrees-with-move {agree*100:.0f}%")
        print(f"#   -> with CORRECT open price our model {'gives sensible near-certain values' if (agree>0.8) else 'is mis-specified'}")
    # STRATEGY P&L
    print(f"\n# === STRATEGY P&L (margin={margin}, spread={spread}, hold-to-resolution) ===")
    if trades:
        print(f"#   trades={trades}  win_rate={wins_pnl/trades:.2f}  NET P&L per share=${pnl:.3f}  "
              f"avg/trade=${pnl/trades:+.4f}  -> {'+EV (edge)' if pnl>0 else '-EV (no edge / spread eats it)'}")
    else:
        print("#   no trades triggered (no mispricings beyond margin)")


if __name__ == "__main__":
    main()
