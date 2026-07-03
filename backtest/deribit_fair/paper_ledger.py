#!/usr/bin/env python3
"""Paper ledger for the crypto tail-overpricing edge (ZERO real money).

Confirmation protocol (2026-07-03 finding: PM overprices far-tail (<=7c) crypto touch/terminal
outcomes 2.5-15x vs Deribit options-implied fair):
  each run "sells" (paper) every mapped market with fair_hi < FAIR_MAX and bid > fair_hi + MIN_EDGE
  at the standing bid, sized to visible bid depth (capped), then marks all open paper positions:
    - touch markets: hit if Binance daily High >= K (or Low <= K for dips) since entry
    - terminal/other: market closed => record PM outcomePrices
    - bid persistence: current bid vs entry bid (capacity reality check)
  Cadence: weekly suffices for the Dec ladder; idempotent (one entry per market id).
  8-12 weeks of cluster-independent observations confirm/refute the ~5x overpricing.

State: ~/pm-data/tail_ledger.jsonl. Inputs: a fresh discrepancies.json from running, in order,
  census.py -> deribit_pull.py -> map_markets.py in this directory (or pass a path as argv[1]).
Schema of discrepancies.json rows: id, question, slug, coin, kind (terminal_lt/terminal_gt/
  touch_up/touch_down/...), K, T, fair/fair_lo/fair_hi, bid/ask/mid, depth_bid/depth_ask, endDate.
"""
import json, os, sys, time, urllib.request

LEDGER = os.path.expanduser("~/pm-data/tail_ledger.jsonl")
FAIR_MAX = 0.03          # only sell where options-implied fair_hi < 3%
MIN_EDGE = 0.005         # and bid exceeds fair_hi by >= 0.5c
SIZE_CAP_USD = 200.0     # paper size cap per market (bid-depth limited below this)
UA = {"User-Agent": "Mozilla/5.0 (paper-ledger; read-only)", "Accept": "application/json"}

def get(u, tries=3):
    for i in range(tries):
        try:
            with urllib.request.urlopen(urllib.request.Request(u, headers=UA), timeout=25) as r:
                return json.loads(r.read().decode())
        except Exception:
            if i == tries - 1: return None
            time.sleep(0.5 * (i + 1))

def load_entries():
    entries = {}
    if os.path.exists(LEDGER):
        for line in open(LEDGER):
            try: j = json.loads(line)
            except Exception: continue
            if j.get("kind") == "entry":
                entries[j["mid"]] = j          # mid = gamma market id
    return entries

def append(obj):
    os.makedirs(os.path.dirname(LEDGER), exist_ok=True)
    with open(LEDGER, "a") as f:
        f.write(json.dumps(obj) + "\n")

def binance_extreme(symbol, start_ms, hi=True):
    out = None; cur = start_ms
    while True:
        d = get(f"https://data-api.binance.vision/api/v3/klines?symbol={symbol}&interval=1d&startTime={cur}&limit=1000")
        if not d: break
        for k in d:
            v = float(k[2]) if hi else float(k[3])
            out = v if out is None else (max(out, v) if hi else min(out, v))
        if len(d) < 1000: break
        cur = d[-1][6] + 1
    return out

def main():
    disc_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "discrepancies.json")
    now = time.time()
    entries = load_entries()
    new = marked = 0
    if os.path.exists(disc_path):
        for r in json.load(open(disc_path)):
            mid_id = r.get("id"); fair_hi = r.get("fair_hi"); bid = r.get("bid")
            if not mid_id or fair_hi is None or bid is None or mid_id in entries: continue
            if fair_hi < FAIR_MAX and (bid - fair_hi) >= MIN_EDGE:
                e = {"kind": "entry", "ts": now, "mid": mid_id, "slug": r.get("slug"),
                     "bid": bid, "fair_hi": fair_hi, "depth_usd": r.get("depth_bid"),
                     "size_usd": min(SIZE_CAP_USD, r.get("depth_bid") or SIZE_CAP_USD),
                     "mkind": r.get("kind"), "coin": r.get("coin"), "K": r.get("K"),
                     "end": r.get("endDate"), "question": r.get("question")}
                append(e); entries[mid_id] = e; new += 1
    else:
        print(f"note: {disc_path} not found — run census/deribit_pull/map_markets first", file=sys.stderr)
    for mid_id, e in entries.items():
        m = get(f"https://gamma-api.polymarket.com/markets/{mid_id}")
        if not m: continue
        touched = None
        if e.get("K") and e.get("coin") in ("BTC", "ETH") and str(e.get("mkind", "")).startswith("touch"):
            up = e["mkind"] == "touch_up"
            ext = binance_extreme({"BTC": "BTCUSDT", "ETH": "ETHUSDT"}[e["coin"]], int(e["ts"] * 1000), hi=up)
            if ext is not None:
                touched = ext >= float(e["K"]) if up else ext <= float(e["K"])
        append({"kind": "mark", "ts": now, "mid": mid_id, "closed": m.get("closed"),
                "outcome_prices": m.get("outcomePrices"), "bid_now": m.get("bestBid"),
                "touched": touched, "entry_bid": e["bid"], "fair_hi": e["fair_hi"]})
        marked += 1
        time.sleep(0.15)
    print(f"paper ledger: {new} new entries, {marked} marks, state={LEDGER}")

if __name__ == "__main__":
    main()
