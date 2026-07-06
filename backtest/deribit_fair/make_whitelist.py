#!/usr/bin/env python3
"""Generate the tail-vendor curation whitelist (state/tail_vendor_whitelist.json on the box).

会话策展环:本脚本 = "我算边际,bot 只执行"。跑法(先刷新锚定数据):
    python3 census.py && python3 deribit_pull.py && python3 map_markets.py
    python3 make_whitelist.py [N=12] > whitelist.json

选择逻辑(镜像 bot 的 decide() 可交易域 + 校准结论 "只卖上行尾 2-7c"):
  - 上行方向(kind terminal_gt / above 语义),YES ask ∈ [0.021, 0.070],days_left ∈ [1, 6];
  - 卖价 = max(0.02, ask − 1 tick)(bot 的定价规则);
  - BTC/ETH(有 Deribit 锚): edge = sell_px − fair_hi,要求 ≥ 0.8c;
  - 排名分 = edge × 活跃度权重 w(vol24) —— 实盘证实成交集中在有 taker 流量的近尾 (3.5-6c),
    纯边际排名会把资金铺到没人吃的深尾;w = clip(sqrt(vol24/2000), 0.25, 1)。
  - SOL/XRP(无期权锚): 历史校准比率(SOL 10.8x / XRP 6.4x, 32,570 盘校准)×溢价 →
    expected_edge ≈ sell_px × (1 − 1/ratio),与锚定 edge 同一尺度合并排名。
  - 输出 top-N slugs;bot 端热读,名单外挂单自动撤(资本轮换)。
"""
import json, os, re, sys, time
import requests

HERE = os.path.dirname(os.path.abspath(__file__))
GAMMA = "https://gamma-api.polymarket.com"
HIST_RATIO = {"SOL": 10.8, "XRP": 6.4}   # 无锚币的历史高估比率(校准 2026-07-03)
TICK = 0.001
BAND = (0.021, 0.070)
DAYS = (1.0, 6.0)
MIN_ANCHORED_EDGE = 0.008

def days_left(T):
    return (T - time.time()) / 86400.0

def sell_px(ask):
    return max(0.02, round(ask - TICK, 3))


def parse_strike(slug):
    """slug 里的执行价: above-68k -> 68000, above-1pt1 -> 1.1, above-120 -> 120。解析不出 -> None。"""
    m = re.search(r"above-(\d+(?:pt\d+)?)(k?)", slug or "")
    if not m:
        return None
    v = float(m.group(1).replace("pt", "."))
    return v * (1000 if m.group(2) == "k" else 1)


def spot_prices():
    out = {}
    for sym, coin in (("SOLUSDT", "SOL"), ("XRPUSDT", "XRP"), ("BTCUSDT", "BTC"), ("ETHUSDT", "ETH")):
        try:
            r = requests.get(f"https://data-api.binance.vision/api/v3/ticker/price?symbol={sym}",
                             timeout=15, headers={"User-Agent": "research/0.1"})
            out[coin] = float(r.json()["price"])
        except Exception:
            pass
    return out


def act_w(vol24):
    """活跃度权重: 无流量深尾降权 (仍保留 0.25 — 免费彩票), 活跃市场满权。"""
    try:
        v = float(vol24 or 0)
    except Exception:
        v = 0.0
    return max(0.25, min(1.0, (v / 2000.0) ** 0.5))

def anchored_rows():
    d = json.load(open(os.path.join(HERE, "discrepancies.json")))
    out = []
    spots = spot_prices()
    COIN_KEY = {"BTC": "BTC", "ETH": "ETH"}
    for r in d:
        if r.get("kind") != "terminal_gt" or not r.get("ask") or r.get("fair_hi") is None:
            continue
        if not (DAYS[0] <= days_left(r["T"]) <= DAYS[1]):
            continue
        # 执行价-现货皮带 (锚定路径也要: 防 fair 映射错误/K 错位让实值盘混入 — clamp 事故的同类洞)
        spot = spots.get(r.get("coin"))
        if not r.get("K") or not spot or r["K"] < spot * 1.03:
            continue
        clamp = False
        if r["ask"] > BAND[1]:
            # 首卖 (clamp): ask 肥/空书但锚定 fair 极低 → 授权 bot 站到带顶 7c 当第一个卖家。
            # 锚是唯一授权来源: fair_hi ≤ 2% 才够 (7c − 2% ≥ 5c 边际); bid 已抬到带内的不碰。
            if r["fair_hi"] <= 0.02 and (r.get("bid") or 0) <= 0.04 and r["ask"] <= 0.98:
                clamp = True
            else:
                continue
        elif r["ask"] < BAND[0]:
            continue
        px = BAND[1] if clamp else sell_px(r["ask"])
        edge = px - r["fair_hi"]
        if edge < MIN_ANCHORED_EDGE:
            continue
        out.append({"slug": r["slug"], "coin": r["coin"], "ask": r["ask"], "sell": px,
                    "fair_hi": r["fair_hi"], "edge": round(edge, 4), "anchor": "deribit",
                    "days": round(days_left(r["T"]), 2), "vol24": round(float(r.get("vol24") or 0)),
                    "clamp": clamp,
                    "score": round(edge * (act_w(r.get("vol24")) if not clamp else 0.6), 4)})
    return out

