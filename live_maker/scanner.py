"""Liquidity-rewards pool scanner — surface Polymarket's reward pools and rank
them by net-yield potential. Migrated from ``pm_trader/rewards.py``; the scoring
and jump-risk functions are unchanged pure logic, the HTTP client is now async
(``httpx.AsyncClient``) so discovery never blocks the event loop.

Two surfaces:
  - ``AsyncRewardsClient``: a thin async reader for the PUBLIC CLOB endpoints
    ``/sampling-markets`` (reward markets + config), ``/book`` (live depth),
    ``/prices-history`` (jump-risk) and ``/markets/{condition_id}`` (reconcile a
    pool's current daily rate). No auth — the SDK doesn't need to wrap these.
  - ``scan``: pull every reward pool, estimate the gross reward yield of posting
    ``min_size`` two-sided one tick inside the band, and jump-filter (a single
    historical jump that wipes many days of reward marks a deferred-jump trap).

The scanner never places orders; it surfaces high-yield, low-jump-risk pools.
"""

from __future__ import annotations

import asyncio

import httpx

from live_maker.models import ApiError

CLOB_BASE = "https://clob.polymarket.com"

_TIMEOUT = httpx.Timeout(15.0)

# Defaults (overridable per scan)
MIN_DAILY = 50.0       # ignore dust pools below this daily reward rate (USD)
JUMP_KILL_DAYS = 20.0  # one historical max jump wiping > this many days = KILL
JUMP_WATCH_DAYS = 7.0  # ... > this many days = WATCH
EMPTY_BAND_SHARE = 0.99  # share at/above this came from an empty band -> unstable

# bound concurrent per-pool book/history fetches during a scan
_SCAN_CONCURRENCY = 8


# ---------------------------------------------------------------------------
# Pure scoring (no I/O)
# ---------------------------------------------------------------------------

def parse_rewards(market: dict) -> dict | None:
    """Extract a pool's reward config from a CLOB market dict.

    Returns ``{daily, max_spread, min_size, tick, token, question, condition_id}``
    or ``None`` if the market has no positive daily reward rate or no token.
    """
    rw = market.get("rewards") or {}
    rates = rw.get("rates") or []
    daily = 0.0
    for rt in rates:
        try:
            daily += float(rt.get("rewards_daily_rate", 0) or 0)
        except (TypeError, ValueError):
            continue
    if daily <= 0:
        return None
    token = None
    for tok in market.get("tokens") or []:
        tid = tok.get("token_id")
        if tid:
            token = tid
            break
    if not token:
        return None
    try:
        max_spread = float(rw.get("max_spread", 0) or 0)
        min_size = float(rw.get("min_size", 0) or 0)
        tick = float(market.get("minimum_tick_size", 0.01) or 0.01)
    except (TypeError, ValueError):
        return None
    return {
        "daily": daily,
        "max_spread": max_spread,
        "min_size": min_size,
        "tick": tick,
        "token": token,
        "question": (market.get("question") or "")[:80],
        "condition_id": market.get("condition_id", ""),
    }


def inband_score(
    levels: list[dict], mid: float, max_spread_cents: float, is_bid: bool
) -> tuple[float, float]:
    """Sum the PM reward score and collateral notional of in-band order levels.

    Each level within ``max_spread_cents`` of ``mid`` contributes
    ``size * ((c - s) / c) ** 2`` to the score (``s`` = distance from mid in
    cents, ``c`` = ``max_spread_cents``) and ``size * price`` (bid) or
    ``size * (1 - price)`` (ask) to the notional.
    """
    c = max_spread_cents
    score = 0.0
    notional = 0.0
    for lvl in levels:
        try:
            price = float(lvl["price"])
            size = float(lvl["size"])
        except (KeyError, TypeError, ValueError):
            continue
        s_cents = ((mid - price) if is_bid else (price - mid)) * 100.0
        if -1e-9 <= s_cents <= c + 1e-9:
            w = ((c - s_cents) / c) ** 2 if c > 0 else 0.0
            score += size * w
            notional += size * (price if is_bid else (1.0 - price))
    return score, notional


def reward_share(
    min_size: float,
    tick: float,
    max_spread_cents: float,
    existing_min_side_score: float,
) -> float:
    """Estimate a maker's reward share for posting ``min_size`` one tick in-band.

    The two-sided requirement makes the binding score the lighter side, so the
    maker's share ~= ``my_one_side_score / (my_one_side_score + existing)``.
    """
    c = max_spread_cents
    s_cents = tick * 100.0
    my_w = ((c - s_cents) / c) ** 2 if c > 0 else 0.0
    my_score = min_size * my_w
    denom = my_score + existing_min_side_score
    return (my_score / denom) if denom > 0 else 0.0


