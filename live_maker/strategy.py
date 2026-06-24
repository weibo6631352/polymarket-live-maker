"""Per-pool quoting/risk DECISIONS for one reward pool.

Migrated from ``pm_trader/maker_live.py`` (the decision logic — ``compute_two_sided_quotes``
with skew, inventory tracking + skew, jump-halt + cooldown, re-quote logic) and
folded in the reconcile + jump-exit decisions that lived in
``engine.accrue_maker_rewards`` (re-check the reward config each loop and exit if
the pool left the program / resolved; exit on a move beyond the band).

KEY LIVE DIFFERENCE vs the paper engine: inventory is built from REAL fills on
the WS user channel (``on_fill``), NOT simulated by inferring mid-crossings. The
adverse-bleed / fill-simulation math (in ``reward_math``) is therefore only used
offline for sizing (``optimal_half_spread``); the live loop reacts to actual fills.

This module is PURE decision logic: it computes a :class:`Decision` and never
touches the network. ``execution``/``runner`` translate decisions into SDK calls.
"""

from __future__ import annotations

from live_maker.models import EXIT, HOLD, IDLE, QUOTE, Decision, OrderBook
from live_maker.reward_math import (
    book_inband_qmin,
    committed_capital,
    maker_reward_share,
)


def compute_two_sided_quotes(
    mid: float,
    *,
    half_spread_c: float,
    size: float,
    tick: float,
    max_spread_c: float,
    skew_ticks: float = 0.0,
) -> list[dict]:
    """Return the two resting orders (a YES bid + a YES ask) to quote at ``mid``.

    Quotes rest ``half_spread_c`` cents either side of mid, rounded to ``tick``,
    clamped into [tick, 1-tick]. Raises if the offset is outside the reward band.

    ``skew_ticks`` shifts BOTH quotes by that many ticks to work inventory back to
    flat ("using the other side"): when long (positive skew) both quotes move DOWN
    — the ask nears mid (sells eagerly to shed the long) while the bid backs off —
    and symmetrically up when short.
    """
    if not 0.0 < mid < 1.0:
        raise ValueError(f"mid must be in (0, 1), got {mid}")
    if half_spread_c <= 0 or half_spread_c > max_spread_c:
        raise ValueError(
            f"half_spread_c must be in (0, {max_spread_c}], got {half_spread_c}"
        )
    offset = half_spread_c / 100.0
    shift = skew_ticks * tick
    bid = max(tick, round((mid - offset - shift) / tick) * tick)
    ask = min(1.0 - tick, round((mid + offset - shift) / tick) * tick)
    return [
        {"side": "BUY", "price": round(bid, 4), "size": size},
        {"side": "SELL", "price": round(ask, 4), "size": size},
    ]


def plan_requote(
    mid_prev: float, mid_now: float, *, half_spread_c: float, tick: float
) -> bool:
    """Whether the mid moved enough to warrant cancelling and re-centering.

    A move of at least one tick means a resting quote is now off-center and (if
    the move is toward a side) at risk of being picked off — re-quote.
    """
    return abs(mid_now - mid_prev) >= tick