def unanchored_rows():
    """SOL/XRP strike dailies via the bot's own gamma series (10022=SOL, 10023=XRP)."""
    out = []
    spots = spot_prices()
    # XRP 日盘实际挂在 series 10024 (10023 空转了两天, 2026-07-05 普查发现); 两个都查取并集。
    for sid, coin in (("10022", "SOL"), ("10023", "XRP"), ("10024", "XRP")):
        try:
            evs = requests.get(f"{GAMMA}/events",
                               params={"series_id": sid, "closed": "false", "limit": "100"},
                               timeout=30, headers={"User-Agent": "research/0.1"}).json()
        except Exception:
            continue
        for ev in evs or []:
            for m in ev.get("markets", []):
                slug = (m.get("slug") or "").lower()
                q = (m.get("question") or "").lower()
                sq = slug + " " + q
                if not any(w in sq for w in ("above", "greater than", "-greater-", "or higher", "or-higher")):
                    continue
                if any(w in sq for w in ("below", "less than", "or lower", "or-lower", "dip")):
                    continue
                try:
                    ask = float(m.get("bestAsk"))
                    end = m.get("endDate", "")
                    import datetime
                    T = datetime.datetime.fromisoformat(end.replace("Z", "+00:00")).timestamp()
                except Exception:
                    continue
                if not (DAYS[0] <= days_left(T) <= DAYS[1]):
                    continue
                try:
                    bid = float(m.get("bestBid") or 0)
                except Exception:
                    bid = 0.0
                # 执行价-现货常识校验 (2026-07-06 实亏教训: "SOL above 30" 在 SOL=82 时是深度实值,
                # 空 bid 让 bid≤0.04 形同虚设 → 卖 7c 的必赢 YES = 送钱)。无锚行一律要求真 OTM。
                strike = parse_strike(slug)
                spot = spots.get(coin)
                if strike is None or spot is None or strike < spot * 1.05:
                    continue
                clamp = False
                if ask > BAND[1]:
                    # 首卖授权: 需可见买盘且 ≤4c (None/空书按拒绝处理) + 更深的 OTM 余量
                    raw_bid = m.get("bestBid")
                    if raw_bid is not None and 0 < float(raw_bid) <= 0.04 and strike >= spot * 1.08:
                        clamp = True
                    else:
                        continue
                elif ask < BAND[0]:
                    continue
                ratio = HIST_RATIO[coin]
                px = BAND[1] if clamp else sell_px(ask)
                v24 = m.get("volume24hr") or 0
                e = px * (1 - 1 / ratio)
                out.append({"slug": m.get("slug"), "coin": coin, "ask": ask, "sell": px,
                            "fair_hi": round(px / ratio, 4), "edge": round(e, 4),
                            "anchor": f"hist{ratio}x", "days": round(days_left(T), 2),
                            "vol24": round(float(v24)), "clamp": clamp,
                            "score": round(e * (act_w(v24) if not clamp else 0.5), 4)})
    return out

def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 12
    rows = sorted(anchored_rows() + unanchored_rows(), key=lambda r: -r["score"])
    seen, top = set(), []
    for r in rows:
        if r["slug"] in seen:
            continue
        seen.add(r["slug"])
        top.append(r)
        if len(top) >= n:
            break
    for r in top:
        print(f"  {r['slug']:55} {r['coin']:4} ask={r['ask']:.3f} sell={r['sell']:.3f} "
              f"fair≤{r['fair_hi']:.4f} edge={r['edge']:+.4f} v24={r['vol24']:>6} score={r['score']:.4f} "
              f"d={r['days']:.1f} [{r['anchor']}]", file=sys.stderr)
    print(json.dumps({"generated_at": int(time.time()), "criteria": "up-tail 2.1-7c, d1-6, ranked edge+activity; clamp=first-seller",
                      "slugs": [r["slug"] for r in top],
                      "clamp": [r["slug"] for r in top if r.get("clamp")],
                      "detail": top}, indent=1))

if __name__ == "__main__":
    main()
