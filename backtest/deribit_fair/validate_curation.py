#!/usr/bin/env python3
"""策展完整性闸门 (FAIL-CLOSED): 每小时刷新前逐级校验管线输出, 任一级看着"信息不完整"就拒绝覆盖白名单。

  python3 validate_curation.py <new_whitelist.json> [current_whitelist.json]
  exit 0 = 完整, 可覆盖;  exit 1 = 不完整, 保留上一版 (bot 继续热读旧名单; watchdog 会在 >90min 时告警)。

动机: curate 唯一的旧闸门是 slugs>=5, 挡不住"部分拉取" —— 例如 gamma 只翻到 BTC 页 (ETH 分页超时) →
census 缺 ETH → 白名单半残但仍 >=5 → 悄悄覆盖好名单。这里逐级卡: census/deribit/map 都必须两个币齐全
且量在正常带内, 且新名单相对上一版没有异常坍缩 —— 宁可用"旧但完整"也不用"新但残缺"。
"""
import json, math, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
FAILS = []


def load(name):
    try:
        return json.load(open(os.path.join(HERE, name)))
    except Exception as e:
        FAILS.append(f"{name}: 读不动/坏 JSON ({e})")
        return None


def coin_of(slug):
    s = (slug or "").lower()
    if "bitcoin" in s: return "BTC"
    if "ethereum" in s: return "ETH"
    if "solana" in s: return "SOL"
    if "xrp" in s: return "XRP"
    return "?"


def check(cond, msg):
    if not cond:
        FAILS.append(msg)


def main():
    new_wl_path = sys.argv[1]
    cur_wl_path = sys.argv[2] if len(sys.argv) > 2 else None

    # 1) census: 两币齐全 + 总量在带内 (部分拉取会让某币骤降)
    c = load("census_open.json")
    if isinstance(c, list):
        mix = {}
        for r in c:
            mix[coin_of(r.get("slug") or r.get("event_slug") or r.get("question"))] = \
                mix.get(coin_of(r.get("slug") or r.get("event_slug") or r.get("question")), 0) + 1
        check(len(c) >= 400, f"census 行数 {len(c)} < 400 (疑似部分拉取)")
        check(mix.get("BTC", 0) >= 50, f"census BTC 盘 {mix.get('BTC',0)} < 50 (BTC 页可能没拉全)")
        check(mix.get("ETH", 0) >= 50, f"census ETH 盘 {mix.get('ETH',0)} < 50 (ETH 页可能没拉全)")
    elif c is not None:
        FAILS.append("census_open.json 不是 list")

    # 2) deribit: BTC+ETH 都在, 期权量足, 指数价有效 (锚的地基, 残缺则所有 fair 中毒)
    dc = load("deribit_chain.json")
    if isinstance(dc, dict):
        for k in ("BTC", "ETH"):
            v = dc.get(k)
            if not isinstance(v, dict):
                FAILS.append(f"deribit_chain 缺 {k}")
                continue
            check(len(v.get("summary", [])) >= 150, f"deribit {k} 期权 {len(v.get('summary',[]))} < 150")
            idx = v.get("index_usd")
            check(isinstance(idx, (int, float)) and math.isfinite(idx) and idx > 0,
                  f"deribit {k} index_usd 无效: {idx}")
    elif dc is not None:
        FAILS.append("deribit_chain.json 不是 dict")

    # 3) map: 映射行数足 + 两币齐全 (census/deribit 任一残缺都会在这里坍缩)
    dd = load("discrepancies.json")
    if isinstance(dd, list):
        cm = {}
        for r in dd:
            cm[r.get("coin")] = cm.get(r.get("coin"), 0) + 1
        check(len(dd) >= 100, f"discrepancies 行数 {len(dd)} < 100")
        check(cm.get("BTC", 0) >= 30, f"discrepancies BTC {cm.get('BTC',0)} < 30")
        check(cm.get("ETH", 0) >= 30, f"discrepancies ETH {cm.get('ETH',0)} < 30")
    elif dd is not None:
        FAILS.append("discrepancies.json 不是 list")

    # 4) 新白名单: >=5, 且相对上一版没有异常坍缩 (旧闸门只有 >=5)。
    #    regime 感知: mania gate 会合法地缩表甚至清空 (整币暂停) — 这不是"部分拉取"。坍缩比较只在
    #    "未暂停币"上做, 否则狂潮里 gate 反被完整性闸废掉 (gate 存在的意义正是那一刻)。
    try:
        new = json.load(open(new_wl_path))
        slugs = new.get("slugs", [])
        n = len(slugs)
    except Exception as e:
        FAILS.append(f"新白名单读不动: {e}")
        new, slugs, n = {}, [], 0
    paused = set(new.get("regime_paused", []))     # 空 = 无暂停 = 与旧行为完全一致 (向后兼容)
    active = {"BTC", "ETH", "SOL", "XRP"} - paused
    new_np = sum(1 for s in slugs if coin_of(s) not in paused)   # 未暂停币的名单数
    # 有未暂停币就要求它们至少出 5 个 (挡"某币被 mania 暂停时, 另一活跃币又部分拉取"的漏网);
    # 只有全币被暂停 (真·全面狂潮) 才允许空名单 = 整体停手。
    if active:
        check(new_np >= 5, f"未暂停币 slugs {new_np} < 5 (active={sorted(active)}, paused={sorted(paused)}) — 疑似部分拉取")
    if cur_wl_path and os.path.exists(cur_wl_path):
        try:
            prev_slugs = json.load(open(cur_wl_path)).get("slugs", [])
        except Exception:
            prev_slugs = []
        prev_np = sum(1 for s in prev_slugs if coin_of(s) not in paused)
        if prev_np >= 8:  # 上一版(未暂停币)足够大才做坍缩比较, 免小样本噪声
            floor = max(5, int(0.4 * prev_np))
            check(new_np >= floor,
                  f"新白名单未暂停币 {new_np} 相对上版 {prev_np} 坍缩 (需 >= {floor}, paused={sorted(paused)}) — 疑似部分拉取")

    if FAILS:
        print("CURATION VALIDATION FAILED — 保留上一版白名单:", file=sys.stderr)
        for f in FAILS:
            print("  ✗ " + f, file=sys.stderr)
        sys.exit(1)
    print(f"curation validation OK (whitelist {n} slugs)", file=sys.stderr)


if __name__ == "__main__":
    main()
