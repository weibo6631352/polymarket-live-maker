"""Tests for feed event normalization + the REST order-book failsafe plumbing."""

from __future__ import annotations

from live_maker.feed import Feed, normalize_event
from live_maker.models import BookEvent, FillEvent, PriceChangeEvent
from tests.conftest import FakeExecution, book


class TestNormalizeEvent:
    def test_book_dict(self):
        ev = normalize_event({"asset_id": "tok", "bids": [{"price": 0.49, "size": 100}],
                              "asks": [{"price": 0.51, "size": 100}]})
        assert isinstance(ev, BookEvent) and ev.token_id == "tok"

    def test_one_sided_book_skipped(self):
        assert normalize_event({"asset_id": "tok", "bids": [{"price": 0.49, "size": 1}],
                                "asks": []}) is None

    def test_fill_event(self):
        ev = normalize_event({"event_type": "trade", "asset_id": "tok", "side": "BUY",
                              "size": 50, "price": 0.49, "order_id": "o1"})
        assert isinstance(ev, FillEvent) and ev.side == "BUY" and ev.size == 50.0

    def test_fill_via_trade_id(self):
        ev = normalize_event({"asset_id": "tok", "side": "SELL", "size": 25,
                              "price": 0.51, "trade_id": "t9"})
        assert isinstance(ev, FillEvent) and ev.side == "SELL"

    def test_price_change(self):
        ev = normalize_event({"asset_id": "tok", "mid": 0.50})
        assert isinstance(ev, PriceChangeEvent) and ev.mid == 0.50

    def test_object_event(self):
        class E:
            asset_id = "tok"
            mid = 0.5
        assert isinstance(normalize_event(E()), PriceChangeEvent)

    def test_garbage_none(self):
        assert normalize_event({"nothing": 1}) is None

    def test_bad_fill_numbers_none(self):
        assert normalize_event({"event_type": "trade", "side": "BUY",
                                "size": "x", "trade_id": "t"}) is None


class TestRestFailsafe:
    async def test_poll_books_enqueues(self):
        exe = FakeExecution(books={"a": book(), "b": book()})
        feed = Feed(exe)
        feed.start(["a", "b"])
        n = await feed._poll_books_once()
        await feed.stop()
        assert n == 2

    async def test_poll_skips_empty_book(self):
        exe = FakeExecution(books={"a": book()})  # 'b' has no book -> empty
        feed = Feed(exe)
        feed._tokens = ["a", "b"]
        assert await feed._poll_books_once() == 1

    async def test_events_yields_enqueued(self):
        exe = FakeExecution(books={"a": book()})
        feed = Feed(exe)
        feed._tokens = ["a"]
        await feed._poll_books_once()
        gen = feed.events()
        ev = await gen.__anext__()
        await feed.stop()
        assert isinstance(ev, BookEvent) and ev.source == "rest"

    async def test_update_tokens(self):
        feed = Feed(FakeExecution())
        feed.update_tokens(["x", "y"])
        assert feed._tokens == ["x", "y"]

    async def test_put_drops_oldest_when_full(self):
        feed = Feed(FakeExecution(), max_queue=1)
        await feed._put("a")
        await feed._put("b")  # evicts "a"
        assert feed._q.qsize() == 1
