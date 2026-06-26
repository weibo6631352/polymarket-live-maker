"""Portfolio orchestrator — turn the pool scanner + single-pool bot into a system.

The validated edge is capacity-constrained: each two-sided ``min_size`` quote earns
a few $/day but locks only ~$50-100, so the strategy is "spread a small book across
many uncorrelated SAFE mid-tail pools."  This module does exactly that:

  scan → select fundable SAFE pools within a capital budget → one dry-run maker bot
  per pool → aggregate expected reward, committed capital, and per-poll plans.

Dry-run only (no real orders); composes ``scanner`` + ``live`` + ``reward_math``.
"""

from __future__ import annotations

import re

from pm_trader.maker_live import LiveMakerBot
from pm_trader.orderbook import committed_capital, maker_quote_score, maker_reward_share
from pm_trader.rewards import RewardsClient, scan

DEFAULT_CAPITAL = 1000.0

# Generic words that carry no correlation signal (so two markets sharing only
# these are NOT treated as the same event).
_STOPWORDS = frozenset(
    "will the be in on at a an of to and or by win wins won most next be the for "
    "vs end up down above below before after into out as is are reach hit with "
    "who what when which than then over under between".split()
)


def _significant_tokens(question: str) -> set[str]:
    """Distinctive lowercase tokens of a question (entities/places), minus
    stopwords, pure numbers, and very short tokens — used to detect correlation."""
    toks = re.findall(r"[a-z0-9]+", (question or "").lower())
    return {t for t in toks if len(t) > 2 and not t.isdigit() and t not in _STOPWORDS}


# Curated topic/entity clusters: markets matching the same cluster move together
# (same FOMC, same country's politics, same race) even when they share no surface
# words — e.g. "Grindeanu next PM" and "Bolojan out" are both the Romanian PM.
# Surface-token overlap can't catch those, so we cluster on known correlated themes.
# Extend as new correlated themes appear in the reward universe.
_CLUSTER_RULES: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("us-fed", ("fed", "fomc", "federal reserve")),
    ("romania", ("romania", "grindeanu", "bolojan", "ciolacu")),
    ("colombia", ("colombia", "petro")),
    ("israel", ("israel", "netanyahu", "knesset", "eizenkot", "bennett")),
    ("france", ("france", "french", "bardella", "macron", "philippe", "melenchon")),
    ("ecb", ("ecb", "european central bank", "lagarde")),
    ("boe", ("bank of england", "boe")),
)


def _cluster_key(question: str) -> str | None:
    """Canonical correlation cluster for a question, or None if it matches no
    known correlated theme.  Catches same-event/same-country pairs that share no
    surface words.  Heuristic + curated — it cannot catch every entity link
    (e.g. an unlisted politician's name), so it complements, not replaces, the
    token-overlap check."""
    q = (question or "").lower()
    for name, keywords in _CLUSTER_RULES:
        if any(kw in q for kw in keywords):
            return name
    return None


def _risk_adjusted_score(p: dict, risk_tolerance_days: float,
                         chop_aversion: float = 1.0) -> float:
    """Reward per day, discounted by BOTH jump-tail AND steady-chop exposure.

    ``reward_per_day`` is the steady income. Two risk dimensions shrink it:
      - jump tail: ``days_wiped`` = days of income one historical MAX jump erases.
        FULLY weighted — a discrete gap fills before any cancel, so reaction speed
        cannot make jumpy pools safe.
      - steady chop: ``daily_vol_c`` = std of typical daily mid moves (cents),
        normalised by the band (``max_spread_c``). Continuous chop IS cancellable,
        so ``chop_aversion`` (0..1) scales this penalty down for a fast canceller
        (1.0 = a slow poller's full aversion; 0.5 = half). Ranking thus favours
        high-yield AND jump-safe pools, with chop-tolerance tuned to our latency.
    """
    reward = p.get("reward_per_day")
    if reward is None:
        reward = p.get("share", 0.0) * p.get("daily", 0.0)
    days_wiped = p.get("days_wiped")
    if days_wiped is None:
        days_wiped = 9_999.0
    jump_disc = 1.0 + days_wiped / risk_tolerance_days
    vol = p.get("daily_vol_c") or 0.0           # 0/None (no history) -> no chop penalty
    band = p.get("max_spread_c") or 0.0
    chop_disc = 1.0 + chop_aversion * (vol / band if band > 0 else 0.0)
    return reward / (jump_disc * chop_disc)


