"""The autonomous loop — one async process, WebSocket-driven, no LLM in the hot path.

  discover (periodic full reward-universe scan)
    -> select (risk-adjusted, decorrelated, cooldown-aware, capital-budgeted)
      -> subscribe (WS market+user) + REST order-book failsafe
        -> on each event: per-pool DECISION (quote / hold / exit) -> gated execution
          -> reconcile (re-check each pool's reward config) + kill-switch + cooldown

Real orders flow only when ``Config.live`` (the operator's ``PM_LIVE=1`` switch) is
on; otherwise every place/cancel/flatten is logged and dropped (dry-run default).
"""

from __future__ import annotations

import asyncio
import logging
import os
import signal
import time

from live_maker.config import Config
from live_maker.discovery import refresh
from live_maker.execution import Execution
from live_maker.feed import Feed
from live_maker.models import (
    EXIT,
    HOLD,
    IDLE,
    QUOTE,
    BookEvent,
    Decision,
    FillEvent,
    OrderBook,
    PriceChangeEvent,
)
from live_maker.portfolio import select_pools
from live_maker.scanner import AsyncRewardsClient
from live_maker.strategy import PoolMaker

log = logging.getLogger("live_maker.runner")

_DAY_S = 86_400.0


class Runner:
    """Owns the whole live-maker process state and tasks."""

    def __init__(self, config: Config, *, execution: Execution | None = None) -> None:
        self.cfg = config
        self.exe = execution  # injected in tests; built in run() otherwise
        self.scanner = AsyncRewardsClient()
        self.feed: Feed | None = None

        # active pools
        self.makers: dict[str, PoolMaker] = {}       # token -> decision engine
        self.meta: dict[str, dict] = {}              # token -> selected pool dict
        self.last_book: dict[str, OrderBook] = {}    # token -> latest book (WS or REST)
        self.daily_rate: dict[str, float] = {}       # token -> current reward daily rate

        # P&L / kill-switch (book mark-to-market, conservative: excludes reward income)
        self.cash_flow: dict[str, float] = {}        # token -> sells$ - buys$ (realized)
        self.inventory: dict[str, float] = {}        # token -> net shares (from real fills)
        self._pnl_reset_at = time.monotonic()

        # cooldown: token/condition_id -> remaining discovery rounds benched
        self.cooldown: dict[str, int] = {}

        self.safe_report: dict = {"pools": []}
        self._stop = asyncio.Event()
        self._kill_reason = ""

    # ----------------------------------------------------------------- run

    async def run(self) -> None:
        logging.getLogger("live_maker").info(self.cfg.banner())
        if self.exe is None:
            self.exe = await Execution.create(self.cfg)
        self._install_signals()
        os.makedirs(self.cfg.state_dir, exist_ok=True)

        # initial discovery + selection before opening the socket
        await self._discover_once()
        await self._select_and_sync()

        self.feed = Feed(self.exe, book_poll_s=self.cfg.book_poll_s,
                         ws_stale_s=self.cfg.ws_stale_s)
        self.feed.start(list(self.makers.keys()))

        tasks = [
            asyncio.create_task(self._hot_loop(), name="hot-loop"),
            asyncio.create_task(self._discovery_loop(), name="discovery"),
            asyncio.create_task(self._reconcile_loop(), name="reconcile"),
            asyncio.create_task(self._kill_switch_loop(), name="kill-switch"),
        ]
        try:
            await self._stop.wait()
        finally:
            for t in tasks:
                t.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            await self._shutdown()

    def _install_signals(self) -> None:
        loop = asyncio.get_event_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, lambda s=sig: self.trip_kill(f"signal:{sig!s}"))
            except (NotImplementedError, RuntimeError):
                pass  # e.g. Windows / non-main thread

    def trip_kill(self, reason: str) -> None:
        if not self._stop.is_set():
            self._kill_reason = reason
            log.warning("KILL-SWITCH tripped: %s — flattening + standing down", reason)
            self._stop.set()

    # ------------------------------------------------------------- hot loop

    async def _hot_loop(self) -> None:
        assert self.feed is not None
        async for ev in self.feed.events():
            if self._stop.is_set():
                break
            try:
                await self._on_event(ev)
            except Exception as e:  # noqa: BLE001 — one bad event must not kill the loop
                log.exception("event handling error: %s", e)

    async def _on_event(self, ev: object) -> None:
        if isinstance(ev, FillEvent):
            self._apply_fill(ev)
            # re-decide so the next quote is skewed to work the new inventory flat
            token, mid = ev.token_id, self._mid(ev.token_id)
            if token in self.makers and mid is not None:
                await self._decide_and_act(token, self.last_book.get(token), mid)
            return

        if isinstance(ev, BookEvent):
            token = ev.token_id
            if token not in self.makers:
                return
            self.last_book[token] = ev.book
            mid = ev.book.midpoint()
            await self._decide_and_act(token, ev.book, mid)
            return

        if isinstance(ev, PriceChangeEvent):
            token = ev.token_id
            if token not in self.makers:
                return
            book = self.last_book.get(token)
            await self._decide_and_act(token, book, ev.mid)
            return

    async def _decide_and_act(self, token: str, book: OrderBook | None, mid: float | None) -> None:
        if book is None or mid is None or not (0.0 < mid < 1.0):
            return
        maker = self.makers.get(token)  # defensive: pool may have been retired meanwhile
        if maker is None:
            return
        decision = maker.decide(book, mid, daily_rate=self.daily_rate.get(token, 0.0))
        await self._execute(decision)

    async def _execute(self, d: Decision) -> None:
        assert self.exe is not None
        if d.action == HOLD or d.action == IDLE:
            return
        if d.action == QUOTE:
            if d.cancel_first:
                await self.exe.cancel_all(d.token_id)
            await self.exe.place_quotes(d.token_id, d.orders)
            log.info("QUOTE %s mid=%.4f share=%.3f inv=%.2f skew=%.2f cap=$%.0f",
                     d.token_id[:10], d.mid, d.est_share, d.inventory, d.skew_ticks,
                     d.committed_capital)
            return
        if d.action == EXIT:
            # retire ALWAYS, even if cancel/flatten raises — never leave a halted
            # pool tracked-but-unmanaged (the shutdown sweep is the final backstop).
            try:
                await self.exe.cancel_all(d.token_id)
                await self.exe.flatten(d.token_id, self.inventory.get(d.token_id, 0.0))
            finally:
                log.warning("EXIT %s reason=%s inv=%.2f", d.token_id[:10], d.reason, d.inventory)
                self._retire(d.token_id, cooldown=d.cooldown, reason=d.reason)

    # --------------------------------------------------------------- fills

    def _apply_fill(self, ev: FillEvent) -> None:
        token = ev.token_id
        if token not in self.makers:
            return
        self.makers[token].on_fill(ev.side, ev.size)
        self.inventory[token] = self.makers[token].inventory
        # conservative book MTM cash-flow: a BUY pays out cash, a SELL takes cash in
        signed = (-1.0 if ev.side.upper() == "BUY" else 1.0) * ev.size * ev.price
        self.cash_flow[token] = self.cash_flow.get(token, 0.0) + signed
        log.info("FILL %s %s %.2f @ %.4f -> inv=%.2f",
                 token[:10], ev.side, ev.size, ev.price, self.inventory[token])

    def _mid(self, token: str) -> float | None:
        book = self.last_book.get(token)
        return book.midpoint() if book is not None else None

    def book_mtm(self) -> float:
        """Conservative book P&L: realized cash flow + open inventory marked at the
        last mid. EXCLUDES reward income (PM pays that out separately), so it is a
        lower bound — exactly what a loss kill-switch should watch."""
        total = sum(self.cash_flow.values())
        for token, inv in self.inventory.items():
            mid = self._mid(token)
            if mid is not None:
                total += inv * mid
        return total

    # ------------------------------------------------------ pool lifecycle

    def _retire(self, token: str, *, cooldown: bool, reason: str) -> None:
        meta = self.meta.pop(token, None)
        self.makers.pop(token, None)
        self.last_book.pop(token, None)
        self.daily_rate.pop(token, None)
        # keep cash_flow/inventory in the day's P&L tally until the daily reset
        if cooldown and meta is not None:
            self.cooldown[meta.get("condition_id", token)] = self.cfg.cooldown_rounds
            self.cooldown[token] = self.cfg.cooldown_rounds
        if self.feed is not None:
            self.feed.update_tokens(list(self.makers.keys()))
        log.info("retired %s (%s); active pools=%d", token[:10], reason, len(self.makers))

    async def _select_and_sync(self) -> None:
        """Recompute the target book from the latest scan and reconcile the active
        set: add newly selected pools, drop de-selected ones (cancel their orders)."""
        cd = set(k for k, v in self.cooldown.items() if v > 0)
        selected = select_pools(
            self.safe_report,
            capital=self.cfg.capital,
            max_pools=self.cfg.max_pools,
            require_safe=self.cfg.require_safe,
            half_spread_ticks=self.cfg.half_spread_ticks,
            risk_tolerance_days=self.cfg.risk_tolerance_days,
            max_token_overlap=self.cfg.max_token_overlap,
            cooldown=cd,
        )
        want = {s["token"]: s for s in selected}

        # drop pools no longer selected (retire even if the cancel raises)
        for token in list(self.makers.keys()):
            if token not in want:
                try:
                    if self.exe is not None:
                        await self.exe.cancel_all(token)
                finally:
                    self._retire(token, cooldown=False, reason="deselected")

        # add newly selected pools
        for token, s in want.items():
            if token in self.makers:
                continue
            # clear any stale resting orders from a previous lifetime of this token
            if self.exe is not None:
                await self.exe.cancel_all(token)
            self.meta[token] = s
            self.daily_rate[token] = float(s.get("daily", 0.0))
            self.makers[token] = PoolMaker(
                token_id=token,
                condition_id=s.get("condition_id", ""),
                question=s.get("question", ""),
                max_spread_c=s["max_spread_c"],
                min_size=s["min_size"],
                tick=s["tick"],
                half_spread_c=s["half_spread_c"],
                max_inventory=self.cfg.max_inventory_mult * s["min_size"],
            )
            log.info("selected %s | %s | daily=$%.0f share~%.2f",
                     token[:10], s["question"][:48], s["daily"], s["share"])

        if self.feed is not None:
            self.feed.update_tokens(list(self.makers.keys()))
        log.info("active book: %d pools, est reward $%.2f/day, committed $%.0f",
                 len(want), sum(s["est_daily_reward"] for s in want.values()),
                 sum(s["committed_capital"] for s in want.values()))

    # --------------------------------------------------------- background

    async def _discover_once(self) -> None:
        try:
            self.safe_report = await refresh(
                self.scanner,
                out_path=os.path.join(self.cfg.state_dir, "safe_pools.json")
                if os.path.isdir(self.cfg.state_dir) else None,
                min_daily=self.cfg.min_daily,
                top=self.cfg.scan_top,
                with_jump_risk=True,
            )
            log.info("discovery: %d safe pools (of %d scored)",
                     self.safe_report.get("safe_count", 0),
                     self.safe_report.get("pools_scored", 0))
        except Exception as e:  # noqa: BLE001 — keep the loop alive on a scan hiccup
            log.warning("discovery scan failed: %s", e)

    async def _discovery_loop(self) -> None:
        while not self._stop.is_set():
            await self._sleep(self.cfg.discovery_interval_s)
            if self._stop.is_set():
                break
            # tick down cooldowns each discovery round
            for k in list(self.cooldown.keys()):
                self.cooldown[k] -= 1
                if self.cooldown[k] <= 0:
                    del self.cooldown[k]
            await self._discover_once()
            await self._select_and_sync()

    async def _reconcile_loop(self) -> None:
        """Re-check each active pool's reward config; a pool that left the program
        or resolved (daily<=0) gets daily_rate=0 -> the strategy exits it."""
        while not self._stop.is_set():
            await self._sleep(self.cfg.reconcile_interval_s)
            if self._stop.is_set():
                break
            for token, meta in list(self.meta.items()):
                cond = meta.get("condition_id")
                if not cond:
                    continue
                try:
                    cfg = await self.scanner.reward_config(cond)
                except Exception:  # noqa: BLE001 — transient; retry next round
                    continue
                self.daily_rate[token] = float(cfg["daily"]) if cfg else 0.0
                if not cfg or cfg["daily"] <= 0:
                    log.info("reconcile: %s left the reward program -> will exit", token[:10])

    def _kill_check(self) -> str | None:
        """Return a kill reason if a kill-switch condition is met, else None.

        Conditions: an operator KILL file (hard switch), or the day's conservative
        book MTM breaching ``-max_loss_per_day``. Extracted for unit testing."""
        if os.path.exists(self.cfg.kill_file) or os.path.exists(
            os.path.join(self.cfg.state_dir, self.cfg.kill_file)
        ):
            return "kill-file"
        mtm = self.book_mtm()
        if mtm <= -self.cfg.max_loss_per_day:
            return f"max-loss/day (book MTM ${mtm:.2f})"
        return None

    async def _kill_switch_loop(self) -> None:
        while not self._stop.is_set():
            await self._sleep(2.0)
            if self._stop.is_set():
                break
            # reset the daily loss baseline every 24h
            if time.monotonic() - self._pnl_reset_at > _DAY_S:
                self._pnl_reset_at = time.monotonic()
                self.cash_flow.clear()
            reason = self._kill_check()
            if reason is not None:
                self.trip_kill(reason)
                break

    async def _sleep(self, seconds: float) -> None:
        """Sleep that wakes early on stop."""
        try:
            await asyncio.wait_for(self._stop.wait(), timeout=seconds)
        except asyncio.TimeoutError:
            pass

    # ---------------------------------------------------------- shutdown

    async def _shutdown(self) -> None:
        log.warning("shutting down (%s): cancelling all orders + flattening",
                    self._kill_reason or "stop")
        if self.feed is not None:
            await self.feed.stop()
        if self.exe is not None:
            for token in list(self.makers.keys()):
                try:
                    await self.exe.cancel_all(token)
                    await self.exe.flatten(token, self.inventory.get(token, 0.0))
                except Exception as e:  # noqa: BLE001
                    log.warning("shutdown cleanup failed for %s: %s", token[:10], e)
            await self.exe.close()
        await self.scanner.close()
        log.warning("live-maker stopped.")


def main() -> None:
    logging.basicConfig(
        level=os.environ.get("LM_LOG_LEVEL", "INFO"),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    cfg = Config.from_env()
    asyncio.run(Runner(cfg).run())


if __name__ == "__main__":
    main()
