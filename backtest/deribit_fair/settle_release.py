#!/usr/bin/env python3
"""结算释放例程 (在箱上跑): 已结算市场的 held 额度释放 + 已实现 P&L 记账。

  python3 settle_release.py          # dry: 只报告哪些 token 已结算、赢/输、可释放多少
  python3 settle_release.py --apply  # 停 unit → 改 state (释放 held) → 记 ledger 事件 → 起 unit

赢仓 (NO=1) 的 USDC 要赎回才回账 (PM 不自动赎回) — 本脚本只管额度与记账;
赎回按 docs/OPS-ROUTINE.md (试验期走 PM 网页 Claim)。
"""
import json, subprocess, sys, time, urllib.request

STATE = "/root/polymarket-live-maker/state/tail_vendor_held.json"
LOG = "/root/polymarket-live-maker/tail_vendor_log.jsonl"
UA = {"User-Agent": "research/0.1"}


def get(u, tries=3):
    for i in range(tries):
        try:
            with urllib.request.urlopen(urllib.request.Request(u, headers=UA), timeout=30) as r:
                return json.loads(r.read())
        except Exception:
            if i == tries - 1:
                return None
            time.sleep(1 + i)


def main():
    apply = "--apply" in sys.argv
    st = json.load(open(STATE))
    th, notes, tcoin = st.get("token_held", {}), st.get("token_note", {}), st.get("token_coin", {})
    resolved = []
    for tok, sh in list(th.items()):
        if sh <= 0:
            continue
        slug = notes.get(tok, "")
        m = None
        if slug:
            # gamma /markets 默认过滤 closed 行 (删失陷阱, EXECUTION-MECHANICS) — 先查活的, 空则查 closed
            rows = get(f"https://gamma-api.polymarket.com/markets?slug={slug}") or []
            if not rows:
                rows = get(f"https://gamma-api.polymarket.com/markets?slug={slug}&closed=true") or []
            m = rows[0] if rows else None
        if not m or not m.get("closed"):
            continue
        try:
            op = m.get("outcomePrices")
            op = json.loads(op) if isinstance(op, str) else op
            yes_won = float(op[0]) > 0.5
        except Exception:
            continue
        # 我们持有 NO: yes_won -> 归零 (亏 held 成本); !yes_won -> NO=1 (赢, 待赎回 $sh)
        resolved.append({"token": tok, "slug": slug, "shares": sh, "coin": tcoin.get(tok, ""),
                         "no_won": (not yes_won)})
    if not resolved:
        print("no resolved holdings")
        return
    for r in resolved:
        print(f"{'WIN ' if r['no_won'] else 'LOSS'} {r['shares']:5.1f} sh  {r['slug']}")
    if not apply:
        print("(dry — --apply to release)")
        return
    subprocess.run(["systemctl", "stop", "tail-vendor-trial"], check=True)
    try:
        st = json.load(open(STATE))  # 重读 (停机后最终版)
        ev = {"ev": "settle_release", "t_ms": int(time.time() * 1000), "released": []}
        for r in resolved:
            tok = r["token"]
            sh = st["token_held"].get(tok, 0.0)
            if sh <= 0:
                continue
            st["held_total"] = max(0.0, st["held_total"] - sh)
            c = st["token_coin"].get(tok, "")
            if c in st.get("held_coin", {}):
                st["held_coin"][c] = max(0.0, st["held_coin"][c] - sh)
                if st["held_coin"][c] == 0.0:
                    del st["held_coin"][c]
            st["token_held"][tok] = 0.0
            ev["released"].append({"slug": r["slug"], "shares": sh, "no_won": r["no_won"],
                                   "pending_redeem_usd": sh if r["no_won"] else 0.0})
        json.dump(st, open(STATE, "w"))
        with open(LOG, "a") as f:
            f.write(json.dumps(ev) + "\n")
        print("released; held_total ->", st["held_total"])
    finally:
        subprocess.run(["systemctl", "start", "tail-vendor-trial"], check=True)


if __name__ == "__main__":
    main()
