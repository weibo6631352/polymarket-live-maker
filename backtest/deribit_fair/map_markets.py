#!/usr/bin/env python3
"""Map open PM BTC/ETH date/level markets -> Deribit fair value; pull CLOB books;
emit discrepancy table (discrepancies.json)."""
import json, math, os, re, time, sys
from datetime import datetime, timezone, timedelta
import requests
from fair import Chain

HERE = os.path.dirname(os.path.abspath(__file__))
CLOB = "https://clob.polymarket.com"
S = requests.Session()
now = time.time()
ch = Chain()

rows = json.load(open(os.path.join(HERE, "census_open.json")))

def parse_money(s):
    s = s.replace(",", "").replace("$", "").strip()
    m = re.match(r"([\d.]+)k$", s, re.I)
    if m:
        return float(m.group(1)) * 1000
    return float(s)

MONEY = r"\$[\d,]+(?:\.\d+)?k?"
ET_OFF = 4 * 3600  # EDT (all target dates are summer 2026)

def et_ts(y, mo, d, h=0, mi=0):
    return datetime(y, mo, d, h, mi, tzinfo=timezone.utc).timestamp() + ET_OFF

def parse_end(iso):
    return datetime.fromisoformat(iso.replace("Z", "+00:00")).timestamp()

MONTHS = {m: i + 1 for i, m in enumerate(
    ["january","february","march","april","may","june","july","august",
     "september","october","november","december"])}

mapped, skipped = [], {}
for r in rows:
    q = r["question"] or ""
    es = r["event_slug"]
    ql = q.lower()
    coin = "BTC" if re.search(r"\bbitcoin\b", ql) else ("ETH" if re.search(r"\bethereum\b", ql) else None)
    if not coin:
        skipped["non-btc-eth"] = skipped.get("non-btc-eth", 0) + 1
        continue
    if "am-et" in r["slug"] or "pm-et" in r["slug"] or re.search(r"\d\s*(AM|PM) ET", q):
        skipped["hourly"] = skipped.get("hourly", 0) + 1
        continue
    if not r.get("acceptingOrders"):
        skipped["not-accepting"] = skipped.get("not-accepting", 0) + 1
        continue
    endts = parse_end(r["endDate"])
    if endts < now:
        skipped["expired"] = skipped.get("expired", 0) + 1
        continue

    kind = None; K = K2 = None; t0 = None; T = endts
    try:
        if re.search(r"-above-on-", es):
            m = re.search(rf"above ({MONEY})", q)
            kind, K = "terminal_gt", parse_money(m.group(1))
        elif "-price-on-" in es:
            if "less than" in ql:
                m = re.search(rf"less than ({MONEY})", q)
                kind, K = "terminal_lt", parse_money(m.group(1))
            elif "greater than" in ql:
                m = re.search(rf"greater than ({MONEY})", q)
                kind, K = "terminal_gt", parse_money(m.group(1))
            elif "between" in ql:
                m = re.search(rf"between ({MONEY}) and ({MONEY})", q)
                kind, K, K2 = "terminal_range", parse_money(m.group(1)), parse_money(m.group(2))
        elif re.search(r"hit-on-|hit-in-|hit-june-|hit-before-|when-will|all.time.high", es):
            # touch family; direction
            up = ("dip" not in ql)
            if "all time high" in ql:
                kind = "touch_up"
                K = None  # fill later with historical ATH
            else:
                m = re.search(rf"(?:reach|dip to|hit) ({MONEY})", q)
                if not m:
                    raise ValueError("no strike")
                K = parse_money(m.group(1))
                kind = "touch_up" if up else "touch_dn"
            # window start
            if "hit-in-july" in es:
                t0 = et_ts(2026, 7, 1)
            elif "hit-june-" in es:
                mm = re.search(r"June (\d+)", q)
                t0 = et_ts(2026, 6, int(mm.group(1)))
            elif "hit-on-" in es:
                mm = re.search(r"on (July|June) (\d+)", q)
                t0 = et_ts(2026, MONTHS[mm.group(1).lower()], int(mm.group(2)))
            else:
                t0 = None  # window long open (before-2027, when-will, ATH)
        if not kind:
            skipped["unparsed"] = skipped.get("unparsed", 0) + 1
            continue
    except Exception as e:
        skipped["parse-error"] = skipped.get("parse-error", 0) + 1
        continue
    mapped.append(dict(r, coin=coin, kind=kind, K=K, K2=K2, t0=t0, T=endts))

print("skipped:", skipped, file=sys.stderr)
print("mapped:", len(mapped), file=sys.stderr)

