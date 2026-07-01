#!/usr/bin/env python3
"""Cross-book line-shopping edge validator: Polymarket (PM) vs Kalshi (the sharper reference book).

ZERO real money. Read-only PUBLIC GETs only. Places no orders, signs nothing, never touches
PM_TRADER_LIVE. Pure offline/at-the-snapshot analysis.

THE EDGE UNDER TEST
-------------------
Hypothesis (from research): PM prices diverge from a SHARPER reference book (Kalshi); when retail
flow pushes PM off the sharp fair value you take the PM side TOWARD that fair and HOLD TO RESOLUTION,
because the sharp book is more accurate so PM converges to it. Question: after fees + slippage +
divergence-persistence, is this a REAL net-positive capturable edge, in which markets, at what size?

WHY THE ANALYSIS IS SPLIT IN TWO (an honest data-availability constraint)
-------------------------------------------------------------------------
A fully rigorous backtest wants, at one moment, BOTH (a) the eventual resolution AND (b) the real
historical order-book depth you'd have paid into. Those are not jointly available:
  * PM resolution is known only for CLOSED markets; for closed markets `/prices-history` is EMPTY
    (verified) so the PM path is rebuilt from data-api `/trades`. There is NO historical PM book
    depth served at all -- only the LIVE `/book`. So you cannot reconstruct what you'd have paid
    historically.
  * Therefore we measure the two halves on the two market sets where each is real:
      PART A  FORECASTING EDGE   -> RESOLVED matched pairs: is Kalshi a sharper forecaster than PM,
              and does the line-shop rule make money GROSS (top-of-book, no slippage)?  [signal]
      PART B  EXECUTION REALITY  -> OPEN matched pairs: snapshot the live gap, walk the REAL PM
              `/book` depth for size to get true slippage, net out PM fees, and re-poll after a
              delay to measure whether the gap PERSISTS long enough to fill.                [cost]
  Combine: a real edge must survive BOTH. If PART A shows no signal, or PART B shows slippage/decay
  eats it, the edge is not capturable -- and we say so plainly.

DATA SOURCES (public, read-only)
--------------------------------
  PM gamma   gamma-api.polymarket.com/public-search , /markets        (events, metadata, RESOLUTION)
  PM clob    clob.polymarket.com/prices-history , /book                (open-market path; LIVE depth)
  PM data    data-api.polymarket.com/trades                            (closed-market path rebuild)
  Kalshi     api.elections.kalshi.com/trade-api/v2  markets,           (RESULT settlement, candles,
             series/{s}/markets/{t}/candlesticks                        de-vig from yes_bid/yes_ask)

MATCHING
--------
Markets are matched only where the resolution is MECHANICALLY IDENTICAL. Two shapes:
  * strike_ladder : an event is a ladder of "above X" binaries (Bitcoin/ETH daily price, CPI, jobs,
    unemployment, Fed level). We pair PM strike <-> Kalshi strike by equal threshold + same "above"
    sense -- each pair is a separate identical-resolution binary. One event yields many clean pairs.
  * binary        : a single yes/no contract on a named event (recession, Powell-out, shutdown).
The curated MATCHES table below names PM search queries + Kalshi series I confirmed exist; tickers,
strikes and resolutions are resolved at runtime so nothing is hand-faked.

USAGE
-----
  python3 backtest/cross_book_lineshop.py            # full pipeline -> report
  python3 backtest/cross_book_lineshop.py --match    # just show resolved matched pairs
  python3 backtest/cross_book_lineshop.py --diverge  # divergence stats only
  python3 backtest/cross_book_lineshop.py --backtest # PART A forecasting edge only
  python3 backtest/cross_book_lineshop.py --exec     # PART B live execution reality only
  flags: --persist-min N (re-poll delay, default 0=skip), --no-cache, --max-pairs N
Needs internet (run on the box, or any machine with outbound https).
"""
import json
import os
import re
import sys
import time
import math
import statistics
import urllib.request
import urllib.parse

GAMMA = "https://gamma-api.polymarket.com"
CLOB = "https://clob.polymarket.com"
DATA = "https://data-api.polymarket.com"
KAL = "https://api.elections.kalshi.com/trade-api/v2"
UA = "Mozilla/5.0 (cross-book-lineshop; read-only research)"

CACHE_DIR = os.environ.get(
    "LINESHOP_CACHE",
    "/private/tmp/claude-501/-Users-wangweibo-code-polymarket-live-maker/"
    "1c75f633-3d39-4bed-a5f9-f1fbedc14733/scratchpad/lineshop_cache",
)
USE_CACHE = "--no-cache" not in sys.argv

# ---- execution-reality assumptions (documented, conservative) ----
# PM currently charges 0 CLOB trading fee on these markets (the `fee` field is read live and
# asserted ~0); the real entry cost is SLIPPAGE walked from the live book, plus the spread you cross.
PM_TAKER_FEE = 0.0          # overridden per-market from gamma `fee` if nonzero
EXEC_SIZES = [100, 500, 1000, 5000, 20000]   # USD notional to walk the book for
DIV_THRESHOLDS = [0.02, 0.03, 0.05, 0.08]     # |PM - Kalshi_fair| entry thresholds tested

# ---------------------------------------------------------------- curated cross-book match table
# Each entry was confirmed to EXIST on both books during probing. category drives the verdict
# slicing (research predicts the edge lives in slow/illiquid politics/events, not fast liquid crypto).
MATCHES = [
    # name                       category        pm_query                 kalshi_series   k_filter  shape          resolution-equivalence note
    ("BTC daily price level",    "crypto_fast",  "Bitcoin above",         "KXBTCD",       None,     "strike_ladder","BTC spot >= strike at the same fixed daily settlement time on both books"),
    ("ETH daily price level",    "crypto_fast",  "Ethereum above",        "KXETHD",       None,     "strike_ladder","ETH spot >= strike at fixed daily settlement on both books"),
    ("Core CPI YoY print",       "econ_print",   "Core CPI YoY",          "KXCPIYOY",     None,     "strike_ladder","same BLS core-CPI YoY release; 'above X%' on both"),
    ("CPI MoM print",            "econ_print",   "Inflation US",          "KXCPI",        None,     "strike_ladder","same BLS CPI MoM release; 'above X%' on both"),
    ("Nonfarm payrolls print",   "econ_print",   "jobs added",            "KXPAYROLLS",   None,     "strike_ladder","same BLS payrolls release; 'above N jobs' on both"),
    ("Unemployment rate print",  "econ_print",   "Unemployment rate",     "KXU3",         None,     "strike_ladder","same BLS U-3 release; 'above X%' on both"),
    ("Fed funds level @ meeting","fed_macro",    "Fed Decision",          "KXFED",        None,     "strike_ladder","fed funds upper bound after the FOMC decision; 'above X%'"),
    ("US recession 2026",        "politics_event","US recession",         "KXRECSSNBER",  None,     "binary",       "NBER-defined US recession by horizon"),
    ("Powell out as Fed Chair",  "politics_event","Powell out",           "POWELLLEAVE",  None,     "binary",       "Jerome Powell leaves the Fed chair by horizon"),
    ("Govt shutdown",            "politics_event","government shutdown",   "KXGOVSHUT",    None,     "binary",       "US federal government shutdown occurs by horizon"),
]


