"""Portfolio selection — turn a scan report into a fundable, diversified book.

Migrated from ``pm_trader/portfolio.py``: the PURE selection logic only (the
paper ``MakerPortfolio``/``fetch_market_data``/``build_portfolio`` orchestration
is dropped — the live runner owns orchestration).

The validated edge is capacity-friendly at small scale: each two-sided
``min_size`` quote earns a few $/day but locks only ~$50-100, so the strategy is
"spread a small book across many uncorrelated SAFE mid-tail pools." ``select_pools``
picks that book by RISK-ADJUSTED yield within a capital budget, decorrelated and
cooldown-aware.
"""

from __future__ import annotations

import re

from live_maker.reward_math import committed_capital

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
# words. Extend as new correlated themes appear in the reward universe.
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
    """Canonical correlation cluster for a question, or None. Catches
    same-event/same-country pairs that share no surface words. Heuristic +
    curated — complements, does not replace, the token-overlap check."""
    q = (question or "").lower()
    for name, keywords in _CLUSTER_RULES:
        if any(kw in q for kw in keywords):
            return name
    return None


def _risk_adjusted_score(p: dict, risk_tolerance_days: float) -> float:
    """Reward per day, discounted by jump-tail exposure.

    ``reward_per_day`` is the steady income; ``days_wiped`` is how many days of it
    one historical max jump erases. Full credit when a jump costs little relative
    to income; shrinks as jump exposure grows — so ranking favours pools that are
    BOTH high-yield AND jump-safe.
    """
    reward = p.get("reward_per_day")
    if reward is None:
        reward = p.get("share", 0.0) * p.get("daily", 0.0)
    days_wiped = p.get("days_wiped")
    if days_wiped is None:
        days_wiped = 9_999.0
    return reward / (1.0 + days_wiped / risk_tolerance_days)


def select_pools(
    scan_report: dict,
    *,
    capital: float = DEFAULT_CAPITAL,
    max_pools: int = 40,
    require_safe: bool = True,
    half_spread_ticks: int = 1,
    risk_tolerance_days: float = 7.0,
    max_token_overlap: int = 1,
    cooldown: set | None = None,
) -> list[dict]:
    """Pick a fundable, diversified book by RISK-ADJUSTED yield within a budget.

    Selection, in order:
      0. Cooldown: skip any pool (by condition_id or token) in ``cooldown`` — a
         market that just had a catalyst jump is no longer the quiet SAFE pool we
         classified, so don't re-enter until it has settled.
      1. Eligibility: SAFE + non-empty-band (unless ``require_safe=False``).
      2. Rank by RISK-ADJUSTED yield (reward_per_day discounted by jump-tail
         exposure) — not cheapest-first, not gross-yield-only.
      3. Correlation filter: skip a candidate in the same curated topic cluster
         as an already-selected pool, OR sharing more than ``max_token_overlap``
         distinctive tokens with one.
      4. Fund greedily down the ranked list until the capital budget or
         ``max_pools`` is hit.
    """
    cd = cooldown or set()
    cands = [
        p for p in scan_report.get("pools", [])
        if p.get("condition_id") not in cd and p.get("token") not in cd
        and (not require_safe or (p.get("jump_verdict") == "SAFE" and not p.get("empty_band")))
    ]
    cands.sort(key=lambda p: -_risk_adjusted_score(p, risk_tolerance_days))

    selected: list[dict] = []
    chosen_tokens: list[set[str]] = []
    chosen_clusters: set[str] = set()
    spent = 0.0
    for p in cands:
        half_spread_c = p["tick"] * 100.0 * half_spread_ticks
        cap = committed_capital(p["min_size"], half_spread_c)
        if cap <= 0 or spent + cap > capital:
            continue
        cluster = _cluster_key(p["question"])
        if cluster is not None and cluster in chosen_clusters:
            continue  # same curated topic/country cluster => correlated
        toks = _significant_tokens(p["question"])
        if toks and any(len(toks & ct) > max_token_overlap for ct in chosen_tokens):
            continue  # surface-token correlation
        selected.append({
            "question": p["question"],
            "condition_id": p["condition_id"],
            "token": p["token"],
            "daily": p["daily"],
            "share": p["share"],
            "min_size": p["min_size"],
            "tick": p["tick"],
            "max_spread_c": p["max_spread_c"],
            "half_spread_c": half_spread_c,
            "committed_capital": round(cap, 2),
            "est_daily_reward": round(p["share"] * p["daily"], 4),
            "risk_adj_score": round(_risk_adjusted_score(p, risk_tolerance_days), 4),
        })
        spent += cap
        chosen_tokens.append(toks)
        if cluster is not None:
            chosen_clusters.add(cluster)
        if len(selected) >= max_pools:
            break
    return selected
