"""Market feed — one normalized event stream multiplexed from TWO sources:

  1. WebSocket (``client.subscribe([MarketSpec(...), UserSpec()])``) — the primary,
     low-latency driver: book snapshots, price changes, and REAL wallet fills.
  2. REST order-book failsafe (``execution.get_book`` on a timer) — pulls the full
     book on a fixed cadence AND whenever the WS has gone quiet for ``ws_stale_s``.
     This is the "保底" backstop: if the socket drops, lags, or silently stalls,
     the loop keeps seeing fresh books and keeps quoting/cancelling correctly.

Both sources push into one ``asyncio.Queue`` of normalized
:class:`BookEvent` / :class:`PriceChangeEvent` / :class:`FillEvent`, so the runner
consumes a single stream and doesn't care where an update came from. The WS reader
auto-reconnects with backoff; the REST poller keeps the loop alive across drops.
"""

from __future__ import annotations

import asyncio
import logging
import time

from live_maker.execution import Execution, _normalize_book
from live_maker.models import BookEvent, FillEvent, PriceChangeEvent

log = logging.getLogger("live_maker.feed")


def _get(obj: object, *names: str, default: object = None) -> object:
    """First present attribute/dict-key among ``names`` (handles SDK objects and
    raw dict events)."""
    for n in names:
        if isinstance(obj, dict):
            if n in obj:
                return obj[n]
        elif hasattr(obj, n):
            return getattr(obj, n)
    return default


def normalize_event(ev: object):
    """Map one SDK stream event to a BookEvent / PriceChangeEvent / FillEvent,
    or ``None`` to skip. Defensive: the SDK's concrete event types vary, so we
    classify by shape (has a book? has a fill? has a price?) not by class.
    """
    token = _get(ev, "token_id", "asset_id", "market", "asset")
    if token is not None:
        token = str(token)

    # a real fill on our wallet (UserTradeEvent-ish): has a side + size
    side = _get(ev, "side")
    size = _get(ev, "size", "amount", "shares")
    is_trade = _get(ev, "type", "event_type")
    if side is not None and size is not None and (
        "trade" in str(is_trade).lower() or "fill" in str(is_trade).lower()
        or _get(ev, "trade_id", "fill_id") is not None
    ):
        try:
            price = float(_get(ev, "price", "avg_price", default=0.0) or 0.0)
            return FillEvent(token_id=token or "", side=str(side).upper(),
                             size=float(size), price=price,
                             order_id=str(_get(ev, "order_id", "id", default="") or ""))
        except (TypeError, ValueError):
            return None

    # a full book snapshot: has bids/asks
    bids = _get(ev, "bids")
    asks = _get(ev, "asks")
    if bids is not None or asks is not None:
        book = _normalize_book(ev)
        if book.bids and book.asks:
            return BookEvent(token_id=token or "", book=book, source="ws")
        return None

    # a price/mid change without a full book
    mid = _get(ev, "mid", "midpoint", "price")
    if token is not None and mid is not None:
        try:
            return PriceChangeEvent(token_id=token, mid=float(mid), source="ws")
        except (TypeError, ValueError):
            return None
    return None