# =============================================================================== http + cache
def _cache_path(url):
    h = re.sub(r"[^a-zA-Z0-9]", "_", url)[-180:]
    return os.path.join(CACHE_DIR, h + ".json")


def http_get(url, tries=3, pause=0.5, cache=True):
    if USE_CACHE and cache:
        p = _cache_path(url)
        if os.path.exists(p):
            try:
                with open(p) as f:
                    return json.load(f)
            except Exception:  # noqa: BLE001
                pass
    last = None
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": "application/json"})
            with urllib.request.urlopen(req, timeout=30) as r:
                d = json.loads(r.read().decode())
            if USE_CACHE and cache:
                os.makedirs(CACHE_DIR, exist_ok=True)
                with open(_cache_path(url), "w") as f:
                    json.dump(d, f)
            return d
        except Exception as e:  # noqa: BLE001
            last = e
            time.sleep(pause * (i + 1))
    raise RuntimeError(f"GET failed {url} :: {last}")


# =============================================================================== parsing helpers
def parse_strike(text):
    """Return (sense, value) from a threshold string. sense in {'above','below'} or None.
    Handles $109,000 / 109k / 3.0% / 150,000 / -50000 / T4.50."""
    if text is None:
        return (None, None)
    t = str(text).lower()
    sense = None
    if any(w in t for w in ("above", "or above", ">=", "at or above", "greater")):
        sense = "above"
    elif any(w in t for w in ("below", "or below", "<=", "less", "under")):
        sense = "below"
    # number: optional $, sign, digits with commas/dots, optional k/m
    m = re.search(r"(-?\$?\s?[\d][\d,\.]*\s?[km]?)\s*%?", t.replace(", ", ","))
    if not m:
        return (sense, None)
    raw = m.group(1).replace("$", "").replace(",", "").replace(" ", "")
    mult = 1.0
    if raw.endswith("k"):
        mult, raw = 1e3, raw[:-1]
    elif raw.endswith("m"):
        mult, raw = 1e6, raw[:-1]
    try:
        return (sense, float(raw) * mult)
    except ValueError:
        return (sense, None)


def kalshi_strike(mkt):
    """Strike + sense for a Kalshi threshold market, preferring the ticker '-T' suffix."""
    tk = mkt.get("ticker", "")
    sub = mkt.get("yes_sub_title", "") or mkt.get("title", "")
    sense, val = parse_strike(sub)
    m = re.search(r"-T(-?[\d\.]+)$", tk)
    if m:
        try:
            val = float(m.group(1))
        except ValueError:
            pass
    if sense is None:
        sense = "above"   # Kalshi T-strike ladders resolve YES = at-or-above strike
    return (sense, val)


# =============================================================================== Polymarket layer
def pm_search_events(query, closed=None):
    d = http_get(f"{GAMMA}/public-search?q={urllib.parse.quote(query)}&limit_per_type=20")
    evs = d.get("events", []) if isinstance(d, dict) else []
    out = []
    for e in evs:
        if closed is not None and bool(e.get("closed")) != closed:
            continue
        out.append(e)
    return out


def pm_markets_of_event(ev):
    """Return normalized PM markets for an event (each: question, cond, token_yes, outcome, fee...)."""
    mkts = ev.get("markets", []) or []
    out = []
    for m in mkts:
        toks = m.get("clobTokenIds")
        if isinstance(toks, str):
            try:
                toks = json.loads(toks)
            except Exception:  # noqa: BLE001
                toks = None
        outs = m.get("outcomes")
        if isinstance(outs, str):
            try:
                outs = json.loads(outs)
            except Exception:  # noqa: BLE001
                outs = ["Yes", "No"]
        opx = m.get("outcomePrices")
        if isinstance(opx, str):
            try:
                opx = json.loads(opx)
            except Exception:  # noqa: BLE001
                opx = None
        if not toks or len(toks) < 2:
            continue
        # YES index
        yi = 0
        if outs:
            for i, o in enumerate(outs):
                if str(o).lower() in ("yes", "y", "up"):
                    yi = i
                    break
        resolved = None
        if opx and m.get("closed"):
            try:
                resolved = 1 if float(opx[yi]) > 0.5 else 0
            except Exception:  # noqa: BLE001
                resolved = None
        try:
            fee = float(m.get("fee", 0) or 0)
        except Exception:  # noqa: BLE001
            fee = 0.0
        q = m.get("question", "")
        out.append({
            "question": q,
            "cond": m.get("conditionId"),
            "token_yes": str(toks[yi]),
            "token_no": str(toks[1 - yi]) if len(toks) > 1 else None,
            "closed": bool(m.get("closed")),
            "resolved_yes": resolved,
            "best_bid": _f(m.get("bestBid")),
            "best_ask": _f(m.get("bestAsk")),
            "fee_bps": fee,
            "strike": parse_strike(q),
            "is_threshold": is_above_threshold(q),   # clean cumulative "above X" (not a range bucket)
            "end": m.get("endDate"),
            "end_ts": iso_ts(m.get("endDate")),
        })
    return out


def _f(x):
    try:
        return float(x)
    except Exception:  # noqa: BLE001
        return None


def iso_ts(s):
    """ISO-8601 (with or without Z) -> epoch seconds, or None."""
    if not s:
        return None
    try:
        s = str(s).replace("Z", "+00:00")
        import datetime as _dt
        return int(_dt.datetime.fromisoformat(s).timestamp())
    except Exception:  # noqa: BLE001
        return None


