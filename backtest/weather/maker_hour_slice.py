#!/usr/bin/env python3
"""Slice temp-dailies MAKER settle-P&L by CITY-LOCAL HOUR (zero-cost iteration driver).

Motivation (2026-07-04): the live temp-maker trial lost 11.2$ with every maker fill landing in the
Shanghai 11:00-13:00 ramp (-14c/sh settle-P&L), while the pooled resolution-day stat said +3.77c/sh.
Hypothesis: capture is hour-dependent — pre-ramp morning flow is noise (maker-positive), the
afternoon ramp is informed (maker-toxic). This script tests that on the archived jul-3 tape.

Input: ~/pm-data/archive-2026-07-03/ramp_trades_final.jsonl.gz (poll snapshots of the public tape,
taker view). Maker P&L/sh per print = taker BUY ? (price - settle) : (settle - price), where
settle(asset) = 1 if that token won, else 0. Resolutions + token maps pulled from gamma by
condition_ids (closed markets carry outcomePrices).
"""
import gzip, json, os, sys, time, urllib.request
from collections import defaultdict

ARCH = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else "~/pm-data/archive-2026-07-03/ramp_trades_final.jsonl.gz")
UA = {"User-Agent": "research/0.1"}
TZ = {  # city (slug fragment) -> UTC offset hours (2026 summer)
    "nyc": -4, "miami": -4, "toronto": -4, "philadelphia": -4, "boston": -4, "atlanta": -4,
    "washington": -4, "detroit": -4, "charlotte": -4, "tampa": -4, "orlando": -4,
    "chicago": -5, "dallas": -5, "houston": -5, "austin": -5, "minneapolis": -5, "kansas-city": -5,
    "denver": -6, "phoenix": -7, "las-vegas": -7, "los-angeles": -7, "san-francisco": -7,
    "seattle": -7, "portland": -7, "san-diego": -7,
    "london": 1, "dublin": 1, "lagos": 1, "paris": 2, "madrid": 2, "berlin": 2, "rome": 2,
    "amsterdam": 2, "brussels": 2, "vienna": 2, "warsaw": 2, "stockholm": 2, "zurich": 2,
    "johannesburg": 2, "moscow": 3, "istanbul": 3, "tel-aviv": 3, "athens": 3, "kyiv": 3,
    "cairo": 2, "nairobi": 3, "riyadh": 3, "dubai": 4, "karachi": 5, "delhi": 5.5, "mumbai": 5.5,
    "lucknow": 5.5, "dhaka": 6, "bangkok": 7, "jakarta": 7, "singapore": 8, "hong-kong": 8,
    "shanghai": 8, "guangzhou": 8, "shenzhen": 8, "beijing": 8, "chengdu": 8, "taipei": 8,
    "manila": 8, "seoul": 9, "tokyo": 9, "sydney": 10, "melbourne": 10, "brisbane": 10,
    "auckland": 12, "buenos-aires": -3, "sao-paulo": -3, "santiago": -4, "bogota": -5, "lima": -5,
    "mexico-city": -6, "panama-city": -5, "honolulu": -10, "anchorage": -8, "ankara": 3,
    "wellington": 12, "abu-dhabi": 4, "doha": 3, "kuwait-city": 3, "casablanca": 1,
    "montreal": -4, "vancouver": -7, "calgary": -6, "edmonton": -6, "winnipeg": -5,
}


def get(u, tries=3):
    for i in range(tries):
        try:
            with urllib.request.urlopen(urllib.request.Request(u, headers=UA), timeout=30) as r:
                return json.loads(r.read())
        except Exception:
            if i == tries - 1:
                return None
            time.sleep(1 + i)


def prints_from_archive():
    prints, seen = [], set()
    with gzip.open(ARCH, "rt") as f:
        for line in f:
            j = json.loads(line)
            for t in j.get("trades", []):
                key = (t.get("transactionHash"), t.get("asset"), t.get("size"), t.get("price"))
                if key in seen:
                    continue
                seen.add(key)
                prints.append(t)
    return prints


