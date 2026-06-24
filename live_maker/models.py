"""Lean dataclasses + error types for the live maker.

Only what the live runtime needs: a normalized order book (the pure reward math
and the strategy both consume it), the decision objects the strategy emits, and
the normalized stream events the feed emits. No paper account / position / trade
abstractions — live P&L comes from REAL fills on the WS user channel.
"""

from __future__ import annotations

from dataclasses import dataclass, field


# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------

class LiveMakerError(Exception):
    """Base error."""


class ApiError(LiveMakerError):
    """A public-CLOB / SDK request failed."""

    def __init__(self, message: str, status_code: int | None = None) -> None:
        super().__init__(message)
        self.status_code = status_code


# ---------------------------------------------------------------------------
# Order book (normalized; the single book shape the hot path consumes)
# ---------------------------------------------------------------------------

@dataclass
class OrderBookLevel:
    price: float
    size: float


@dataclass
class OrderBook:
    bids: list[OrderBookLevel] = field(default_factory=list)
    asks: list[OrderBookLevel] = field(default_factory=list)

    def best_bid(self) -> float | None:
        return max((lvl.price for lvl in self.bids), default=None)

    def best_ask(self) -> float | None:
        return min((lvl.price for lvl in self.asks), default=None)

    def midpoint(self) -> float | None:
        bb, ba = self.best_bid(), self.best_ask()
        if bb is None or ba is None:
            return None
        return (bb + ba) / 2.0


# Book coercion from raw SDK/CLOB shapes lives in ``execution._normalize_book``.


# ---------------------------------------------------------------------------
# Strategy decision (what a per-pool maker wants done this event)
# ---------------------------------------------------------------------------

# action values
QUOTE = "QUOTE"   # cancel stale (if any) + post the two-sided quote in `orders`
HOLD = "HOLD"     # quote unchanged; do nothing
EXIT = "EXIT"     # cancel everything and stand down (rewards ended / jump / kill)
IDLE = "IDLE"     # already exited; on cooldown; do nothing


@dataclass
class Decision:
    """One pool's decision for one event. The runner translates it into SDK calls."""

    token_id: str
    action: str
    mid: float
    orders: list[dict] = field(default_factory=list)   # [{side, price, size}, ...]
    cancel_first: bool = False                          # cancel resting orders before (re)quote
    reason: str = ""                                    # e.g. "jump", "rewards_ended"
    cooldown: bool = False                              # flag the pool for cooldown on exit
    est_share: float = 0.0
    inventory: float = 0.0
    skew_ticks: float = 0.0
    committed_capital: float = 0.0


# ---------------------------------------------------------------------------
# Normalized stream events (the feed flattens SDK events into these)
# ---------------------------------------------------------------------------

@dataclass
class BookEvent:
    """A fresh full book (from WS book snapshot OR the REST fallback poll)."""

    token_id: str
    book: OrderBook
    source: str = "ws"   # "ws" | "rest" (rest = the order-book failsafe poll)


@dataclass
class PriceChangeEvent:
    """A mid/price move without a full book refresh."""

    token_id: str
    mid: float
    source: str = "ws"


@dataclass
class FillEvent:
    """A REAL fill on the operator's wallet (WS user channel)."""

    token_id: str
    side: str          # "BUY" | "SELL"
    size: float
    price: float
    order_id: str = ""