class Feed:
    """Multiplexes WS + REST-failsafe events into one async stream."""

    def __init__(
        self,
        execution: Execution,
        *,
        book_poll_s: float = 15.0,
        ws_stale_s: float = 30.0,
        max_queue: int = 10_000,
    ) -> None:
        self.exe = execution
        self.book_poll_s = book_poll_s
        self.ws_stale_s = ws_stale_s
        self._q: asyncio.Queue = asyncio.Queue(maxsize=max_queue)
        self._tokens: list[str] = []
        self._tasks: list[asyncio.Task] = []
        self._last_ws = 0.0
        self._last_rest_poll = 0.0
        self._dropped = 0
        self._stop = asyncio.Event()

    # -- lifecycle ----------------------------------------------------------

    def start(self, token_ids: list[str]) -> None:
        self._tokens = list(token_ids)
        self._last_ws = time.monotonic()
        self._last_rest_poll = 0.0  # 0 -> the REST loop polls immediately (cold-start book)
        self._tasks = [
            asyncio.create_task(self._ws_loop(), name="feed-ws"),
            asyncio.create_task(self._rest_loop(), name="feed-rest"),
        ]

    def update_tokens(self, token_ids: list[str]) -> None:
        """Swap the subscribed token set (e.g. after re-selection). The WS loop
        re-subscribes on its next cycle; the REST poller picks it up immediately."""
        self._tokens = list(token_ids)

    async def stop(self) -> None:
        self._stop.set()
        for t in self._tasks:
            t.cancel()
        for t in self._tasks:
            try:
                await t
            except (asyncio.CancelledError, Exception):  # noqa: BLE001
                pass
        self._tasks = []

    async def events(self):
        """Yield normalized events until stopped."""
        while not self._stop.is_set():
            try:
                ev = await asyncio.wait_for(self._q.get(), timeout=1.0)
            except asyncio.TimeoutError:
                continue
            yield ev

    # -- WS source ----------------------------------------------------------

    async def _subscribe(self):
        """Open the SDK stream for the current tokens + the user channel."""
        from polymarket.streams import MarketSpec, UserSpec  # lazy

        specs = [MarketSpec(token_ids=list(self._tokens)), UserSpec()]
        return await self.exe.client.subscribe(specs)  # type: ignore[union-attr]

    async def _ws_loop(self) -> None:
        backoff = 1.0
        while not self._stop.is_set():
            try:
                stream = await self._subscribe()
                backoff = 1.0
                async with stream:
                    async for ev in stream:
                        if self._stop.is_set():
                            break
                        self._last_ws = time.monotonic()
                        norm = normalize_event(ev)
                        if norm is not None:
                            await self._put(norm)
            except asyncio.CancelledError:
                raise
            except Exception as e:  # noqa: BLE001 — never let WS kill the process
                log.warning("WS stream error (%s); reconnecting in %.0fs "
                            "(REST failsafe still live)", e, backoff)
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2, 30.0)

    # -- REST failsafe source ----------------------------------------------

    async def _rest_loop(self) -> None:
        """Pull the full book for every active token on a timer, and immediately
        if the WS has gone stale. Tagged ``source="rest"`` so the runner can tell
        a failsafe refresh from a live WS push."""
        while not self._stop.is_set():
            await asyncio.sleep(min(self.book_poll_s, 5.0))
            now = time.monotonic()
            stale = (now - self._last_ws) > self.ws_stale_s
            # poll on a true elapsed-time cadence OR immediately when the socket
            # looks dead (elapsed time, NOT absolute-time modulo, so the interval
            # is guaranteed regardless of when the process started).
            due = stale or (now - self._last_rest_poll) >= self.book_poll_s
            if not due:
                continue
            self._last_rest_poll = now
            await self._poll_books_once()
            if stale:
                log.info("WS stale > %.0fs — REST failsafe is driving the book", self.ws_stale_s)

    async def _poll_books_once(self) -> int:
        """Pull every active token's book once via REST and enqueue it. Returns the
        number of books enqueued. This is the order-book failsafe in one step."""
        self._last_rest_poll = time.monotonic()
        n = 0
        for token in list(self._tokens):
            try:
                book = await self.exe.get_book(token)
            except Exception as e:  # noqa: BLE001
                log.debug("REST book poll failed for %s: %s", token, e)
                continue
            if book.bids and book.asks:
                await self._put(BookEvent(token_id=token, book=book, source="rest"))
                n += 1
        return n

    async def _put(self, ev: object) -> None:
        # No await inside this method -> atomic on the single event loop, so the
        # drop-oldest recovery cannot interleave with another producer.
        try:
            self._q.put_nowait(ev)
        except asyncio.QueueFull:
            # drop the oldest to stay current (we care about the latest book/mid)
            try:
                self._q.get_nowait()
                self._q.put_nowait(ev)
                self._dropped += 1
                if self._dropped % 100 == 1:
                    log.warning("feed queue full — dropped %d stale events so far "
                                "(consumer is behind)", self._dropped)
            except (asyncio.QueueEmpty, asyncio.QueueFull):
                pass
