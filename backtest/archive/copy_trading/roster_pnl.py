#!/usr/bin/env python3
"""Find WHO actually makes money on Polymarket (reliable realized P&L) + strategy signals.
ZERO real money — read-only public-API GET only, no orders.

The /leaderboard pnl field is unreliable (all-time #1 ~ $31k, implausible). So compute P&L
INDEPENDENTLY two ways and cross-check:
  A) /positions aggregate: sum(cashPnl)=unrealized + sum(realizedPnl)=realized-on-open  (current book)
  B) /activity cash-flow:  sum(SELL.usdcSize + REDEEM.usdcSize) - sum(BUY.usdcSize) + open_value
                            = all-time realized+unrealized over the activity covered (capped pages)

Roster = profit-leaderboard (all windows/categories) + known concentrated-directional wallets +
volume leaders. Per account, also derive STRATEGY signals: market-type mix, buy-side bias + avg buy
price (longshot-No harvesting shows as high-price No buys), concentration, bet count, time span,
neutral(holds both sides) vs directional. Skill(many bets, steady) vs variance(few big bets) tag.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import curated_backtest as cb  # noqa: E402

DA = "https://data-api.polymarket.com"
ACT_MAX_OFFSET = 3000  # data-api caps offset ~3000


def get(u):
    try:
        return cb.http_get(u)
    except Exception:  # noqa: BLE001
        return None


def market_type(title, slug=""):
    q = (title + " " + slug).lower()
    if any(k in q for k in ("election", "president", "nominee", "primary", "senate", "governor",
                            "parliament", "prime minister", "chancellor", "coalition", "impeach")):
        return "politics"
    if any(k in q for k in ("bitcoin", "btc", "ethereum", " eth ", "solana", "crypto", "fdv", "token", "$")):
        return "crypto"
    if any(k in q for k in ("up or down", "fed", "gdp", "cpi", "rate", "temperature", "jobs", "inflation")):
        return "macro"
    if any(k in q for k in ("lol:", "valorant", "counter-strike", "dota", "cs2", "esports", "ewc")):
        return "esports"
    if any(k in q for k in ("vs", "win", "cup", "league", "nba", "nfl", "mlb", "score", "goal", "match",
                            "champion", "playoff", "open", "trophy", "final")):
        return "sports"
    return "other"


def positions_pnl(w):
    pos = get(f"{DA}/positions?user={w}&limit=500") or []
    cash = sum(float(p.get("cashPnl", 0) or 0) for p in pos)
    real = sum(float(p.get("realizedPnl", 0) or 0) for p in pos)
    openval = sum(float(p.get("currentValue", 0) or 0) for p in pos)
    # concentration: largest single-position |pnl| share
    pnls = sorted((abs(float(p.get("cashPnl", 0) or 0) + float(p.get("realizedPnl", 0) or 0)) for p in pos),
                  reverse=True)
    tot = sum(pnls) or 1.0
    conc = pnls[0] / tot if pnls else 0.0
    # neutrality: fraction of positions where they hold both YES and NO of same event (mergeable)
    mergeable = sum(1 for p in pos if p.get("mergeable"))
    return dict(npos=len(pos), cash=cash, real=real, openval=openval, conc=conc,
                mergeable_frac=mergeable / len(pos) if pos else 0.0, pos=pos)


def activity_pnl(w):
    """Cash-flow realized over covered activity; also strategy signals (buy price/side, market mix)."""
    buy = sell = redeem = 0.0
    n_trades = n_redeem = 0
    buy_px_sum = buy_n = 0.0
    side_no_buys = side_yes_buys = 0
    mkt = {}
    conds = set()
    first_ts = last_ts = None
    off = 0
    while off <= ACT_MAX_OFFSET:
        a = get(f"{DA}/activity?user={w}&limit=500&offset={off}")
        if not isinstance(a, list) or not a:
            break
        for r in a:
            t = str(r.get("type"))
            usd = float(r.get("usdcSize", 0) or 0)
            ts = int(r.get("timestamp", 0) or 0)
            first_ts = ts if first_ts is None else min(first_ts, ts)
            last_ts = ts if last_ts is None else max(last_ts, ts)
            conds.add(r.get("conditionId"))
            mt = market_type(str(r.get("title", "")), str(r.get("slug", "")))
            mkt[mt] = mkt.get(mt, 0.0) + usd
            if t == "TRADE":
                n_trades += 1
                side = str(r.get("side"))
                px = float(r.get("price", 0) or 0)
                oc = str(r.get("outcome", "")).lower()
                if side == "BUY":
                    buy += usd
                    buy_px_sum += px; buy_n += 1
                    if oc in ("no", "n"):
                        side_no_buys += 1
                    else:
                        side_yes_buys += 1
                else:
                    sell += usd
            elif t in ("REDEEM", "REWARD", "CONVERSION"):
                redeem += usd
                if t == "REDEEM":
                    n_redeem += 1
        off += 500
        if len(a) < 500:
            break
        time.sleep(0.05)
    capped = off > ACT_MAX_OFFSET
    return dict(buy=buy, sell=sell, redeem=redeem, n_trades=n_trades, n_redeem=n_redeem,
                avg_buy_px=(buy_px_sum / buy_n if buy_n else 0), no_buy_frac=(side_no_buys / max(buy_n, 1)),
                mkt=mkt, n_markets=len(conds), capped=capped,
                span_days=((last_ts - first_ts) / 86400.0 if first_ts and last_ts else 0))


def build_roster():
    seeds = {}
    # leaderboard cuts
    for win in ("all", "monthly", "weekly"):
        for cat in ("", "&category=SPORTS", "&category=POLITICS", "&category=CRYPTO", "&category=ECONOMICS"):
            lb = get(f"{DA}/v1/leaderboard?window={win}&limit=12&type=profit{cat}") or []
            for r in (lb if isinstance(lb, list) else []):
                seeds[r["proxyWallet"]] = str(r.get("userName", ""))[:16]
    # known concentrated-directional + farmers spotted earlier (by wallet where known)
    known = {
        "0x84cfffc3f16dcc353094de30d4a45226eccd2f63": "mooseborzoi",
        "0x204f72f35326db932158cba6adff0b9a1da95e14": "swisstony",
    }
    seeds.update(known)
    return seeds


def main():
    roster = build_roster()
    print(f"# roster: {len(roster)} candidate accounts (profit-board all windows/cats + known)\n", flush=True)
    rows = []
    for w, nm in roster.items():
        pp = positions_pnl(w)
        ap = activity_pnl(w)
        total_book = pp["cash"] + pp["real"]                       # method A (current book)
        total_flow = ap["sell"] + ap["redeem"] - ap["buy"] + pp["openval"]  # method B (all-time, capped)
        bets = ap["n_markets"]
        # skill vs variance: many resolved bets + low concentration = skill
        tag = "SKILL" if (bets >= 40 and pp["conc"] < 0.5 and ap["n_redeem"] >= 10) else \
              ("VARIANCE" if bets <= 8 else "MIXED")
        topmkt = max(ap["mkt"].items(), key=lambda x: x[1])[0] if ap["mkt"] else "?"
        rows.append((nm, w, total_book, total_flow, bets, pp, ap, tag, topmkt))
        print(f"  {nm:16s} bookPnl=${total_book:11.0f} flowPnl=${total_flow:11.0f} bets={bets:4d} "
              f"redeem={ap['n_redeem']:4d} conc={pp['conc']:.2f} noBuy%={ap['no_buy_frac']*100:3.0f} "
              f"avgBuyPx={ap['avg_buy_px']:.2f} top={topmkt:8s} {'CAP' if ap['capped'] else '   '} {tag}",
              flush=True)
        time.sleep(0.05)

    print("\n# === TOP by book P&L (current positions) ===")
    for r in sorted(rows, key=lambda x: -x[2])[:12]:
        print(f"#  {r[0]:16s} ${r[2]:11.0f}  bets={r[4]:4d}  {r[7]:8s}  top={r[8]}")
    print("\n# === TOP by activity cash-flow P&L (all-time, capped) ===")
    for r in sorted(rows, key=lambda x: -x[3])[:12]:
        print(f"#  {r[0]:16s} ${r[3]:11.0f}  bets={r[4]:4d}  redeem={r[6]['n_redeem']:4d}  {r[7]:8s}  top={r[8]}")


if __name__ == "__main__":
    main()
