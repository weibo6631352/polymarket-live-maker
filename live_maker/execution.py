"""Execution adapter over the official ``polymarket-client`` ``AsyncSecureClient``.

The ONLY place that talks to the trading API. Two responsibilities:
  1. Translate strategy decisions into SDK calls (place / cancel / flatten).
  2. Enforce the real-money gate. Order-MUTATING calls (place, cancel, flatten,
     approvals) are no-ops that only LOG when ``live`` is False — the dry-run
     default. Read calls (book, midpoint) are always real and harmless.

Real submission requires the operator's explicit ``PM_LIVE=1`` (-> ``Config.live``
-> ``Execution.live``). Nothing here flips that switch on its own.

The ``polymarket-client`` SDK is imported lazily so the pure-logic modules (and
their tests) import without the SDK present.
"""

from __future__ import annotations

import logging

from live_maker.config import Config
from live_maker.models import OrderBook, OrderBookLevel

log = logging.getLogger("live_maker.execution")


def _normalize_book(raw: object) -> OrderBook:
    """Coerce whatever ``get_order_book`` returns into our :class:`OrderBook`.

    Handles: an object with ``.bids``/``.asks`` (levels exposing ``.price``/``.size``
    or ``[price, size]`` pairs or ``{"price","size"}`` dicts) and the plain CLOB
    ``{"bids":[...],"asks":[...]}`` dict shape.
    """
    def side(obj: object) -> list[OrderBookLevel]:
        out: list[OrderBookLevel] = []
        for lvl in obj or []:  # type: ignore[union-attr]
            try:
                if hasattr(lvl, "price") and hasattr(lvl, "size"):
                    out.append(OrderBookLevel(float(lvl.price), float(lvl.size)))
                elif isinstance(lvl, dict):
                    out.append(OrderBookLevel(float(lvl["price"]), float(lvl["size"])))
                elif isinstance(lvl, (list, tuple)) and len(lvl) >= 2:
                    out.append(OrderBookLevel(float(lvl[0]), float(lvl[1])))
            except (KeyError, TypeError, ValueError):
                continue
        return out

    bids = getattr(raw, "bids", None)
    asks = getattr(raw, "asks", None)
    if bids is None and isinstance(raw, dict):
        bids, asks = raw.get("bids"), raw.get("asks")
    return OrderBook(bids=side(bids), asks=side(asks))


class Execution:
    """Order placement/cancellation + the dry-run gate. Wrap one per process."""

    def __init__(self, client: object | None, *, live: bool) -> None:
        self.client = client
        self.live = live
        self._dry_seq = 0

    # -- lifecycle ----------------------------------------------------------

    @classmethod
    async def create(cls, config: Config) -> "Execution":
        """Build the ``AsyncSecureClient`` (auth needs the key even for dry-run
        reads/streams) and run trading approvals ONLY when live."""
        config.require_key()
        from polymarket import AsyncSecureClient  # lazy: SDK optional for tests

        client = await AsyncSecureClient.create(
            private_key=config.private_key,
            wallet=config.wallet,
        )
        exe = cls(client, live=config.live)
        if config.live:
            log.warning("LIVE mode: running trading approvals (wallet deploy + USDC)")
            await client.setup_trading_approvals()
        else:
            log.info("DRY-RUN: skipping setup_trading_approvals (no on-chain txs)")
        return exe

    async def close(self) -> None:
        if self.client is not None and hasattr(self.client, "close"):
            await self.client.close()

    # -- reads (always real; harmless) -------------------------------------

    async def get_book(self, token_id: str) -> OrderBook:
        book = await self.client.get_order_book(token_id=token_id)  # type: ignore[union-attr]
        return _normalize_book(book)

    async def get_midpoint(self, token_id: str) -> float | None:
        try:
            mid = await self.client.get_midpoint(token_id=token_id)  # type: ignore[union-attr]
            return float(mid)
        except Exception:  # noqa: BLE001 — best-effort read; book.midpoint() is the fallback
            return None

    # -- mutations (gated by self.live) ------------------------------------

    def _dry_ack(self, **info: object) -> dict:
        self._dry_seq += 1
        ack = {"ok": True, "dry_run": True, "order_id": f"DRY-{self._dry_seq}", **info}
        log.info("DRY-RUN %s", ack)
        return ack

    async def place_quote(self, token_id: str, side: str, price: float, size: float) -> dict:
        """Place one resting limit order. No-op (logged) unless live."""
        if not self.live:
            return self._dry_ack(action="PLACE", token_id=token_id, side=side,
                                 price=price, size=size)
        resp = await self.client.place_limit_order(  # type: ignore[union-attr]
            token_id=token_id, side=side, price=str(price), size=str(size),
        )
        # Fail-closed: if neither field is present we treat the order as NOT ok
        # rather than optimistically assuming success.
        return {
            "ok": bool(getattr(resp, "ok", getattr(resp, "success", False))),
            "order_id": getattr(resp, "order_id", getattr(resp, "id", "")),
            "dry_run": False,
        }

    async def place_quotes(self, token_id: str, orders: list[dict]) -> list[dict]:
        """Place a two-sided quote (list of {side, price, size})."""
        acks = []
        for o in orders:
            acks.append(await self.place_quote(token_id, o["side"], o["price"], o["size"]))
        return acks

    async def cancel_all(self, token_id: str) -> dict:
        """Cancel every resting order on a token (the latency-sensitive op).

        NOTE: the SDK's cancel response shape is not verified locally — we treat a
        non-raising call as success and echo the raw response for the operator to
        eyeball. Confirm the response schema during the live smoke-test.
        """
        if not self.live:
            return self._dry_ack(action="CANCEL_ALL", token_id=token_id)
        resp = await self.client.cancel_market_orders(token_id=token_id)  # type: ignore[union-attr]
        return {"ok": True, "dry_run": False, "resp": repr(resp)[:200]}

    async def cancel_order(self, order_id: str) -> dict:
        if not self.live:
            return self._dry_ack(action="CANCEL", order_id=order_id)
        resp = await self.client.cancel_order(order_id=order_id)  # type: ignore[union-attr]
        return {"ok": True, "dry_run": False, "resp": repr(resp)[:200]}

    async def flatten(self, token_id: str, inventory: float) -> dict | None:
        """Market-out a net inventory to go flat (long -> SELL, short -> BUY).

        Used on a jump/drift exit so we don't carry a one-sided position into a
        moving market. No-op (logged) unless live or inventory is ~0.
        """
        if abs(inventory) < 1e-9:
            return None
        side = "SELL" if inventory > 0 else "BUY"
        size = abs(inventory)
        if not self.live:
            return self._dry_ack(action="FLATTEN", token_id=token_id, side=side, size=size)
        # marketable order; FAK so any unfilled remainder is dropped, not rested
        resp = await self.client.place_market_order(  # type: ignore[union-attr]
            token_id=token_id, side=side, amount=str(size), order_type="FAK",
        )
        return {"ok": bool(getattr(resp, "ok", True)), "dry_run": False,
                "side": side, "size": size}