def classify_jump_risk(
    prices: list[float],
    reward_per_day: float,
    min_size: float,
    *,
    kill_days: float = JUMP_KILL_DAYS,
    watch_days: float = JUMP_WATCH_DAYS,
) -> dict:
    """Score a pool's deferred-jump risk from its daily price history.

    A maker holding ``min_size`` gets the stale side picked off on a jump,
    losing about ``min_size * max_daily_jump``. Express that loss in days of
    reward accrual: ``> kill_days`` = KILL, ``> watch_days`` = WATCH, else SAFE.
    """
    if len(prices) < 10:
        return {"verdict": "no-history", "days": len(prices)}
    moves = [abs(prices[i] - prices[i - 1]) for i in range(1, len(prices))]
    max_jump = max(moves)
    mean = sum(moves) / len(moves)
    vol = (sum((m - mean) ** 2 for m in moves) / len(moves)) ** 0.5
    jump_loss = min_size * max_jump
    days_wiped = (jump_loss / reward_per_day) if reward_per_day > 0 else float("inf")
    if days_wiped > kill_days:
        verdict = "KILL"
    elif days_wiped > watch_days:
        verdict = "WATCH"
    else:
        verdict = "SAFE"
    return {
        "verdict": verdict,
        "days": len(prices),
        "max_jump_c": round(max_jump * 100, 2),
        "daily_vol_c": round(vol * 100, 2),
        "days_wiped": round(days_wiped, 1) if days_wiped != float("inf") else None,
    }


def _best(levels: list[dict], *, is_bid: bool) -> float | None:
    """Best (highest bid / lowest ask) price from a book side, or None."""
    prices = []
    for lvl in levels:
        try:
            prices.append(float(lvl["price"]))
        except (KeyError, TypeError, ValueError):
            continue
    if not prices:
        return None
    return max(prices) if is_bid else min(prices)


def score_pool(pool: dict, book: dict, history: list[dict]) -> dict | None:
    """Score one reward pool's gross yield + jump risk from its book and history.

    Returns a structured pool report, or ``None`` if the book is one-sided/empty
    (cannot quote two-sided, so not a candidate).
    """
    bids = book.get("bids") or []
    asks = book.get("asks") or []
    best_bid = _best(bids, is_bid=True)
    best_ask = _best(asks, is_bid=False)
    if best_bid is None or best_ask is None:
        return None
    mid = (best_bid + best_ask) / 2.0
    c = pool["max_spread"]
    bscore, bnot = inband_score(bids, mid, c, True)
    ascore, anot = inband_score(asks, mid, c, False)
    share = reward_share(pool["min_size"], pool["tick"], c, min(bscore, ascore))
    capital = pool["min_size"]  # two-sided min_size locks ~= min_size dollars
    reward_per_day = share * pool["daily"]
    gross_ann = (reward_per_day * 365 / capital * 100) if capital > 0 else 0.0

    prices = []
    for pt in history:
        try:
            prices.append(float(pt["p"]))
        except (KeyError, TypeError, ValueError):
            continue
    jump = classify_jump_risk(prices, reward_per_day, pool["min_size"])

    empty_band = share >= EMPTY_BAND_SHARE
    return {
        "question": pool["question"],
        "condition_id": pool["condition_id"],
        "token": pool["token"],
        "daily": round(pool["daily"], 2),
        "max_spread_c": c,
        "min_size": pool["min_size"],
        "tick": pool["tick"],
        "mid": round(mid, 4),
        "spread_c": round((best_ask - best_bid) * 100, 2),
        "inband_notional": round(bnot + anot),
        "share": round(share, 4),
        "empty_band": empty_band,
        "reward_per_day": round(reward_per_day, 2),
        "gross_ann_pct": round(gross_ann),
        "jump_verdict": jump.get("verdict"),
        "max_jump_c": jump.get("max_jump_c"),
        "days_wiped": jump.get("days_wiped"),
    }


# ---------------------------------------------------------------------------
# Async HTTP reader (public CLOB, no auth)
# ---------------------------------------------------------------------------

