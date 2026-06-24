"""Liquidity-rewards maker math — PURE functions, migrated verbatim from the
validated paper build (``pm_trader/orderbook.py``). No I/O, no state.

Polymarket pays a fixed daily USDC pool, from its own treasury, to resting limit
orders quoted within ``max_spread`` cents of the midpoint (two-sided,
size-weighted by ``((c - s) / c) ** 2`` where ``s`` is the order's distance from
mid in cents and ``c`` is ``max_spread``). A maker's reward share is its
binding-side (lighter of bid/ask) score over the total in-band score.

Net maker P&L = reward accrual - adverse bleed. The validated edge (see
``docs/research/04-lp-rewards-edge.md``) is that low-catalyst mid-tail pools net
positive while deep marquee pools are a kill (one jump wipes weeks of reward).
"""

from __future__ import annotations

import math

from live_maker.models import OrderBook


# ---------------------------------------------------------------------------
# In-band reward scoring
# ---------------------------------------------------------------------------

def _inband_weight(s_cents: float, max_spread_c: float) -> float:
    """PM size-weight ``((c - s) / c) ** 2`` for an order ``s`` cents from mid.

    Returns 0 when the order is outside the band (``s < 0`` means it crosses the
    mid, ``s > c`` means it is too wide) or when ``max_spread_c <= 0``.
    """
    if max_spread_c <= 0:
        return 0.0
    if s_cents < -1e-9 or s_cents > max_spread_c + 1e-9:
        return 0.0
    return ((max_spread_c - s_cents) / max_spread_c) ** 2


def book_inband_qmin(book: OrderBook, mid: float, max_spread_c: float) -> float:
    """Binding-side (min of bid/ask) in-band reward score of the live book.

    Sums each side's ``size * weight`` over the levels within ``max_spread_c`` of
    ``mid``, then returns the lighter side — the competing makers' Qmin, used as
    the denominator term when estimating our own reward share.
    """
    bid_score = sum(
        lvl.size * _inband_weight((mid - lvl.price) * 100.0, max_spread_c)
        for lvl in book.bids
    )
    ask_score = sum(
        lvl.size * _inband_weight((lvl.price - mid) * 100.0, max_spread_c)
        for lvl in book.asks
    )
    return min(bid_score, ask_score)


def maker_quote_score(size: float, half_spread_c: float, max_spread_c: float) -> float:
    """Binding-side reward score of our own two-sided quote.

    Both sides rest ``half_spread_c`` cents from mid with ``size`` shares, so the
    bid and ask scores are equal and the binding (min) score is just one side.
    A quote wider than ``max_spread_c`` scores 0 (earns no reward).
    """
    return size * _inband_weight(half_spread_c, max_spread_c)


def maker_reward_share(
    size: float, half_spread_c: float, max_spread_c: float, existing_qmin: float
) -> float:
    """Our share of the daily pool ~= ``our Qmin / (our Qmin + existing Qmin)``.

    Returns 0 if our quote is out of band (score 0); returns 1 against an empty
    in-band book (``existing_qmin == 0``).
    """
    mine = maker_quote_score(size, half_spread_c, max_spread_c)
    denom = mine + existing_qmin
    return (mine / denom) if denom > 0 else 0.0


def reward_accrual(share: float, daily_rate: float, seconds: float) -> float:
    """USDC reward for resting in-band for ``seconds`` at a given pool share."""
    if share <= 0 or daily_rate <= 0 or seconds <= 0:
        return 0.0
    return share * daily_rate * (seconds / 86_400.0)


def adverse_bleed(
    size: float,
    half_spread_c: float,
    mid_prev: float,
    mid_now: float,
    cancel_efficiency: float = 0.0,
) -> float:
    """Per-poll adverse-selection loss for a re-centering (no-inventory) maker.

    A move within ``half_spread_c`` of mid is harmless; a larger move fills the
    stale side at its quote price before the re-center, costing
    ``size * (|move| - half_spread) * (1 - cancel_efficiency)``.
    """
    offset = half_spread_c / 100.0
    excess = abs(mid_now - mid_prev) - offset
    if excess <= 0:
        return 0.0
    return size * excess * max(0.0, 1.0 - cancel_efficiency)


def skewed_center(
    mid: float, inventory: float, size: float, half_spread_c: float,
    skew_strength: float,
) -> float:
    """Inventory-skewed quote center — lean away from inventory to flatten it.

    ``center = mid - skew_strength * (inventory/size) * offset`` (offset in price).
    Long (inventory > 0) -> center DOWN, so the ask is keener to be lifted (sell,
    get flat) and the bid less keen to be hit; short -> center UP. This is the
    Avellaneda-Stoikov / Ho-Stoll reservation-price idea: quote around an
    inventory-adjusted fair value so the book mean-reverts toward flat.
    """
    offset = half_spread_c / 100.0
    return mid - skew_strength * (inventory / size) * offset if size > 0 else mid


def committed_capital(size: float, half_spread_c: float) -> float:
    """Cash locked by a two-sided ``size`` quote (a YES bid + a NO bid).

    bid notional + ask notional = ``size*(mid - s) + size*(1 - mid - s)``
    ``= size * (1 - 2s)`` — independent of mid (``s`` in price units). Clamped
    to 0 for degenerate quotes wider than 50c per side.
    """
    cap = size * (1.0 - 2.0 * (half_spread_c / 100.0))
    return cap if cap > 0 else 0.0


