#!/usr/bin/env python3
"""Resolved-cohort puller: constructs event slugs per calendar date (May 1 - Jul 1 2026)
for the slow crypto families, fetches resolved markets + CLOB price history at
T-7d/T-3d/T-1d. Cohort selection is BY END DATE (deterministic slugs), not by
resolution order, to avoid the censored-stratum trap."""
import json, os, re, sys, time
from datetime import date, datetime, timedelta, timezone
import requests

HERE = os.path.dirname(os.path.abspath(__file__))
GAMMA = "https://gamma-api.polymarket.com"
CLOB = "https://clob.polymarket.com"
S = requests.Session()
S.headers["User-Agent"] = "research/0.1"

EV_CACHE = os.path.join(HERE, "bt_events.json")
ROWS_OUT = os.path.join(HERE, "bt_rows.jsonl")

COINS = ["bitcoin", "ethereum"]
D0, D1 = date(2026, 5, 1), date(2026, 7, 1)

def gen_slugs():
    slugs = []
    d = D0
    while d <= D1:
        mon = d.strftime("%B").lower()
        for c in COINS:
            slugs.append(f"{c}-above-on-{mon}-{d.day}-2026")
            slugs.append(f"what-price-will-{c}-hit-on-{mon}-{d.day}")
        # weekly events end on Sundays (window Mon-Sun, ends d)
        if d.weekday() == 6:
            start = d - timedelta(days=6)
            smon = start.strftime("%B").lower()
            for c in COINS:
                if smon == mon:
                    slugs.append(f"what-price-will-{c}-hit-{mon}-{start.day}-{d.day}-2026")
                else:
                    slugs.append(f"what-price-will-{c}-hit-{smon}-{start.day}-{mon}-{d.day}-2026")
        d += timedelta(days=1)
    for c in COINS:
        slugs.append(f"what-price-will-{c}-hit-in-may-2026")
        slugs.append(f"what-price-will-{c}-hit-in-june-2026")
    return slugs

def fetch_events():
    if os.path.exists(EV_CACHE):
        return json.load(open(EV_CACHE))
    out = {}
    slugs = gen_slugs()
    print(f"{len(slugs)} candidate slugs", file=sys.stderr)
    for i, sl in enumerate(slugs):
        try:
            r = S.get(f"{GAMMA}/events", params={"slug": sl}, timeout=30)
            evs = r.json() if r.status_code == 200 else []
        except Exception:
            evs = []
        out[sl] = evs[0] if evs else None
        if i % 25 == 0:
            print(f"{i}/{len(slugs)} hits={sum(1 for v in out.values() if v)}", file=sys.stderr)
        time.sleep(0.18)
    json.dump(out, open(EV_CACHE, "w"))
    return out

MONEY = r"\$[\d,]+(?:\.\d+)?k?"
def parse_money(s):
    s = s.replace(",", "").replace("$", "").strip()
    m = re.match(r"([\d.]+)k$", s, re.I)
    return float(m.group(1)) * 1000 if m else float(s)

def parse_end(iso):
    return datetime.fromisoformat(iso.replace("Z", "+00:00")).timestamp()

def market_rows(events):
    rows = []
    for sl, ev in events.items():
        if not ev:
            continue
        coin = "BTC" if "bitcoin" in sl else "ETH"
        for m in ev.get("markets", []):
            q = m.get("question") or ""
            ql = q.lower()
            op = m.get("outcomePrices")
            try:
                op = json.loads(op) if isinstance(op, str) else op
                y = float(op[0])
            except Exception:
                continue
            if y not in (0.0, 1.0):
                continue  # unresolved / weird
            if "above-on" in sl:
                mm = re.search(rf"above ({MONEY})", q)
                if not mm:
                    continue
                kind, K = "terminal_gt", parse_money(mm.group(1))
            else:
                mm = re.search(rf"(?:reach|dip to|hit) ({MONEY})", q)
                if not mm:
                    continue
                kind = "touch_dn" if "dip" in ql else "touch_up"
                K = parse_money(mm.group(1))
            try:
                tok = json.loads(m["clobTokenIds"])[0]
            except Exception:
                continue
            fam = ("daily_terminal" if "above-on" in sl else
                   "monthly_touch" if "-hit-in-" in sl else
                   "daily_touch" if "-hit-on-" in sl else "weekly_touch")
            rows.append(dict(slug=sl, mslug=m.get("slug"), q=q, coin=coin, kind=kind,
                             K=K, fam=fam, outcome=y, T=parse_end(m["endDate"]),
                             tok=tok, vol=m.get("volumeNum"),
                             feesEnabled=m.get("feesEnabled"), feeType=m.get("feeType")))
    return rows

def px_at(hist, ts, tol=6 * 3600):
    best, bd = None, tol
    for pt in hist:
        d = abs(pt["t"] - ts)
        if d < bd:
            best, bd = pt["p"], d
    return best

def main():
    events = fetch_events()
    hits = sum(1 for v in events.values() if v)
    print(f"events found: {hits}/{len(events)}", file=sys.stderr)
    rows = market_rows(events)
    print(f"resolved markets: {len(rows)}", file=sys.stderr)

    done = set()
    if os.path.exists(ROWS_OUT):
        for line in open(ROWS_OUT):
            try:
                done.add(json.loads(line)["tok"])
            except Exception:
                pass
    from concurrent.futures import ThreadPoolExecutor
    import threading
    f = open(ROWS_OUT, "a")
    lock = threading.Lock()
    todo = [r for r in rows if r["tok"] not in done]
    cnt = [0]

    def work(r):
        T = r["T"]
        sess = requests.Session()
        try:
            resp = sess.get(f"{CLOB}/prices-history",
                            params=dict(market=r["tok"], startTs=int(T - 8.2 * 86400),
                                        endTs=int(T), fidelity=60), timeout=30)
            hist = resp.json().get("history", []) if resp.status_code == 200 else []
        except Exception:
            hist = []
        for lbl, dt in (("p7", 7), ("p3", 3), ("p1", 1)):
            r[lbl] = px_at(hist, T - dt * 86400)
        r["nhist"] = len(hist)
        with lock:
            f.write(json.dumps(r) + "\n")
            f.flush()
            cnt[0] += 1
            if cnt[0] % 100 == 0:
                print(f"hist pulled {cnt[0]}/{len(todo)}", file=sys.stderr)
        time.sleep(0.05)

    with ThreadPoolExecutor(max_workers=8) as ex:
        list(ex.map(work, todo))
    f.close()
    print("done")

if __name__ == "__main__":
    main()
