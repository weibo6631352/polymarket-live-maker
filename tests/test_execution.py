"""Tests for the execution adapter: the dry-run gate + book normalization."""

from __future__ import annotations

from live_maker.execution import Execution, _normalize_book
from live_maker.models import OrderBook, OrderBookLevel


class _Resp:
    def __init__(self, ok=True, order_id="abc"):
        self.ok = ok
        self.order_id = order_id


class FakeSDKClient:
    """Shaped like AsyncSecureClient for the calls Execution makes."""

    def __init__(self):
        self.calls: list[tuple] = []

    async def place_limit_order(self, *, token_id, side, price, size):
        self.calls.append(("place", token_id, side, price, size))
        return _Resp(order_id="live-1")

    async def place_market_order(self, *, token_id, side, amount, order_type):
        self.calls.append(("market", token_id, side, amount, order_type))
        return _Resp()

    async def cancel_market_orders(self, *, token_id):
        self.calls.append(("cancel_all", token_id))
        return {"canceled": True}

    async def cancel_order(self, *, order_id):
        self.calls.append(("cancel", order_id))
        return {"canceled": True}

    async def get_order_book(self, *, token_id):
        return {"bids": [{"price": 0.49, "size": 100}], "asks": [{"price": 0.51, "size": 100}]}

    async def get_midpoint(self, *, token_id):
        return 0.50

    async def close(self):
        self.calls.append(("close",))


# ---- normalization --------------------------------------------------------

class TestNormalizeBook:
    def test_dict_shape(self):
        b = _normalize_book({"bids": [{"price": 0.49, "size": 100}],
                             "asks": [{"price": 0.51, "size": 100}]})
        assert b.best_bid() == 0.49 and b.best_ask() == 0.51

    def test_object_with_levels(self):
        class L:
            def __init__(self, p, s): self.price, self.size = p, s

        class B:
            bids = [L(0.49, 100)]
            asks = [L(0.51, 100)]
        b = _normalize_book(B())
        assert b.midpoint() == 0.50

    def test_tuple_levels(self):
        b = _normalize_book({"bids": [[0.49, 100]], "asks": [[0.51, 100]]})
        assert b.best_bid() == 0.49

    def test_garbage_dropped(self):
        b = _normalize_book({"bids": [{"price": "x"}], "asks": []})
        assert b.bids == []


# ---- dry-run gate ---------------------------------------------------------

class TestDryRunGate:
    async def test_place_is_noop_when_not_live(self):
        c = FakeSDKClient()
        exe = Execution(c, live=False)
        ack = await exe.place_quote("tok", "BUY", 0.49, 50)
        assert ack["dry_run"] is True
        assert c.calls == []  # NOTHING sent to the network

    async def test_cancel_is_noop_when_not_live(self):
        c = FakeSDKClient()
        exe = Execution(c, live=False)
        await exe.cancel_all("tok")
        await exe.cancel_order("oid")
        assert c.calls == []

    async def test_flatten_noop_when_not_live(self):
        c = FakeSDKClient()
        exe = Execution(c, live=False)
        ack = await exe.flatten("tok", 50.0)
        assert ack["dry_run"] is True and c.calls == []

    async def test_flatten_zero_inventory_returns_none(self):
        exe = Execution(FakeSDKClient(), live=False)
        assert await exe.flatten("tok", 0.0) is None

    async def test_reads_always_real(self):
        c = FakeSDKClient()
        exe = Execution(c, live=False)
        b = await exe.get_book("tok")
        assert b.midpoint() == 0.50
        assert await exe.get_midpoint("tok") == 0.50


class TestLivePath:
    async def test_place_calls_sdk_when_live(self):
        c = FakeSDKClient()
        exe = Execution(c, live=True)
        ack = await exe.place_quote("tok", "BUY", 0.49, 50)
        assert ack["dry_run"] is False and ack["order_id"] == "live-1"
        assert c.calls[0][0] == "place"

    async def test_place_quotes_two_sided(self):
        c = FakeSDKClient()
        exe = Execution(c, live=True)
        await exe.place_quotes("tok", [{"side": "BUY", "price": 0.49, "size": 50},
                                       {"side": "SELL", "price": 0.51, "size": 50}])
        assert [x[0] for x in c.calls] == ["place", "place"]

    async def test_cancel_calls_sdk_when_live(self):
        c = FakeSDKClient()
        exe = Execution(c, live=True)
        await exe.cancel_all("tok")
        await exe.cancel_order("oid")
        assert [x[0] for x in c.calls] == ["cancel_all", "cancel"]

    async def test_flatten_market_orders_when_live(self):
        c = FakeSDKClient()
        exe = Execution(c, live=True)
        await exe.flatten("tok", 50.0)   # long -> SELL
        assert c.calls[0][:3] == ("market", "tok", "SELL")
        await exe.flatten("tok", -30.0)  # short -> BUY
        assert c.calls[1][:3] == ("market", "tok", "BUY")

    async def test_close(self):
        c = FakeSDKClient()
        exe = Execution(c, live=True)
        await exe.close()
        assert ("close",) in c.calls