def prints_from_cohort(date_iso):
    """已结算队列直取: gamma tag 104596 + end-date 窗 → 每市场公开 tape (无需预先录制)。"""
    evs = []
    off = 0
    while True:
        batch = get(f"https://gamma-api.polymarket.com/events?tag_id=104596&closed=true"
                    f"&end_date_min={date_iso}T00:00:00Z&end_date_max={date_iso}T23:59:59Z"
                    f"&limit=100&offset={off}") or []
        evs += batch
        if len(batch) < 100:
            break
        off += 100
    conds = [m["conditionId"] for e in evs for m in e.get("markets", []) if m.get("conditionId")]
    print(f"cohort {date_iso}: {len(evs)} events, {len(conds)} buckets", file=sys.stderr)
    prints = []
    for i, c in enumerate(conds):
        for t in get(f"https://data-api.polymarket.com/trades?market={c}&limit=1000") or []:
            prints.append(t)
        if i % 100 == 0:
            print(f"  tape {i}/{len(conds)}", file=sys.stderr)
    return prints


def main():
    if len(sys.argv) > 2 and sys.argv[1] == "--cohort":
        prints = prints_from_cohort(sys.argv[2])
    else:
        prints = prints_from_archive()
    conds = sorted({t["conditionId"] for t in prints})
    print(f"{len(prints)} prints across {len(conds)} conds", file=sys.stderr)

    meta = {}
    for i in range(0, len(conds), 20):  # gamma 数组参数 = 重复 query 形式
        batch = "&".join(f"condition_ids={c}" for c in conds[i:i + 20])
        for m in get(f"https://gamma-api.polymarket.com/markets?{batch}&limit=20") or []:
            try:
                toks = m.get("clobTokenIds")
                toks = json.loads(toks) if isinstance(toks, str) else toks
                op = m.get("outcomePrices")
                op = json.loads(op) if isinstance(op, str) else op
                yes_won = float(op[0]) > 0.5
                slug = (m.get("slug") or "")
                city = next((c for c in TZ if f"-in-{c}-" in "-in-" + slug or f"in-{c}-on" in slug), None)
                if not m.get("closed") or city is None:
                    continue
                meta[m["conditionId"]] = {"yes": toks[0], "no": toks[1], "yes_won": yes_won, "city": city}
            except Exception:
                continue
    print(f"resolved+mapped conds: {len(meta)}", file=sys.stderr)

    by_hour = defaultdict(lambda: [0.0, 0.0])  # hour -> [shares, $]
    tot_sh = tot_pnl = 0.0
    skipped = 0
    for t in prints:
        mm = meta.get(t["conditionId"])
        if mm is None:
            skipped += 1
            continue
        settle = None
        if t["asset"] == mm["yes"]:
            settle = 1.0 if mm["yes_won"] else 0.0
        elif t["asset"] == mm["no"]:
            settle = 0.0 if mm["yes_won"] else 1.0
        if settle is None:
            skipped += 1
            continue
        px, sz = float(t["price"]), float(t["size"])
        pnl_sh = (px - settle) if t["side"] == "BUY" else (settle - px)  # MAKER = taker 的对手
        lh = int((int(t["timestamp"]) / 3600.0 + TZ[mm["city"]]) % 24)
        by_hour[lh][0] += sz
        by_hour[lh][1] += pnl_sh * sz
        tot_sh += sz
        tot_pnl += pnl_sh * sz
    print(f"skipped {skipped} prints (unresolved/unmapped)", file=sys.stderr)
    print(f"\nALL: {tot_sh:.0f} sh, maker settle-P&L {tot_pnl:+.0f}$ = {tot_pnl/tot_sh*100:+.2f}c/sh")
    print(f"{'local-h':>7} {'shares':>9} {'$':>9} {'c/sh':>7}")
    for h in sorted(by_hour):
        sz, p = by_hour[h]
        print(f"{h:>5}:00 {sz:9.0f} {p:+9.1f} {p/sz*100:+7.2f}")


if __name__ == "__main__":
    main()
