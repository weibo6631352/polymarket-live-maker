#!/usr/bin/env python3
"""Generate a tail-seller order sheet (orders.json) from the calibrated spec. READ-ONLY.

Spec (2026-07-03 calibration, 32,570 resolved markets): sell UP-tails only — YES priced 2-7c,
strike-daily/weekly + negrisk families (5.3x / 1.9x overpriced), enter T-5..T-3 (T-1 still 2.7x),
coins BTC/ETH/SOL/XRP (SOL 10.8x / XRP 6.4x). Down-tails and touch families are fairly priced — skip.

Mechanics: selling YES at p == resting BUY NO at 1-p. We undercut the best YES ask by one tick
(so lottery buyers lift us first) but never sell YES below FLOOR_YES (margin vs fair).

Usage: python3 make_trial_sheet.py [--max N] [--out orders.json]
Prints the candidate table; writes the sheet for tail-seller (dry-validates there anyway).
"""
import argparse, json, time, urllib.request

UA = {"User-Agent": "Mozilla/5.0 (trial-sheet; read-only)", "Accept": "application/json"}
YES_MIN, YES_MAX = 0.02, 0.07     # up-tail YES band
FLOOR_YES = 0.02                  # never sell YES below this
DAYS_MIN, DAYS_MAX = 1.0, 6.0     # time to resolution window (T-5..T-1)
SIZE = 10                         # shares per order (trial)
COINS = ("bitcoin", "ethereum", "solana", "xrp")
UP_WORDS = ("above", "greater", "reach", "or-higher", "up")   # up-tail direction markers
DOWN_WORDS = ("below", "less", "dip", "or-lower", "down")

def get(u, tries=3):
    for i in range(tries):
        try:
            with urllib.request.urlopen(urllib.request.Request(u, headers=UA), timeout=25) as r:
                return json.loads(r.read().decode())
        except Exception:
            if i == tries - 1: return None
            time.sleep(0.5 * (i + 1))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max", type=int, default=5)
    ap.add_argument("--out", default="orders.json")
    args = ap.parse_args()
    now = time.time()
    cands = []
    offset = 0
    while offset < 3000:
        d = get(f"https://gamma-api.polymarket.com/markets?closed=false&limit=100&offset={offset}"
                f"&order=endDate&ascending=true&end_date_min={time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(now + DAYS_MIN*86400))}"
                f"&end_date_max={time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(now + DAYS_MAX*86400))}")
        if not d: break
        for m in d:
            slug = (m.get("slug") or "").lower()
            q = (m.get("question") or "").lower()
            if not any(c in slug or c in q for c in COINS): continue
            if "up-or-down" in slug: continue                       # micro windows — dead class
            if any(w in slug or w in q for w in DOWN_WORDS): continue
            if not any(w in slug or w in q for w in UP_WORDS): continue
            if "hit" in slug and "on" not in slug: pass             # monthly touch — skip below
            fam_touch = ("hit" in slug or "reach" in slug or "dip" in slug) and "on-" not in slug
            if fam_touch: continue                                  # touch families fairly priced — skip
            try:
                bb = float(m.get("bestBid") or 0); ba = float(m.get("bestAsk") or 1)
                toks = json.loads(m.get("clobTokenIds") or "[]")
            except Exception:
                continue
            if len(toks) < 2: continue
            yes_mid = (bb + ba) / 2 if (bb > 0 and ba < 1) else None
            if yes_mid is None or not (YES_MIN <= yes_mid <= YES_MAX): continue
            if ba - bb > 0.05: continue                             # needs a real book
            sell_yes_at = max(round(ba - 0.001, 3), FLOOR_YES)      # undercut best ask by a tick
            if sell_yes_at <= bb: continue                          # never cross into taker
            coin = next(c for c in COINS if c in slug or c in q)
            cands.append({
                "token_id": toks[1],                                 # NO token (verify outcomes order!)
                "price": round(1 - sell_yes_at, 3),                 # BUY NO price
                "size": SIZE,
                "note": m.get("slug"),
                "_yes_bid": bb, "_yes_ask": ba, "_sell_yes_at": sell_yes_at,
                "_coin": coin, "_end": m.get("endDate"),
                "_outcomes": m.get("outcomes"), "_v24": m.get("volume24hr"),
            })
        if len(d) < 100: break
        offset += 100
        time.sleep(0.12)
    # prefer fattest calibration cells: SOL/XRP first, then ETH/BTC; one per coin first, then round-robin
    order = {"solana": 0, "xrp": 1, "ethereum": 2, "bitcoin": 3}
    cands.sort(key=lambda c: (order.get(c["_coin"], 9), -(c["_yes_ask"] - c["_yes_bid"])))
    picked, seen_coin = [], set()
    for c in cands:                                                  # first pass: one per coin
        if c["_coin"] in seen_coin: continue
        picked.append(c); seen_coin.add(c["_coin"])
        if len(picked) >= args.max: break
    for c in cands:                                                  # fill remaining slots
        if len(picked) >= args.max: break
        if c not in picked: picked.append(c)

    print(f"{'coin':10s} {'sell YES @':>10s} {'yes bid/ask':>12s} {'end':>22s} slug")
    for c in picked:
        print(f"{c['_coin']:10s} {c['_sell_yes_at']:>10.3f} {c['_yes_bid']:.3f}/{c['_yes_ask']:.3f} "
              f"{(c['_end'] or ''):>22s} {c['note']}")
        print(f"           NO outcomes={c['_outcomes']} v24={c['_v24']}")
    sheet = [{k: v for k, v in c.items() if not k.startswith("_")} for c in picked]
    with open(args.out, "w") as f:
        json.dump(sheet, f, indent=1)
    tot = sum(o["price"] * o["size"] for o in sheet)
    print(f"\nwrote {len(sheet)} orders -> {args.out}  (collateral ${tot:.2f})")
    print("!! VERIFY token order: token_id must be the NO token — check _outcomes above (['Yes','No'] -> toks[1]=No).")

if __name__ == "__main__":
    main()
