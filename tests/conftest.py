"""Shared test fixtures/fakes. No network, no SDK required."""

from __future__ import annotations

import pytest

from live_maker.models import OrderBook, OrderBookLevel


def book(bid=0.49, ask=0.51, size=1000.0) -> OrderBook:
    return OrderBook(
        bids=[OrderBookLevel(price=bid, size=size)],
        asks=[OrderBookLevel(price=ask, size=size)],
    )


def raw_book(bid=0.49, ask=0.51, size=1000.0) -> dict:
    return {"bids": [{"price": bid, "size": size}],
            "asks": [{"price": ask, "size": size}]}


def market(token, daily=400.0, min_size=50.0, tick=0.01, max_spread=4.5, question=None):
    return {
        "rewards": {"rates": [{"rewards_daily_rate": daily}],
                    "max_spread": max_spread, "min_size": min_size},
        "minimum_tick_size": tick,
        "tokens": [{"token_id": token}],
        "question": question or f"Will {token} happen?",
        "condition_id": "0x" + token,
    }


def flat_hist(n=15, p=0.5):
    return [{"t": i * 3600, "p": p} for i in range(n)]


class FakeExecution:
    """Records the place/cancel/flatten calls a Decision would trigger, and serves
    canned books for the REST failsafe — stands in for the SDK-backed Execution."""

    def __init__(self, books: dict | None = None, live: bool = False):
        self.live = live
        self.books = books or {}
        self.client = object()
        self.calls: list[tuple] = []
        self.closed = False

    async def get_book(self, token_id):
        return self.books.get(token_id, OrderBook())

    async def get_midpoint(self, token_id):
        b = self.books.get(token_id)
        return b.midpoint() if b else None

    async def place_quote(self, token_id, side, price, size):
        self.calls.append(("place", token_id, side, price, size))
        return {"ok": True, "order_id": f"o{len(self.calls)}", "dry_run": not self.live}

    async def place_quotes(self, token_id, orders):
        return [await self.place_quote(token_id, o["side"], o["price"], o["size"]) for o in orders]

    async def cancel_all(self, token_id):
        self.calls.append(("cancel_all", token_id))
        return {"ok": True}

    async def cancel_order(self, order_id):
        self.calls.append(("cancel", order_id))
        return {"ok": True}

    async def flatten(self, token_id, inventory):
        self.calls.append(("flatten", token_id, inventory))
        return None if abs(inventory) < 1e-9 else {"ok": True}

    async def close(self):
        self.closed = True


@pytest.fixture
def fake_exe():
    return FakeExecution()