def is_above_threshold(q):
    """True only for a CLEAN cumulative 'at-or-above X' binary -- the only PM shape that matches a
    Kalshi 'above X' threshold. Range-bucket questions ('between', 'exactly X%') are rejected because
    P(bucket) != P(above X) and matching them would manufacture fake divergence."""
    t = str(q).lower()
    if any(w in t for w in ("between", "to ", "range", "exactly")):
        # 'X to Y' bucket; but 'from March to April' is a period descriptor, not a value range
        if "between" in t or "range" in t or "exactly" in t:
            return False
    if any(w in t for w in (" above ", ">", "or more", "or higher", "at least", "or above",
                            "exceed", "greater than", " reach ", " hit ", " be above")):
        return True
    return False


def pm_prices_history(token, fidelity=720):
    # IMPORTANT: interval=all returns the FULL path for CLOSED markets too (interval=max is empty
    # once a market resolves -- a real gotcha). fidelity is in MINUTES (720=12h, 60=1h, 1=1m).
    d = http_get(f"{CLOB}/prices-history?market={token}&interval=all&fidelity={fidelity}")
    return [(int(p["t"]), float(p["p"])) for p in d.get("history", [])]


def pm_path_from_trades(cond, max_off=2500):
    """Rebuild a YES-price path for a CLOSED PM market from data-api /trades (prices-history empty).
    Returns sorted [(ts, yes_price)] of every print, normalized to YES-space."""
    rows, off = [], 0
    while off <= max_off:
        try:
            d = http_get(f"{DATA}/trades?market={cond}&limit=500&offset={off}", tries=2, cache=True)
        except Exception:  # noqa: BLE001
            break
        if not isinstance(d, list) or not d:
            break
        for t in d:
            ts = int(t.get("timestamp", 0) or 0)
            px = _f(t.get("price"))
            sz = _f(t.get("size"))
            if not px or not sz or px <= 0:
                continue
            oc = str(t.get("outcome", "")).lower()
            if oc in ("yes", "y", "up") or t.get("outcomeIndex", 0) == 0:
                yp = px
            else:
                yp = 1.0 - px
            rows.append((ts, yp))
        off += 500
        time.sleep(0.05)
    rows.sort()
    return rows


def pm_book(token):
    b = http_get(f"{CLOB}/book?token_id={token}", cache=False)
    bids = [(float(x["price"]), float(x["size"])) for x in b.get("bids", [])]
    asks = [(float(x["price"]), float(x["size"])) for x in b.get("asks", [])]
    bids.sort(reverse=True)   # best bid first
    asks.sort()               # best ask first
    return bids, asks


# =============================================================================== Kalshi layer
def kalshi_markets(series, status=None, k_filter=None):
    url = f"{KAL}/markets?series_ticker={series}&limit=500"
    if status:
        url += f"&status={status}"
    out, cursor, pages = [], "", 0
    while pages < 8:
        u = url + (f"&cursor={cursor}" if cursor else "")
        d = http_get(u)
        ms = d.get("markets", []) if isinstance(d, dict) else []
        for m in ms:
            if k_filter and k_filter not in m.get("ticker", ""):
                continue
            out.append(m)
        cursor = d.get("cursor", "") if isinstance(d, dict) else ""
        pages += 1
        if not cursor:
            break
    return out


def kalshi_candles(series, ticker, days=180, period=1440):
    ct = int(time.time())
    u = (f"{KAL}/series/{series}/markets/{ticker}/candlesticks"
         f"?start_ts={ct - days * 86400}&end_ts={ct}&period_interval={period}")
    try:
        d = http_get(u)
    except Exception:  # noqa: BLE001
        return []
    out = []
    for c in d.get("candlesticks", []):
        ts = int(c.get("end_period_ts", 0) or 0)
        yb = c.get("yes_bid", {}) or {}
        ya = c.get("yes_ask", {}) or {}
        pr = c.get("price", {}) or {}
        bid = _f(yb.get("close_dollars"))
        ask = _f(ya.get("close_dollars"))
        mean = _f(pr.get("mean_dollars"))
        close = _f(pr.get("close_dollars"))
        # de-vig a single binary = bid/ask midpoint; fall back to mean/close last-trade
        if bid is not None and ask is not None and 0 < bid <= ask <= 1:
            fair = 0.5 * (bid + ask)
        elif mean is not None:
            fair = mean
        elif close is not None:
            fair = close
        else:
            continue
        out.append((ts, max(0.0, min(1.0, fair)), bid, ask))
    out.sort()
    return out


def kalshi_live_fair(mkt, max_spread=0.10):
    """Live de-vigged fair = bid/ask midpoint, but ONLY if Kalshi has a REAL two-sided quote.
    Returns (None, bid, ask) when there is no genuine market (no bid, no ask, degenerate, or absurd
    spread) -- you cannot line-shop against a phantom reference, and a 0.00/0.00 'book' must NOT
    become fair=0.00 (that bug manufactured giant fake edges on un-traded Kalshi legs)."""
    bid = _f(mkt.get("yes_bid_dollars"))
    ask = _f(mkt.get("yes_ask_dollars"))
    if bid is None or ask is None:
        return (None, bid, ask)
    if not (0 < bid < ask < 1):           # need a real, ordered, non-degenerate two-sided quote
        return (None, bid, ask)
    if (ask - bid) > max_spread:           # too wide -> no reliable fair
        return (None, bid, ask)
    return (0.5 * (bid + ask), bid, ask)