class AsyncRewardsClient:
    """Async reader for the public CLOB reward-pool/book/history endpoints."""

    def __init__(self, http: httpx.AsyncClient | None = None) -> None:
        self._http = http if http is not None else httpx.AsyncClient(timeout=_TIMEOUT)
        self._owns_http = http is None

    async def __aenter__(self) -> "AsyncRewardsClient":
        return self

    async def __aexit__(self, *exc: object) -> None:
        await self.close()

    async def close(self) -> None:
        if self._owns_http:
            await self._http.aclose()

    async def _get(self, url: str, params: dict | None = None) -> list | dict:
        try:
            resp = await self._http.get(url, params=params)
            resp.raise_for_status()
            return resp.json()
        except httpx.HTTPStatusError as e:
            raise ApiError(
                f"Polymarket CLOB API error: {e.response.status_code} "
                f"{e.response.text[:200]}",
                status_code=e.response.status_code,
            ) from e
        except httpx.RequestError as e:
            raise ApiError(f"Polymarket CLOB API request failed: {e}") from e

    async def sampling_markets(self, *, max_pages: int = 100) -> list[dict]:
        """Fetch ALL reward-enabled markets, following pagination.

        Up to ~1000 per page with a ``next_cursor``; discovery must NOT stop at
        page 1 (that misses the bulk of high-reward pools). Follows the cursor
        until it ends (``""``/``"LTE="``) or repeats; ``max_pages`` is a runaway
        safety bound, not an opportunity cap.
        """
        out: list[dict] = []
        cursor = ""
        for _ in range(max_pages):
            params = {"next_cursor": cursor} if cursor else None
            data = await self._get(f"{CLOB_BASE}/sampling-markets", params=params)
            page = data.get("data", []) if isinstance(data, dict) else data
            if not isinstance(page, list) or not page:
                break
            out.extend(page)
            nxt = data.get("next_cursor", "") if isinstance(data, dict) else ""
            if nxt in ("", "LTE=") or nxt == cursor:
                break
            cursor = nxt
        return out

    async def book(self, token_id: str) -> dict:
        """Fetch the live order book for a CLOB token."""
        data = await self._get(f"{CLOB_BASE}/book", params={"token_id": token_id})
        return data if isinstance(data, dict) else {}

    async def prices_history(
        self, token_id: str, *, interval: str = "max", fidelity: int = 1440
    ) -> list[dict]:
        """Fetch price history points ``[{t, p}, ...]`` for a CLOB token."""
        data = await self._get(
            f"{CLOB_BASE}/prices-history",
            params={"market": token_id, "interval": interval, "fidelity": fidelity},
        )
        if isinstance(data, dict):
            return data.get("history", []) or []
        return []

    async def reward_config(self, condition_id: str) -> dict | None:
        """Current reward config for one market (``/markets/{condition_id}``), or
        ``None`` if it pays no rewards. Used by the runner's periodic reconcile —
        a pool that has left the program / resolved returns ``None`` (or daily<=0).
        """
        data = await self._get(f"{CLOB_BASE}/markets/{condition_id}")
        if not isinstance(data, dict):
            return None
        return parse_rewards(data)


async def scan(
    client: AsyncRewardsClient,
    *,
    min_daily: float = MIN_DAILY,
    top: int = 30,
    with_jump_risk: bool = True,
) -> dict:
    """Scan the liquidity-rewards program and rank pools by net-yield potential.

    Pulls ``/sampling-markets``, keeps pools with daily rate >= ``min_daily``,
    fetches each pool's live book (and price history for jump-risk) CONCURRENTLY,
    scores the gross reward yield of a two-sided ``min_size`` quote, and ranks
    SAFE pools first. Empty-band pools (share ~= 1) are flagged + de-prioritised.
    """
    markets = await client.sampling_markets()
    pools: list[dict] = []
    for m in markets:
        p = parse_rewards(m)
        if p is not None and p["daily"] >= min_daily:
            pools.append(p)
    pools.sort(key=lambda p: -p["daily"])
    pools = pools[: max(1, top)]

    sem = asyncio.Semaphore(_SCAN_CONCURRENCY)

    async def _score_one(p: dict) -> dict | None:
        async with sem:
            try:
                book = await client.book(p["token"])
            except ApiError:
                return None
            history: list[dict] = []
            if with_jump_risk:
                try:
                    history = await client.prices_history(p["token"])
                except ApiError:
                    history = []
        return score_pool(p, book, history)

    results = await asyncio.gather(*(_score_one(p) for p in pools))
    scored = [r for r in results if r is not None]

    # rank: SAFE first, then realistic (non-empty-band) high gross yield
    verdict_order = {"SAFE": 0, "WATCH": 1, "no-history": 2, "KILL": 3, None: 4}
    scored.sort(
        key=lambda r: (
            r["empty_band"],
            verdict_order.get(r["jump_verdict"], 9),
            -r["gross_ann_pct"],
        )
    )
    safe = [r for r in scored if r["jump_verdict"] == "SAFE" and not r["empty_band"]]
    return {
        "params": {"min_daily": min_daily, "top": top, "with_jump_risk": with_jump_risk},
        "total_reward_pools": len([m for m in markets if parse_rewards(m)]),
        "pools_scored": len(scored),
        "safe_count": len(safe),
        "pools": scored,
    }
