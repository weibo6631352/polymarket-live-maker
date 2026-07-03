#!/usr/bin/env python3
"""Live-trial ledger for the crypto tail-overpricing edge (companion to paper_ledger.py).

Reads the box's tail_vendor_log.jsonl (pull first: `pmpull /root/polymarket-live-maker/tail_vendor_log.jsonl
~/pm-data/tail_vendor_log.jsonl`), reconstructs real positions from fill events, joins each with
  - resolution status (gamma /markets?slug=)          -> realized P&L once closed
  - Deribit options-implied fair (discrepancies.json)  -> expected edge at entry
and prints the trial scoreboard.

持续盈利判据 (最小份额试验; 2026-07-03 定义):
  领先指标: 每笔成交的入场边际 = (1 - fair_yes) - no_cost > 0 (卖出的溢价确实高于期权隐含公允)。
  确认指标: 已结算仓位的实际 P&L 累计为正, 且 YES 命中数落在 Deribit 隐含的 Poisson 期望 ±2σ 内
            (命中率显著超公允 = 我们被逆选/校准错了, 停; 一次大额尾部命中≈-$15/仓, 属预期方差)。
  样本要求: 边际微小 (~1-3c/仓), 单周期噪声大 — 至少跑过 2 个独立结算簇 (≥2 周) 再下结论。

P&L convention: BUY NO at no_cost; NO resolves 1 -> +(1-no_cost)*shares, NO resolves 0 -> -no_cost*shares.
"""
import json, os, sys, time, urllib.request

LOG = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/pm-data/tail_vendor_log.jsonl")
DISCREPANCIES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "discrepancies.json")
UA = {"User-Agent": "Mozilla/5.0 (live-ledger; read-only)", "Accept": "application/json"}


def get(u, tries=3):
    for i in range(tries):
        try:
            with urllib.request.urlopen(urllib.request.Request(u, headers=UA), timeout=25) as r:
                return json.loads(r.read().decode())
        except Exception:
            if i == tries - 1:
                return None
            time.sleep(0.5 * (i + 1))


def load_events(path):
    evs = []
    if not os.path.exists(path):
        sys.exit(f"log not found: {path} (pmpull it from the box first)")
    for line in open(path):
        try:
            evs.append(json.loads(line))
        except Exception:
            continue
    return evs


def main():
    evs = load_events(LOG)
    token_note = {}   # token -> slug (from place events)
    fills = {}        # token -> {shares, cost, n}
    n_place = n_cancel = 0
    for e in evs:
        ev = e.get("ev")
        if ev == "place" and e.get("resp", {}).get("status") == "PLACED":
            n_place += 1
            token_note[e["resp"].get("token_id", "")] = e.get("note", "")
        elif ev == "cancel":
            n_cancel += 1
        elif ev in ("fill", "fill_recovered", "fill_corrected"):
            f = e.get("fill", {})
            if f.get("side") != "BUY":
                continue
            tok = f.get("token_id", "")
            d = fills.setdefault(tok, {"shares": 0.0, "cost": 0.0, "n": 0})
            d["shares"] += f.get("size", 0.0)
            d["cost"] += f.get("size", 0.0) * f.get("price", 0.0)
            d["n"] += 1

    # Deribit fair by slug (entry-time snapshot; refresh discrepancies.json for later entries)
    fair = {}
    if os.path.exists(DISCREPANCIES):
        for r in json.load(open(DISCREPANCIES)):
            fair[r.get("slug", "")] = r
    print(f"events: {len(evs)}  placed: {n_place}  cancels: {n_cancel}  tokens filled: {len(fills)}")
    if not fills:
        print("no fills yet — resting quotes only; premium accrues when lottery buyers cross.")
        return

    # 只认下过单的 token: 修好的 maker 口径下, 合法 fill 一定落在我们挂过的 token 上;
    # 未挂单 token 的 fill = 旧 taker-视角 bug 的残留行 (对侧 token) 或异常 — 跳过并示警。
    for tok in [t for t in fills if t not in token_note]:
        print(f"⚠ skipping fill on never-placed token {tok[:20]}… "
              f"({fills[tok]['shares']:.1f} sh — stale taker-view row or manual activity; investigate)")
        del fills[tok]

    tot_shares = tot_cost = tot_prem = tot_ev = 0.0
    realized = 0.0
    n_resolved = n_yes = 0
    exp_yes = 0.0  # sum of fair_yes over resolved positions (Poisson expectation of tail hits)
    print(f"\n{'slug':52} {'sh':>6} {'no_cost':>8} {'prem':>7} {'fairY':>6} {'EV':>7}  status")
    for tok, d in sorted(fills.items(), key=lambda kv: -kv[1]["shares"]):
        slug = token_note.get(tok, "?")
        avg_no = d["cost"] / d["shares"] if d["shares"] else 0.0
        prem = (1.0 - avg_no) * d["shares"]
        fr = fair.get(slug, {})
        fair_yes = fr.get("fair_hi")  # conservative upper bound of options-implied P(YES)
        ev = ((1.0 - fair_yes) - avg_no) * d["shares"] if fair_yes is not None else None

        status = "open"
        m = (get(f"https://gamma-api.polymarket.com/markets?slug={slug}") or [None])[0] if slug != "?" else None
        if m and m.get("closed"):
            try:
                yes_px = float(json.loads(m["outcomePrices"])[0]) if isinstance(m.get("outcomePrices"), str) \
                    else float(m["outcomePrices"][0])
            except Exception:
                yes_px = None
            if yes_px is not None:
                n_resolved += 1
                hit = yes_px > 0.5
                n_yes += int(hit)
                if fair_yes is not None:
                    exp_yes += fair_yes
                pnl = (-avg_no if hit else (1.0 - avg_no)) * d["shares"]
                realized += pnl
                status = f"RESOLVED {'YES(hit)' if hit else 'NO(win)'} pnl={pnl:+.2f}"
        tot_shares += d["shares"]
        tot_cost += d["cost"]
        tot_prem += prem
        if ev is not None:
            tot_ev += ev
        print(f"{slug:52} {d['shares']:6.1f} {avg_no:8.3f} {prem:7.2f} "
              f"{(f'{fair_yes:6.4f}' if fair_yes is not None else '     ?')} "
              f"{(f'{ev:7.2f}' if ev is not None else '      ?')}  {status}")

    print(f"\ndeployed (filled collateral): ${tot_cost:.2f}   premium sold: ${tot_prem:.2f}   "
          f"entry EV vs Deribit fair: ${tot_ev:+.2f}")
    if n_resolved:
        sigma = (exp_yes ** 0.5) if exp_yes > 0 else float("nan")
        print(f"resolved: {n_resolved}  tail hits (YES): {n_yes}  expected hits (Deribit): {exp_yes:.2f} "
              f"(±2σ ≈ {2*sigma:.2f})   realized P&L: ${realized:+.2f}")
        if exp_yes > 0 and n_yes > exp_yes + 2 * sigma:
            print("⚠ hits exceed fair expectation +2σ — adverse selection / miscalibration; STOP and re-audit.")
    else:
        print("resolved: 0 — realized P&L arrives with the first settlement cluster (Jul 6-8).")


if __name__ == "__main__":
    main()