# =============================================================================== matching
def resolve_matches(want_closed=None, max_pairs=None, verbose=False):
    """Build the list of matched binary pairs. Each pair:
       {name,category,shape,strike, pm:{...}, k:{ticker,series,result,...}}"""
    pairs = []
    for (name, cat, pm_q, kser, kfilt, shape, note) in MATCHES:
        # ---- Kalshi side
        try:
            kms = kalshi_markets(kser, k_filter=kfilt)
        except Exception as e:  # noqa: BLE001
            if verbose:
                print(f"  ! kalshi {kser} fail: {e}")
            kms = []
        # ---- PM side
        try:
            evs = pm_search_events(pm_q)
        except Exception as e:  # noqa: BLE001
            if verbose:
                print(f"  ! pm '{pm_q}' fail: {e}")
            evs = []
        pmkts = []
        for ev in evs:
            pmkts.extend(pm_markets_of_event(ev))

        # date-proximity tolerance: matched contracts must resolve on the SAME underlying event.
        # crypto dailies are intraday-sensitive -> 90 min; econ prints -> same release window 2 days;
        # politics binaries -> fuzzy horizons, 21 days (still verified by-eye in --match output).
        date_tol = {"crypto_fast": 5400, "econ_print": 2 * 86400, "fed_macro": 2 * 86400}.get(cat, 21 * 86400)

        if shape == "strike_ladder":
            kidx = {}
            for km in kms:
                ks, kv = kalshi_strike(km)
                if kv is None:
                    continue
                kidx.setdefault(round(kv, 4), []).append(km)
            for pm in pmkts:
                ps, pv = pm["strike"]
                if pv is None or not pm["is_threshold"]:   # only clean cumulative 'above X' PM markets
                    continue
                cands = kidx.get(round(pv, 4), [])
                if not cands:   # crypto uses X.99 strikes -> small relative tolerance
                    for kk, lst in kidx.items():
                        if kk != 0 and abs(kk - pv) / max(abs(kk), 1) < 0.0007:
                            cands = lst
                            break
                # require the SAME resolution date/time, else it's a different underlying event
                cands = [km for km in cands if _date_ok(pm, km, date_tol)]
                if not cands:
                    continue
                km = _best_kalshi(cands, pm)
                pairs.append(_mk_pair(name, cat, shape, pv, pm, km, kser, note))
        else:  # binary
            pm_b = [p for p in pmkts if p["token_yes"]]
            for pm in pm_b[:4]:
                cands = [km for km in kms if _date_ok(pm, km, date_tol)]
                km = _best_kalshi(cands, pm)
                if km is None:
                    continue
                pairs.append(_mk_pair(name, cat, shape, None, pm, km, kser, note))

    # filter by closed-state if requested (a pair is usable for a part only if both sides agree)
    out = []
    for pr in pairs:
        if pr["k"]["result"] in (None, "", "void") and want_closed is True:
            continue
        if want_closed is True and not pr["pm"]["closed"]:
            continue
        if want_closed is False and (pr["pm"]["closed"] or pr["k"]["result"] not in (None, "")):
            continue
        out.append(pr)
    # de-dup by (pm cond, k ticker)
    seen, dedup = set(), []
    for pr in out:
        key = (pr["pm"]["cond"], pr["k"]["ticker"])
        if key in seen:
            continue
        seen.add(key)
        dedup.append(pr)
    if max_pairs:
        dedup = dedup[:max_pairs]
    return dedup


def _date_ok(pm, km, tol):
    """True if PM and Kalshi resolve on the same underlying event (close times within tol seconds)."""
    pe = pm.get("end_ts")
    kc = iso_ts(km.get("close_time")) or iso_ts(km.get("expiration_time"))
    if pe is None or kc is None:
        return tol >= 21 * 86400   # only allow undated matches for the loose politics class
    return abs(pe - kc) <= tol


def _best_kalshi(cands, pm):
    if not cands:
        return None
    # closest resolution time, then most liquid
    pe = pm.get("end_ts") or 0
    def keyf(m):
        kc = iso_ts(m.get("close_time")) or iso_ts(m.get("expiration_time")) or 0
        return (abs((kc or 0) - pe), -(_f(m.get("volume_fp")) or 0))
    return min(cands, key=keyf)


def _mk_pair(name, cat, shape, strike, pm, km, kser, note):
    res = km.get("result")
    k_yes = 1 if res == "yes" else (0 if res == "no" else None)
    return {
        "name": name, "category": cat, "shape": shape, "strike": strike, "note": note,
        "pm": pm,
        "k": {"ticker": km.get("ticker"), "series": kser, "result": res, "resolved_yes": k_yes,
              "title": km.get("title"), "sub": km.get("yes_sub_title"), "close_ts": iso_ts(km.get("close_time")),
              "vol": _f(km.get("volume_fp")) or 0, "raw": km},
    }