def _deploy_size(p: dict, half_spread_c: float, *, target_cap: float,
                 share_cap: float, loss_budget: float) -> float:
    """Capital-aware order size for a pool, >= min_size, capped by THREE limits so
    sizing up never quietly blows risk:
      - capital: don't lock more than ``target_cap`` (the per-pool capital slice).
      - reward-share: keep our est share <= ``share_cap`` — reward is ~linear in size
        only while our share is small; past that we'd dominate the pool, the reward
        saturates, and we become the adverse-selection target.
      - jump risk: a single worst-case historical jump (``max_jump_c``) on this size
        must not exceed ``loss_budget`` — so a gap we CAN'T cancel stays survivable.
    """
    min_size = p["min_size"]
    per_share = committed_capital(min_size, half_spread_c) / min_size  # capital lock / share
    cap_capital = (target_cap / per_share) if per_share > 0 else min_size
    w = maker_quote_score(1.0, half_spread_c, p.get("max_spread_c") or 0.0)  # in-band weight
    comp = p.get("min_side_score") or 0.0                                    # competitors' Qmin
    cap_share = (share_cap / (1.0 - share_cap) * comp / w) if (w > 0 and 0 < share_cap < 1) else cap_capital
    mj = (p.get("max_jump_c") or 0.0) / 100.0
    cap_risk = (loss_budget / mj) if mj > 0 else cap_capital
    return max(min_size, min(cap_capital, cap_share, cap_risk))


def select_pools(
    scan_report: dict,
    *,
    capital: float = DEFAULT_CAPITAL,
    require_safe: bool = True,
    half_spread_ticks: int = 1,
    risk_tolerance_days: float = 7.0,
    chop_aversion: float = 1.0,
    max_token_overlap: int = 1,
    size_share_cap: float = 0.33,
    loss_budget: float = 0.0,
    quality_floor_frac: float = 0.10,
    max_pool_frac: float = 0.25,
    cooldown: set | None = None,
) -> list[dict]:
    """Pick a fundable, diversified book by RISK-ADJUSTED yield within a budget.

    Selection criteria, in order:
      0. Cooldown: skip any pool (by condition_id or token) in ``cooldown`` — a
         market that just had a catalyst jump is no longer the quiet SAFE pool we
         classified, so don't re-enter it until it has settled.
      1. Eligibility: SAFE + non-empty-band (unless ``require_safe=False``).
      2. Rank by RISK-ADJUSTED yield (``reward_per_day`` discounted by jump-tail
         exposure) — not cheapest-first, not gross-yield-only.
      3. Correlation filter: skip a candidate that is in the same curated topic
         cluster (Fed, a country's politics, …) as an already-selected pool, OR
         shares more than ``max_token_overlap`` distinctive tokens with one
         (same race / event / entity ⇒ correlated ⇒ no real diversification).
      4. Fund greedily down the ranked list until the capital budget runs out.

    Sizing is always yield-weighted: capital flows to quality (size ∝ score),
    capped per pool by ``max_pool_frac`` plus the share/jump limits in
    ``_deploy_size``; a tight budget / zero ``loss_budget`` floors it at each
    pool's ``min_size``. Pool COUNT is governed by the dynamic quality floor +
    budget — there is no hard pool-count cap.
    """
    cd = cooldown or set()
    cands = [
        p for p in scan_report.get("pools", [])
        if p.get("condition_id") not in cd and p.get("token") not in cd
        and (not require_safe or (p.get("jump_verdict") == "SAFE" and not p.get("empty_band")))
    ]
    scored = [(p, _risk_adjusted_score(p, risk_tolerance_days, chop_aversion)) for p in cands]
    scored.sort(key=lambda ps: -ps[1])
    # DYNAMIC quality floor (not a hard pool count): keep funding pools while they're
    # worth at least `quality_floor_frac` of the BEST pool's risk-adjusted score. A
    # clear quality gap -> fund only the few good ones; a flat field of good pools ->
    # fund many. This drops the junk tail so capital isn't tied up in weak pools.
    best = scored[0][1] if scored else 0.0
    cutoff = quality_floor_frac * best if quality_floor_frac > 0 else 0.0
    elig = [(p, s) for p, s in scored if s >= cutoff]
    total_score = sum(s for _, s in elig) or 1.0

    selected: list[dict] = []
    chosen_tokens: list[set[str]] = []
    chosen_clusters: set[str] = set()
    spent = 0.0
    for p, score in elig:
        half_spread_c = p["tick"] * 100.0 * half_spread_ticks
        # YIELD-WEIGHTED allocation: capital flows to quality (size ∝ score), with a
        # per-pool diversification cap (max_pool_frac) so no pool dominates, plus the
        # share/jump caps in _deploy_size. Risk budget scales with the allocation.
        # A tight budget / zero loss_budget floors size at the pool's min_size.
        weight = score / total_score
        target = min(max_pool_frac * capital, capital * weight)
        rp = loss_budget * weight if loss_budget > 0 else 0.0
        size = _deploy_size(p, half_spread_c, target_cap=target,
                            share_cap=size_share_cap, loss_budget=rp)
        share = maker_reward_share(size, half_spread_c, p["max_spread_c"],
                                   p.get("min_side_score") or 0.0)
        cap = committed_capital(size, half_spread_c)
        if cap <= 0 or spent + cap > capital:
            continue
        cluster = _cluster_key(p["question"])
        if cluster is not None and cluster in chosen_clusters:
            continue  # same curated topic/country cluster ⇒ correlated
        toks = _significant_tokens(p["question"])
        if toks and any(len(toks & ct) > max_token_overlap for ct in chosen_tokens):
            continue  # surface-token correlation
        selected.append({
            "question": p["question"],
            "condition_id": p["condition_id"],
            "token": p["token"],
            "daily": p["daily"],
            "share": round(share, 4),
            "min_size": p["min_size"],
            "size": round(size, 2),
            "tick": p["tick"],
            "max_spread_c": p["max_spread_c"],
            "half_spread_c": half_spread_c,
            "committed_capital": round(cap, 2),
            "est_daily_reward": round(share * p["daily"], 4),
            "risk_adj_score": round(score, 4),
        })
        spent += cap
        chosen_tokens.append(toks)
        if cluster is not None:
            chosen_clusters.add(cluster)
    return selected