# ---- historical ATH for ATH markets (Binance monthly klines full history) ----
ath = {}
for coin, sym in (("BTC", "BTCUSDT"), ("ETH", "ETHUSDT")):
    kl = S.get("https://data-api.binance.vision/api/v3/klines",
               params=dict(symbol=sym, interval="1M", limit=200), timeout=30).json()
    ath[coin] = max(float(k[2]) for k in kl)
print("ATH:", ath, file=sys.stderr)

# ---- compute fair values ----
for m in mapped:
    coin = m["coin"]
    K = m["K"] if m["K"] is not None else ath[coin]
    m["K"] = K
    if m["kind"] == "terminal_gt":
        p, dbg = ch.digital(coin, K, m["T"], now)
    elif m["kind"] == "terminal_lt":
        pg, dbg = ch.digital(coin, K, m["T"], now)
        p = 1 - pg
    elif m["kind"] == "terminal_range":
        p1, dbg = ch.digital(coin, K, m["T"], now)
        p2, _ = ch.digital(coin, m["K2"], m["T"], now)
        p = max(0.0, p1 - p2)
    elif m["kind"] in ("touch_up", "touch_dn"):
        up = m["kind"] == "touch_up"
        # touch windows end 11:59PM ET on last day; endDate is usually 04:00Z next day = ok
        variants = {}
        for vr in ("geo", "atm", "barrier"):
            if m["t0"] and m["t0"] > now:
                pv, dbg = ch.touch_window(coin, K, m["t0"], m["T"], now, up=up, vol_ref=vr)
            else:
                pv, dbg = ch.touch(coin, K, m["T"], now, up=up, vol_ref=vr)
            variants[vr] = pv
        p = variants["geo"]
        m["fair_lo"] = round(min(variants.values()), 4)
        m["fair_hi"] = round(max(variants.values()), 4)
    m["fair"] = p
    if "fair_lo" not in m:
        m["fair_lo"] = m["fair_hi"] = round(p, 4)
    m["dbg"] = {k: v for k, v in dbg.items() if isinstance(v, (int, float))}

# ---- pull CLOB books (batch) ----
tok_of = {}
for m in mapped:
    try:
        toks = json.loads(m["clobTokenIds"]) if isinstance(m["clobTokenIds"], str) else m["clobTokenIds"]
        tok_of[m["id"]] = toks[0]  # YES token
    except Exception:
        tok_of[m["id"]] = None

books = {}
ids = [t for t in tok_of.values() if t]
for i in range(0, len(ids), 50):
    chunk = ids[i:i + 50]
    r = S.post(f"{CLOB}/books", json=[{"token_id": t} for t in chunk], timeout=30)
    if r.status_code == 200:
        for b in r.json():
            books[b.get("asset_id")] = b
    else:
        print("books batch fail", r.status_code, file=sys.stderr)
    time.sleep(0.4)
print("books:", len(books), file=sys.stderr)

def best(book):
    bids = [(float(x["price"]), float(x["size"])) for x in (book.get("bids") or [])]
    asks = [(float(x["price"]), float(x["size"])) for x in (book.get("asks") or [])]
    bb = max(bids)[0] if bids else None
    ba = min(asks)[0] if asks else None
    # depth within 5c of best on each side (USDC notional)
    dep_b = sum(p * s for p, s in bids if bb and p >= bb - 0.05)
    dep_a = sum(p * s for p, s in asks if ba and p <= ba + 0.05)
    return bb, ba, dep_b, dep_a

out = []
for m in mapped:
    t = tok_of[m["id"]]
    b = books.get(t)
    bb = ba = dep_b = dep_a = None
    if b:
        bb, ba, dep_b, dep_a = best(b)
    mid = None
    if bb is not None and ba is not None:
        mid = (bb + ba) / 2
    edge = (m["fair"] - mid) if mid is not None else None
    out.append(dict(
        id=m["id"], question=m["question"], event=m["event_slug"], coin=m["coin"],
        kind=m["kind"], K=m["K"], K2=m["K2"], T=m["T"], t0=m["t0"],
        fair=round(m["fair"], 4), fair_lo=m["fair_lo"], fair_hi=m["fair_hi"],
        bid=bb, ask=ba, mid=mid,
        edge=None if edge is None else round(edge, 4),
        depth_bid=dep_b and round(dep_b, 0), depth_ask=dep_a and round(dep_a, 0),
        spread=(None if (bb is None or ba is None) else round(ba - bb, 3)),
        vol=m["volume"], vol24=m["volume24hr"], liq=m["liquidity"],
        rewardsMaxSpread=m["rewardsMaxSpread"], rewardsMinSize=m["rewardsMinSize"],
        feeType=m["feeType"], dbg=m["dbg"], endDate=m["endDate"], slug=m["slug"],
    ))
with open(os.path.join(HERE, "discrepancies.json"), "w") as f:
    json.dump(out, f, indent=1)
print(f"wrote {len(out)} rows -> discrepancies.json")