# ---------------------------------------------------------------------------
# Volatility-aware optimal quoting (Avellaneda-Stoikov, adapted to the subsidy)
# ---------------------------------------------------------------------------
#
# Classic MM widens the spread with volatility to dodge adverse selection. Our
# case is INVERTED by the reward subsidy — PM pays a share that grows
# QUADRATICALLY as the quote tightens (((c-s)/c)^2), a force pulling us tighter
# that classic MM lacks. The optimal offset balances reward(s) (rises as s -> 0)
# against bleed(s) (also rises as s -> 0, scaling with volatility, shrunk by
# cancel_efficiency). We grid-search s in [tick, max_spread] for the net max.


def expected_excess_move(sigma: float, offset: float) -> float:
    """E[max(|d| - offset, 0)] for a zero-mean Gaussian move with std ``sigma``.

    Closed form for ``X ~ N(0, sigma^2)`` and ``offset >= 0``::

        E[(|X| - a)+] = 2*sigma*phi(a/sigma) - 2*a*(1 - Phi(a/sigma))

    (phi = standard-normal PDF, Phi = its CDF). The per-period adverse move
    beyond a quote resting ``offset`` from mid — the bleed driver. ``sigma`` and
    ``offset`` share units (price or cents). Returns 0 for non-positive sigma.
    """
    if sigma <= 0:
        return 0.0
    a = offset / sigma
    phi = math.exp(-0.5 * a * a) / math.sqrt(2.0 * math.pi)
    cdf = 0.5 * (1.0 + math.erf(a / math.sqrt(2.0)))
    return max(0.0, 2.0 * sigma * phi - 2.0 * offset * (1.0 - cdf))


def realized_sigma_c_from_history(history: list[dict], poll_seconds: float) -> float:
    """Per-poll mid-move volatility (cents) from CLOB ``prices-history`` points.

    Estimates the std of consecutive mid moves at the history's own cadence
    (median timestamp gap), then scales to the ``poll_seconds`` re-quote interval
    by random-walk sqrt-time scaling. Returns 0.0 when the path or cadence is
    degenerate (treated as no measured risk -> recommends the tightest quote).
    """
    prices: list[float] = []
    ts: list[float] = []
    for pt in history:
        try:
            prices.append(float(pt["p"]))
            ts.append(float(pt["t"]))
        except (KeyError, TypeError, ValueError):
            continue
    if len(prices) < 2 or poll_seconds <= 0:
        return 0.0
    gaps = sorted(ts[i] - ts[i - 1] for i in range(1, len(ts)) if ts[i] > ts[i - 1])
    if not gaps:
        return 0.0
    step = gaps[len(gaps) // 2]  # all gaps are positive by construction
    diffs_c = [(prices[i] - prices[i - 1]) * 100.0 for i in range(1, len(prices))]
    n = len(diffs_c)
    mean = sum(diffs_c) / n
    var = sum((d - mean) ** 2 for d in diffs_c) / n
    return (var ** 0.5) * (poll_seconds / step) ** 0.5


def optimal_half_spread(
    *,
    daily_rate: float,
    max_spread_c: float,
    min_size: float,
    tick_c: float,
    existing_qmin: float,
    sigma_c: float,
    periods_per_day: float,
    cancel_efficiency: float = 0.0,
    grid: int = 200,
) -> dict:
    """Grid-search the half-spread (cents) that maximises net daily maker yield.

    ``net(s) = reward(s) - bleed(s)`` where
    ``reward(s) = daily_rate * own(s)/(own(s)+existing_qmin)`` with
    ``own(s) = min_size*((c-s)/c)^2``, and
    ``bleed(s) = (1-eff)*min_size*E[(|d|-s)+]*periods_per_day`` for
    ``d ~ N(0, sigma_c^2)``. Searches ``s in [tick_c, max_spread_c]`` and returns
    the best offset plus its net/reward/bleed/share. With ``sigma_c = 0`` the
    bleed term vanishes and the tightest quote (``tick_c``) wins.
    """
    lo, hi = tick_c, max_spread_c
    if grid < 1:
        grid = 1
    if hi <= lo:
        candidates = [lo]
    else:
        step = (hi - lo) / grid
        candidates = [lo + i * step for i in range(grid + 1)]

    best: dict | None = None
    for s in candidates:
        own = maker_quote_score(min_size, s, max_spread_c)
        denom = own + existing_qmin
        share = (own / denom) if denom > 0 else 0.0
        reward = share * daily_rate
        bleed = (
            max(0.0, 1.0 - cancel_efficiency)
            * min_size
            * (expected_excess_move(sigma_c, s) / 100.0)
            * periods_per_day
        )
        net = reward - bleed
        if best is None or net > best["net_per_day"]:
            best = {
                "half_spread_c": round(s, 4),
                "net_per_day": round(net, 4),
                "reward_per_day": round(reward, 4),
                "bleed_per_day": round(bleed, 4),
                "share": round(share, 4),
            }
    return best