class MakerPortfolio:
    """A portfolio of two-sided maker quotes across selected pools.

    ``dry_run=True`` (default) plans orders without sending. For LIVE, pass
    ``dry_run=False`` and a real ``submitter`` (e.g. ``build_clob_signer()``);
    each bot then places/cancels real orders through it.
    """

    def __init__(self, selected: list[dict], *, dry_run: bool = True,
                 submitter=None) -> None:
        self.meta: dict[str, dict] = {}
        self.bots: dict[str, LiveMakerBot] = {}
        for s in selected:
            self.meta[s["token"]] = s
            self.bots[s["token"]] = LiveMakerBot(
                token_id=s["token"], max_spread_c=s["max_spread_c"],
                min_size=s.get("size") or s["min_size"], tick=s["tick"],
                half_spread_c=s["half_spread_c"], dry_run=dry_run,
                submitter=submitter, external_fills=not dry_run,
            )

    def committed_capital(self) -> float:
        return round(
            sum(committed_capital(b.size, b.half_spread_c) for b in self.bots.values()), 2
        )

    def est_daily_reward(self) -> float:
        return round(sum(m["est_daily_reward"] for m in self.meta.values()), 4)

    def plan_all(self, market_data: dict) -> list[dict]:
        """Run one quoting step per pool. ``market_data``: {token: (OrderBook, mid)}."""
        plans = []
        for token, bot in self.bots.items():
            md = market_data.get(token)
            if md is None:
                continue
            book, mid = md
            plans.append({"question": self.meta[token]["question"], **bot.step(book, mid)})
        return plans

    def summary(self) -> dict:
        cap = self.committed_capital()
        daily = self.est_daily_reward()
        return {
            "pools": len(self.bots),
            "committed_capital": cap,
            "free_implied": None,
            "est_daily_reward": daily,
            "est_annualized_pct": round(daily * 365 / cap * 100, 1) if cap > 0 else 0.0,
        }


def fetch_market_data(client: RewardsClient, selected: list[dict]) -> dict:
    """Pull a live (OrderBook, mid) for each selected pool's token, for plan_all.

    Returns {token: (OrderBook, mid)}; pools whose book is empty/one-sided or
    errors are skipped (so a single bad market doesn't sink the portfolio).
    """
    from pm_trader.models import OrderBook, OrderBookLevel

    out: dict = {}
    for s in selected:
        token = s["token"]
        try:
            raw = client.book(token)
        except Exception:
            continue
        bids = [OrderBookLevel(price=float(b["price"]), size=float(b["size"]))
                for b in (raw.get("bids") or []) if "price" in b and "size" in b]
        asks = [OrderBookLevel(price=float(a["price"]), size=float(a["size"]))
                for a in (raw.get("asks") or []) if "price" in a and "size" in a]
        if not bids or not asks:
            continue
        mid = (max(b.price for b in bids) + min(a.price for a in asks)) / 2.0
        out[token] = (OrderBook(bids=bids, asks=asks), mid)
    return out


def build_portfolio(
    client: RewardsClient,
    *,
    capital: float = DEFAULT_CAPITAL,
    min_daily: float = 80.0,
    top: int = 40,
) -> dict:
    """Scan live, select a fundable SAFE book, and return its plan + summary (dry-run)."""
    report = scan(client, min_daily=min_daily, top=top, with_jump_risk=True)
    selected = select_pools(report, capital=capital)
    portfolio = MakerPortfolio(selected, dry_run=True)
    market_data = fetch_market_data(client, selected)
    plans = portfolio.plan_all(market_data)
    return {
        "capital_budget": capital,
        "selected": selected,
        "summary": portfolio.summary(),
        "planned": len(plans),
        "plans": plans,
    }


def run(**kwargs: object) -> dict:
    """Convenience wrapper: build a client, build the portfolio, close the client."""
    client = RewardsClient()
    try:
        return build_portfolio(client, **kwargs)  # type: ignore[arg-type]
    finally:
        client.close()