class PoolMaker:
    """Decision engine for one reward pool. Stateless w.r.t. the network.

    Lifecycle: ``decide(book, mid, daily_rate)`` each market event returns a
    :class:`Decision`; ``on_fill(side, size)`` updates inventory when the wallet's
    WS user channel reports a real fill. Once exited (``halted``) it returns
    ``IDLE`` until the runner drops it (cooldown).
    """

    def __init__(
        self,
        *,
        token_id: str,
        max_spread_c: float,
        min_size: float,
        tick: float,
        condition_id: str = "",
        question: str = "",
        half_spread_c: float | None = None,
        size: float | None = None,
        skew_strength_ticks: float = 2.0,
        max_inventory: float | None = None,
        jump_exit_ticks: float | None = None,
    ) -> None:
        self.token_id = token_id
        self.condition_id = condition_id
        self.question = question
        self.max_spread_c = max_spread_c
        self.min_size = min_size
        self.tick = tick
        self.half_spread_c = (tick * 100.0) if half_spread_c is None else half_spread_c
        self.size = min_size if size is None else size
        self.skew_strength_ticks = skew_strength_ticks
        self.max_inventory = (
            max_inventory if max_inventory is not None else 5.0 * self.size
        )
        # A move beyond the reward band is a catalyst jump, not normal drift.
        self.jump_exit_ticks = (
            (max_spread_c / (tick * 100.0)) if jump_exit_ticks is None else jump_exit_ticks
        )
        # state
        self.inventory = 0.0
        self.entry_mid: float | None = None
        self.last_mid: float | None = None   # previous event mid (jump detection)
        self.quote_mid: float | None = None  # mid our resting quote is centered on
        self.last_bid: float | None = None
        self.last_ask: float | None = None
        self.halted = False
        self.exit_reason = ""

    # -- inventory (from REAL fills) ----------------------------------------

    def on_fill(self, side: str, size: float) -> None:
        """Apply a real fill to inventory (BUY -> long YES, SELL -> short YES),
        clamped to the position cap."""
        if side.upper() == "BUY":
            self.inventory += size
        else:
            self.inventory -= size
        if self.max_inventory > 0:
            self.inventory = max(
                -self.max_inventory, min(self.max_inventory, self.inventory)
            )

    def _skew_ticks(self) -> float:
        """Quote shift (ticks) to pull inventory toward flat; 0 when flat.

        Positive (shift quotes DOWN) when long, negative (UP) when short, scaled
        by how close inventory is to its cap and clamped at ``skew_strength_ticks``.
        """
        if self.max_inventory <= 0:
            return 0.0
        ratio = max(-1.0, min(1.0, self.inventory / self.max_inventory))
        return self.skew_strength_ticks * ratio

    # -- the decision -------------------------------------------------------

    def _exit(self, mid: float, reason: str, *, cooldown: bool) -> Decision:
        self.halted = True
        self.exit_reason = reason
        return Decision(
            token_id=self.token_id, action=EXIT, mid=mid, cancel_first=True,
            reason=reason, cooldown=cooldown, inventory=round(self.inventory, 4),
        )

    def decide(self, book: OrderBook, mid: float, *, daily_rate: float) -> Decision:
        """Return this pool's decision for the current (book, mid).

        Order of checks each event:
          1. RECONCILE — if the pool stopped paying (rewards ended / resolved)
             cancel and stand down (no cooldown; it's gone, not jumpy).
          2. If already halted -> IDLE (the runner will retire it on cooldown).
          3. JUMP / DRIFT — a per-event move beyond the band, or cumulative drift
             a full band from entry, means the regime changed: cancel, flag for
             cooldown, recommend exit (the runner flattens the inventory).
          4. Otherwise (re)quote, skewed by inventory; HOLD if the resting quote
             is still centered (mid hasn't moved a tick since we last quoted).
        """
        # 1. reconcile against the live program
        if daily_rate <= 0:
            return self._exit(mid, "rewards_ended", cooldown=False)

        # 2. already out
        if self.halted:
            return Decision(token_id=self.token_id, action=IDLE, mid=mid,
                            reason=self.exit_reason, inventory=round(self.inventory, 4))

        if self.entry_mid is None:
            self.entry_mid = mid

        # 3. jump (per-event) / drift (from entry) detection
        moved = abs(mid - self.last_mid) if self.last_mid is not None else 0.0
        self.last_mid = mid
        if moved >= self.jump_exit_ticks * self.tick:
            return self._exit(mid, "jump", cooldown=True)
        if abs(mid - self.entry_mid) >= self.max_spread_c / 100.0:
            return self._exit(mid, "drift_exit", cooldown=True)

        # 4. normal two-sided quote, skewed to work inventory flat
        skew = self._skew_ticks()
        orders = compute_two_sided_quotes(
            mid, half_spread_c=self.half_spread_c, size=self.size,
            tick=self.tick, max_spread_c=self.max_spread_c, skew_ticks=skew,
        )
        existing_qmin = book_inband_qmin(book, mid, self.max_spread_c)
        share = maker_reward_share(
            self.size, self.half_spread_c, self.max_spread_c, existing_qmin
        )
        cap = committed_capital(self.size, self.half_spread_c)

        requote = self.quote_mid is None or plan_requote(
            self.quote_mid, mid, half_spread_c=self.half_spread_c, tick=self.tick
        )
        if not requote:
            return Decision(
                token_id=self.token_id, action=HOLD, mid=mid, orders=orders,
                est_share=round(share, 4), inventory=round(self.inventory, 4),
                skew_ticks=round(skew, 4), committed_capital=round(cap, 2),
            )

        self.quote_mid = mid
        self.last_bid = orders[0]["price"]
        self.last_ask = orders[1]["price"]
        return Decision(
            token_id=self.token_id, action=QUOTE, mid=mid, orders=orders,
            cancel_first=True, est_share=round(share, 4),
            inventory=round(self.inventory, 4), skew_ticks=round(skew, 4),
            committed_capital=round(cap, 2),
        )