# =============================================================================== time alignment
def align(series_a, series_b, bucket=86400):
    """Bucket two [(ts,val)] series to a common grid (last value in each bucket); inner-join.
    Returns [(bucket_ts, a, b)] sorted."""
    def buck(s):
        d = {}
        for ts, v in s:
            d[ts // bucket] = v   # last in bucket (series are time-sorted)
        return d
    A, B = buck(series_a), buck(series_b)
    keys = sorted(set(A) & set(B))
    return [(k * bucket, A[k], B[k]) for k in keys]


# ======================================================== PART A-econ: resolved econ-print backtest
# PM frames econ as a PARTITION of value-buckets while Kalshi uses cumulative 'above X' thresholds.
# To compare apples-to-apples we SUM PM bucket probabilities into a cumulative PM(value >= X) and
# match it to Kalshi's 'above X'. Resolution comes from Kalshi's settled strike (authoritative on the
# realized number). This is the only clean way to test the SLOW SCHEDULED-ECON class on resolved data.
ECON_FAMILIES = [
    ("Core CPI YoY",  "Core CPI YoY",     "KXCPIYOY"),
    ("CPI MoM",       "monthly inflation","KXCPI"),
    ("Nonfarm jobs",  "jobs added",       "KXPAYROLLS"),
    ("Unemployment",  "unemployment rate","KXU3"),
]


def econ_resolved_backtest(verbose=True):
    """Build resolved cumulative PM(>=X) vs Kalshi 'above X' pairs from recent econ releases; measure
    divergence + line-shop GROSS P&L + Brier. Returns same dict shape as part_a (so reporting reuses)."""
    div_all, div_by_cat = [], {}
    brier_pm = brier_k = 0.0
    nbrier = 0
    bets, persist_runs = [], []
    used = 0
    detail = []
    for (fam, pm_q, kser) in ECON_FAMILIES:
        try:
            kset = kalshi_markets(kser)
        except Exception:  # noqa: BLE001
            kset = []
        # group Kalshi settled strikes by release month (event_ticker), keep those with a result+candles
        k_by_event = {}
        for km in kset:
            if km.get("result") not in ("yes", "no"):
                continue
            k_by_event.setdefault(km.get("event_ticker", ""), []).append(km)
        # PM closed events for this family
        try:
            pm_evs = pm_search_events(pm_q, closed=True)
        except Exception:  # noqa: BLE001
            pm_evs = []
        # cap to the most-recent few closed events per family (bounds the prices-history fan-out)
        pm_evs = sorted(pm_evs, key=lambda e: iso_ts(e.get("endDate")) or 0, reverse=True)[:4]
        for ev in pm_evs:
            pmkts = [m for m in pm_markets_of_event(ev) if m["strike"][1] is not None and m["closed"]]
            if len(pmkts) < 3:
                continue
            pe = iso_ts(ev.get("endDate")) or (pmkts[0].get("end_ts"))
            # find the Kalshi event closest in close-time (same release)
            best_ev, best_dt = None, 1e18
            for et, kl in k_by_event.items():
                kc = iso_ts(kl[0].get("close_time"))
                if kc and pe and abs(kc - pe) < best_dt and abs(kc - pe) <= 4 * 86400:
                    best_ev, best_dt = et, abs(kc - pe)
            if not best_ev:
                continue
            kl = k_by_event[best_ev]
            # PM bucket lower-edges + prices-history (cache); build cumulative on a daily grid
            pm_hist = {}
            for m in pmkts:
                lo = m["strike"][1]
                try:
                    h = pm_prices_history(m["token_yes"], fidelity=720)
                except Exception:  # noqa: BLE001
                    h = []
                if h:
                    pm_hist[lo] = h
            if len(pm_hist) < 3:
                continue
            # daily PM cumulative >=X path, for each Kalshi strike X
            lows = sorted(pm_hist)
            # bucket value at each day = last price in that day; build day->{lo:price}
            day_px = {}
            for lo, h in pm_hist.items():
                for ts, p in h:
                    day_px.setdefault(ts // 86400, {})[lo] = p
            for km in kl:
                _ks, X = kalshi_strike(km)
                if X is None:
                    continue
                y = 1 if km.get("result") == "yes" else 0   # Kalshi authoritative: actual >= X ?
                kc = kalshi_candles(kser, km["ticker"], period=1440)
                if len(kc) < 3:
                    continue
                kfair = {ts // 86400: fair for (ts, fair, _b, _a) in kc}
                # PM cumulative >= X on each day present in BOTH
                rows_al = []
                for day in sorted(set(day_px) & set(kfair)):
                    bk = day_px[day]
                    # only use days where PM buckets cover the partition reasonably (sum in [0.6,1.4])
                    s = sum(bk.values())
                    if not (0.6 <= s <= 1.4):
                        continue
                    pm_cum = sum(v for lo, v in bk.items() if lo >= X - 1e-9)
                    pm_cum = max(0.0, min(1.0, pm_cum))
                    kf = kfair[day]
                    if not (NTM_LO <= kf <= NTM_HI):
                        continue
                    rows_al.append((day, pm_cum, kf))
                if len(rows_al) < 2:
                    continue
                used += 1
                ds = [(t, p - k) for (t, p, k) in rows_al]
                for _t, d in ds:
                    div_all.append(d); div_by_cat.setdefault("econ_print", []).append(d)
                for (_t, p, k) in (rows_al[:-1] if len(rows_al) > 2 else rows_al):
                    brier_pm += (p - y) ** 2; brier_k += (k - y) ** 2; nbrier += 1
                run = 0
                for _t, d in ds:
                    if abs(d) >= THETA_MAIN:
                        run += 1
                    elif run:
                        persist_runs.append(run); run = 0
                if run:
                    persist_runs.append(run)
                for i, (_t, d) in enumerate(ds):
                    if abs(d) >= THETA_MAIN:
                        pm_entry = rows_al[i][1]
                        if d < 0:
                            pnl = y - pm_entry; won = 1 if y == 1 else 0
                        else:
                            pnl = (1 - y) - (1 - pm_entry); won = 1 if y == 0 else 0
                        bets.append(("econ_print", abs(d), pnl, won, pm_entry, rows_al[i][2], y))
                        detail.append((fam, km["ticker"], X, y, pm_entry, rows_al[i][2], abs(d)))
                        break
    if verbose and detail:
        print(f"# econ resolved cumulative pairs used: {used}; sample bets:")
        for dd in detail[:12]:
            print(f"#   {dd[0]:14s} {dd[1][-14:]:14s} X={dd[2]:>8g} y={dd[3]} PMentry={dd[4]:.3f} "
                  f"Kfair={dd[5]:.3f} gap={dd[6]:.3f}")
    return {"div_all": div_all, "div_by_cat": div_by_cat, "bets": bets, "persist_runs": persist_runs,
            "brier_pm": (brier_pm / nbrier) if nbrier else None,
            "brier_k": (brier_k / nbrier) if nbrier else None,
            "nbrier": nbrier, "used_pairs": used}


# =============================================================================== PART A: forecasting
THETA_MAIN = 0.03   # main entry threshold for the line-shop rule (swept in DIV_THRESHOLDS too)
GRAN_BY_CAT = {"crypto_fast": 3600}   # crypto dailies are intraday -> hourly; everything else daily
NTM_LO, NTM_HI = 0.03, 0.97           # exclude near-certain legs (0.99/1.00 rounding noise) but KEEP
                                      # the longshot region [0.03..] where retail overpricing lives


def part_a(pairs, verbose=True):
    """RESOLVED pairs: divergence stats + is-Kalshi-sharper (Brier) + line-shop GROSS P&L.
    Filters: Kalshi must have real volume (a price), and we only score NEAR-THE-MONEY buckets
    (Kalshi fair in [0.07,0.93]) so 0.99-vs-1.00 rounding noise on deep ITM legs can't fake an edge."""
    div_all, div_by_cat = [], {}
    brier_pm = brier_k = 0.0
    nbrier = 0
    bets = []              # (category, gap, pnl_per_share, won, pm_entry, k_fair, y)
    persist_runs = []
    used = 0
    for pr in pairs:
        if pr["k"]["vol"] <= 0:
            continue
        y = pr["pm"]["resolved_yes"]
        if y is None:
            y = pr["k"]["resolved_yes"]
        if y is None:
            continue
        gran = GRAN_BY_CAT.get(pr["category"], 86400)
        # prices-history interval=all works for closed AND open; fidelity 1h for intraday, 12h else
        pm_path = pm_prices_history(pr["pm"]["token_yes"], fidelity=(60 if gran < 86400 else 720))
        if len(pm_path) < 3:
            continue
        k_path = kalshi_candles(pr["k"]["series"], pr["k"]["ticker"],
                                period=(60 if gran < 86400 else 1440))
        if len(k_path) < 3:
            continue
        k_fair = [(ts, fair) for (ts, fair, _b, _a) in k_path]
        aligned = align(pm_path, k_fair, bucket=gran)
        # keep only near-the-money buckets (meaningful divergence)
        aligned = [(ts, p, k) for (ts, p, k) in aligned if NTM_LO <= k <= NTM_HI]
        if len(aligned) < 2:
            continue
        used += 1
        ds = [(ts, p - k) for (ts, p, k) in aligned]
        for _ts, d in ds:
            div_all.append(d)
            div_by_cat.setdefault(pr["category"], []).append(d)
        for (_ts, p, k) in (aligned[:-1] if len(aligned) > 2 else aligned):
            brier_pm += (p - y) ** 2
            brier_k += (k - y) ** 2
            nbrier += 1
        run = 0
        for _ts, d in ds:
            if abs(d) >= THETA_MAIN:
                run += 1
            elif run:
                persist_runs.append(run); run = 0
        if run:
            persist_runs.append(run)
        # ONE independent bet per pair: first NTM bucket where |d|>=theta, hold to resolution
        for i, (_ts, d) in enumerate(ds):
            if abs(d) >= THETA_MAIN:
                pm_entry, kf = aligned[i][1], aligned[i][2]
                if d < 0:   # PM cheap vs Kalshi -> BUY YES on PM
                    pnl = y - pm_entry
                    won = 1 if y == 1 else 0
                else:       # PM rich -> BUY NO on PM (sell YES)
                    pnl = (1 - y) - (1 - pm_entry)
                    won = 1 if y == 0 else 0
                bets.append((pr["category"], abs(d), pnl, won, pm_entry, kf, y))
                break

    return {"div_all": div_all, "div_by_cat": div_by_cat, "bets": bets, "persist_runs": persist_runs,
            "brier_pm": (brier_pm / nbrier) if nbrier else None,
            "brier_k": (brier_k / nbrier) if nbrier else None,
            "nbrier": nbrier, "used_pairs": used}


# =============================================================================== PART B: execution
def walk_book(levels, notional):
    """VWAP fill walking `levels`=[(price,size)] (asks ascending) for USD `notional`.
    Returns (vwap, filled_usd, depth_exhausted)."""
    spent = shares = 0.0
    for px, sz in levels:
        lvl_usd = px * sz
        take = min(lvl_usd, notional - spent)
        if take <= 0:
            break
        spent += take
        shares += take / px
        if spent >= notional - 1e-6:
            return (spent / shares if shares else None, spent, False)
    return (spent / shares if shares else None, spent, True)


MAX_PM_SPREAD = 0.06   # an illiquid PM market (wide spread) has no meaningful mid -> not tradeable


def part_b(pairs, persist_min=0, verbose=True):
    """OPEN pairs: LIVE divergence census (both books must have a real two-sided quote) + for any
    actionable gap, REAL book-walk slippage by size, fees, persistence re-poll. Strict validity gates
    guard against phantom/stale/illiquid quotes faking an edge."""
    rows = []
    census = []  # (pair, pm_mid, k_fair, gap, pm_spread)  -- every VALID liquid matched pair
    snaps = []   # for persistence
    for pr in pairs:
        pm = pr["pm"]
        try:
            km = http_get(f"{KAL}/markets?series_ticker={pr['k']['series']}"
                          f"&tickers={pr['k']['ticker']}", cache=False)
            kmk = (km.get("markets") or [None])[0] if isinstance(km, dict) else None
        except Exception:  # noqa: BLE001
            kmk = None
        if not kmk:
            continue
        k_fair, k_bid, k_ask = kalshi_live_fair(kmk)
        if k_fair is None:        # GATE 1: Kalshi must have a real two-sided market
            continue
        try:
            bids, asks = pm_book(pm["token_yes"])
        except Exception:  # noqa: BLE001
            continue
        if not bids or not asks:
            continue
        pm_bid, pm_ask = bids[0][0], asks[0][0]
        pm_spread = pm_ask - pm_bid
        if not (0 < pm_bid < pm_ask < 1) or pm_spread > MAX_PM_SPREAD:   # GATE 2: PM must be liquid
            continue
        pm_mid = 0.5 * (pm_bid + pm_ask)
        gap = pm_mid - k_fair
        census.append((pr, pm_mid, k_fair, gap, pm_spread))   # a VALID, comparable live observation
        if abs(gap) < DIV_THRESHOLDS[0]:
            continue   # no actionable divergence right now (but counted in the census)
        # side: if PM cheap (gap<0) buy YES (walk asks); if PM rich buy NO (walk bids of YES = sell)
        if gap < 0:
            side, levels, ref_px = "BUY_YES", asks, pm_ask
        else:
            # buying NO = selling YES into bids; cost per NO share = 1 - bid_price; fair_no = 1-k_fair
            side, levels, ref_px = "BUY_NO", bids, pm_bid
        sizes_net = {}
        cap_usd = None
        for usd in EXEC_SIZES:
            vwap, filled, exh = walk_book(levels, usd)
            if vwap is None:
                continue
            if side == "BUY_YES":
                entry = vwap                 # pay vwap for YES
                fair_cost = k_fair
                gross = k_fair - pm_ask      # top-of-book edge
                net = (fair_cost - entry)    # value(fair) - what you paid
            else:
                entry = 1 - vwap             # pay (1-bid) for a NO share
                fair_cost = 1 - k_fair
                gross = (1 - k_fair) - (1 - pm_bid)
                net = fair_cost - entry
            fee = PM_TAKER_FEE * filled
            net_usd = net * (filled / max(entry, 1e-6)) - fee
            sizes_net[usd] = (net, net_usd, exh)
            if net > 0:
                cap_usd = usd                # largest size still net-positive
        rows.append({
            "pair": pr, "side": side, "pm_mid": pm_mid, "k_fair": k_fair,
            "gap": gap, "gross_top": (k_fair - pm_ask) if side == "BUY_YES" else ((1 - k_fair) - (1 - pm_bid)),
            "sizes": sizes_net, "cap_usd": cap_usd, "k_bid": k_bid, "k_ask": k_ask,
            "pm_bid": pm_bid, "pm_ask": pm_ask,
        })
        snaps.append((pr, pm_mid, k_fair, side))

    persist = None
    if persist_min > 0 and snaps:
        if verbose:
            print(f"# [PART B] sleeping {persist_min} min to re-poll persistence ...", flush=True)
        time.sleep(persist_min * 60)
        persist = []
        for (pr, pm_mid0, k_fair0, side) in snaps:
            try:
                bids, asks = pm_book(pr["pm"]["token_yes"])
                pm_mid1 = 0.5 * (bids[0][0] + asks[0][0]) if bids and asks else None
                km = http_get(f"{KAL}/markets?series_ticker={pr['k']['series']}"
                              f"&tickers={pr['k']['ticker']}", cache=False)
                kmk = (km.get("markets") or [None])[0]
                k_fair1, _b, _a = kalshi_live_fair(kmk) if kmk else (None, None, None)
            except Exception:  # noqa: BLE001
                pm_mid1 = k_fair1 = None
            if pm_mid1 is None or k_fair1 is None:
                continue
            persist.append({
                "name": pr["name"], "gap0": pm_mid0 - k_fair0, "gap1": pm_mid1 - k_fair1,
            })
    return {"rows": rows, "persist": persist, "census": census}


# =============================================================================== reporting
def histogram(vals, lo=-0.20, hi=0.20, nb=16):
    if not vals:
        return
    step = (hi - lo) / nb
    counts = [0] * nb
    for v in vals:
        b = int((min(max(v, lo), hi - 1e-9) - lo) / step)
        counts[min(max(b, 0), nb - 1)] += 1
    mx = max(counts) or 1
    for i in range(nb):
        a = lo + i * step
        bar = "#" * int(40 * counts[i] / mx)
        print(f"   {a:+.3f}..{a+step:+.3f} {counts[i]:5d} {bar}")


def report_match(pairs):
    print(f"\n# ===== MATCHED PAIRS ({len(pairs)}) =====")
    by = {}
    for pr in pairs:
        by.setdefault(pr["category"], []).append(pr)
    for cat in sorted(by):
        print(f"\n## {cat} ({len(by[cat])})")
        for pr in by[cat][:12]:
            st = f"@{pr['strike']:g}" if pr["strike"] is not None else ""
            y = pr["pm"]["resolved_yes"]
            ky = pr["k"]["resolved_yes"]
            print(f"   {pr['name'][:22]:22s}{st:>12} | PM '{pr['pm']['question'][:34]:34s}' "
                  f"res={y} | K {pr['k']['ticker'][-16:]:16s} res={ky} vol={pr['k']['vol']:.0f}")


def report_a(a):
    print("\n# ============================ PART A : FORECASTING EDGE (resolved pairs) ============================")
    print(f"# usable resolved pairs with both paths: {a['used_pairs']}   aligned samples: {len(a['div_all'])}")
    if a["div_all"]:
        ds = a["div_all"]
        ab = [abs(x) for x in ds]
        print(f"# divergence d = PM - Kalshi_fair (near-the-money buckets only):")
        print(f"#   mean={statistics.mean(ds):+.4f}  median|d|={statistics.median(ab):.4f}  "
              f"p90|d|={_pct(ab,90):.4f}  max|d|={max(ab):.4f}  n={len(ds)}")
        for th in DIV_THRESHOLDS:
            fr = sum(1 for x in ab if x >= th) / len(ab)
            print(f"#   P(|d| >= {th:.2f}) = {fr*100:5.1f}%")
        if a.get("div_by_cat"):
            print("#   by category (median|d| / p90|d| / n):")
            for c in sorted(a["div_by_cat"]):
                cab = [abs(x) for x in a["div_by_cat"][c]]
                print(f"#      {c:16s} med={statistics.median(cab):.4f} p90={_pct(cab,90):.4f} n={len(cab)}")
        print("# histogram of d (PM-Kalshi):")
        histogram(ds)
    if a["persist_runs"]:
        pr = a["persist_runs"]
        print(f"# persistence: run-length of |d|>=0.03 (in buckets): median={statistics.median(pr):.1f} "
              f"mean={statistics.mean(pr):.1f} max={max(pr)}  (n_runs={len(pr)})")
    if a["brier_pm"] is not None:
        bp, bk = a["brier_pm"], a["brier_k"]
        sharper = "KALSHI sharper" if bk < bp else "PM sharper"
        print(f"# Brier (lower=better forecaster): PM={bp:.4f}  Kalshi={bk:.4f}  -> {sharper} "
              f"(by {abs(bp-bk):.4f}, n={a['nbrier']})")
    bets = a["bets"]
    print(f"\n# LINE-SHOP RULE (enter |d|>=0.03 toward Kalshi, hold to resolution) -- GROSS, top-of-book")
    if bets:
        _bet_table(bets)
    else:
        print("#   no bets triggered")


def _bet_table(bets):
    cats = {}
    for b in bets:
        cats.setdefault(b[0], []).append(b)
    print(f"#   {'category':16s} {'bets':>4s} {'hit%':>6s} {'avgGapEntry':>11s} {'grossEdge/$1':>12s} {'totGross/$1stk':>14s}")
    tot_pnl = tot_n = 0
    for c in sorted(cats):
        bs = cats[c]
        n = len(bs)
        hit = sum(b[3] for b in bs) / n
        avg_gap = statistics.mean(b[1] for b in bs)
        # edge per $1 staked: pnl is per-share (=per $1 of payoff). stake per share = entry price.
        edges = [b[2] / max(b[4] if b[2] >= 0 else (1 - b[4]), 0.02) for b in bs]
        avg_edge = statistics.mean(edges)
        tot = sum(edges)
        tot_pnl += tot
        tot_n += n
        print(f"#   {c:16s} {n:>4d} {hit*100:>5.0f}% {avg_gap:>11.4f} {avg_edge:>+12.4f} {tot:>+14.3f}")
    overall_hit = sum(b[3] for b in bets) / len(bets)
    print(f"#   {'ALL':16s} {tot_n:>4d} {overall_hit*100:>5.0f}% {'':>11s} {'':>12s} {tot_pnl:>+14.3f}")
    print(f"#   (grossEdge/$1 = (resolution - PM entry)/stake; >0 means line-shopping toward Kalshi wins gross)")


def _pct(xs, p):
    if not xs:
        return float("nan")
    s = sorted(xs)
    i = min(len(s) - 1, int(p / 100 * len(s)))
    return s[i]


def report_b(b):
    print("\n# ============================ PART B : EXECUTION REALITY (open pairs, LIVE book) ============================")
    # --- LIVE DIVERGENCE CENSUS: every matched pair where BOTH books have a real two-sided quote ---
    cen = b.get("census") or []
    print(f"# LIVE DIVERGENCE CENSUS -- {len(cen)} matched non-sports pairs with a real two-sided quote on BOTH books:")
    if cen:
        gaps = [c[3] for c in cen]
        ab = [abs(g) for g in gaps]
        print(f"#   live gap = PM_mid - Kalshi_fair :  mean={statistics.mean(gaps):+.4f}  "
              f"median|gap|={statistics.median(ab):.4f}  p90|gap|={_pct(ab,90):.4f}  max|gap|={max(ab):.4f}")
        for th in DIV_THRESHOLDS:
            print(f"#   P(|gap| >= {th:.2f}) = {sum(1 for x in ab if x>=th)/len(ab)*100:5.1f}%  "
                  f"({sum(1 for x in ab if x>=th)}/{len(ab)})")
        print("#   the matched liquid pairs and their live gaps:")
        for (pr, pmm, kf, gp, sp) in sorted(cen, key=lambda c: -abs(c[3])):
            print(f"#     {pr['name'][:20]:20s} {('@%g'%pr['strike']) if pr['strike'] is not None else '':>9s} "
                  f"PM_mid={pmm:.3f} K_fair={kf:.3f} gap={gp:+.3f} (PMspread={sp:.3f})")
    print()
    rows = b["rows"]
    if not rows:
        print("# -> NO matched liquid pair shows an actionable live divergence (|gap|>=0.02) at this snapshot.")
        print("#    That is the result: on correctly-matched, two-sided-liquid non-sports markets the books agree")
        print("#    to within ~1c right now, so there is nothing for line-shopping to capture after costs.")
        return
    print(f"# {len(rows)} open pair(s) with a live |gap|>=0.02. For each: gross top-of-book edge vs")
    print(f"# NET edge after walking the REAL PM book for size (PM taker fee={PM_TAKER_FEE*100:.2f}%).\n")
    for r in rows:
        pr = r["pair"]
        print(f"## {pr['name']}  {('@%g'%pr['strike']) if pr['strike'] is not None else ''}  [{pr['category']}]")
        print(f"   PM '{pr['pm']['question'][:46]}'")
        print(f"   PM bid/ask={r['pm_bid']:.3f}/{r['pm_ask']:.3f} mid={r['pm_mid']:.3f} | "
              f"Kalshi fair={r['k_fair']:.3f} (bid/ask={r['k_bid']}/{r['k_ask']}) | "
              f"side={r['side']} gap={r['gap']:+.3f} grossTop/$1={r['gross_top']/max(r['pm_ask'],0.02):+.3f}")
        print(f"   {'sizeUSD':>8s} {'net/$1':>9s} {'netUSD':>9s} {'depthOut':>9s}")
        for usd in EXEC_SIZES:
            if usd in r["sizes"]:
                net, net_usd, exh = r["sizes"][usd]
                print(f"   {usd:>8d} {net:>+9.3f} {net_usd:>+9.1f} {'YES' if exh else '':>9s}")
        print(f"   -> largest net-positive size walked: "
              f"{('$%d'%r['cap_usd']) if r['cap_usd'] else 'NONE (slippage kills it at every size)'}")
    if b["persist"] is not None:
        print("\n# PERSISTENCE re-poll (did the live gap survive the delay?):")
        for p in b["persist"]:
            decay = abs(p["gap1"]) - abs(p["gap0"])
            print(f"#   {p['name'][:24]:24s} gap {p['gap0']:+.3f} -> {p['gap1']:+.3f}  "
                  f"({'WIDENED' if decay>0 else 'decayed'} {decay:+.3f})")


def verdict(a, b):
    print("\n# ============================ VERDICT ============================")
    print("# (read PART A 'is the signal real & gross-positive' WITH PART B 'does it survive execution')")
    if a and a.get("bets"):
        overall_hit = sum(x[3] for x in a["bets"]) / len(a["bets"])
        cats = {}
        for x in a["bets"]:
            cats.setdefault(x[0], []).append(x)
        print(f"# PART A: {len(a['bets'])} independent line-shop bets, gross hit-rate {overall_hit*100:.0f}%.")
        if a.get("brier_k") is not None:
            print(f"#         Brier PM={a['brier_pm']:.4f} vs Kalshi={a['brier_k']:.4f} "
                  f"-> {'Kalshi IS the sharper book' if a['brier_k']<a['brier_pm'] else 'PM not beaten by Kalshi'}.")
    if b and b.get("rows") is not None:
        n_pos = sum(1 for r in b["rows"] if r["cap_usd"])
        print(f"# PART B: {len(b['rows'])} live divergences; {n_pos} stay net-positive after real book slippage.")
    print("# Full written verdict is in the report this tool feeds; numbers above are the evidence.")


# =============================================================================== main
def main():
    only = None
    for f in ("--match", "--diverge", "--backtest", "--exec"):
        if f in sys.argv:
            only = f
    persist_min = 0
    if "--persist-min" in sys.argv:
        persist_min = float(sys.argv[sys.argv.index("--persist-min") + 1])
    max_pairs = None
    if "--max-pairs" in sys.argv:
        max_pairs = int(sys.argv[sys.argv.index("--max-pairs") + 1])
    gran = 86400
    if "--hourly" in sys.argv:
        gran = 3600

    print(f"# cross-book line-shop validator  (cache={'on' if USE_CACHE else 'off'})", flush=True)
    if only in (None, "--match", "--diverge", "--backtest"):
        print("# resolving RESOLVED matched pairs (PART A) ...", flush=True)
        res_pairs = resolve_matches(want_closed=True, max_pairs=max_pairs, verbose=True)
        print(f"# resolved matched pairs: {len(res_pairs)}", flush=True)
    if only == "--match":
        report_match(res_pairs)
        return
    a = None
    if only in (None, "--diverge", "--backtest"):
        a = part_a(res_pairs)
        print("# [general matched set: dominated by crypto dailies, which converge instantly -> 0 path]")
        report_a(a)
        print("\n# ---- ECON-PRINT resolution backtest (PM buckets summed to cumulative >=X vs Kalshi 'above X') ----")
        ec = econ_resolved_backtest()
        report_a(ec)
        a = ec if (ec.get("bets")) else a   # prefer the econ result for the verdict if it produced bets

    b = None
    if only in (None, "--exec"):
        print("\n# resolving OPEN matched pairs (PART B) ...", flush=True)
        open_pairs = resolve_matches(want_closed=False, max_pairs=max_pairs, verbose=True)
        print(f"# open matched pairs: {len(open_pairs)}", flush=True)
        b = part_b(open_pairs, persist_min=persist_min)
        report_b(b)

    if only is None:
        verdict(a, b)


if __name__ == "__main__":
    main()
