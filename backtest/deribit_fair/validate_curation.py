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

    # 4) 新白名单: >=5, 且相对上一版没有异常坍缩 (旧闸门只有 >=5)
    try:
        new = json.load(open(new_wl_path))
        n = len(new.get("slugs", []))
    except Exception as e:
        FAILS.append(f"新白名单读不动: {e}")
        n = 0
    check(n >= 5, f"新白名单 slugs {n} < 5")
    if cur_wl_path and os.path.exists(cur_wl_path):
        try:
            prev = len(json.load(open(cur_wl_path)).get("slugs", []))
        except Exception:
            prev = 0
        if prev >= 8:  # 上一版足够大才做坍缩比较, 免小样本噪声
            floor = max(5, int(0.4 * prev))
            check(n >= floor, f"新白名单 {n} 相对上版 {prev} 坍缩 (需 >= {floor}) — 疑似部分拉取")

    if FAILS:
        print("CURATION VALIDATION FAILED — 保留上一版白名单:", file=sys.stderr)
        for f in FAILS:
            print("  ✗ " + f, file=sys.stderr)
        sys.exit(1)
    print(f"curation validation OK (whitelist {n} slugs)", file=sys.stderr)


if __name__ == "__main__":
    main()
