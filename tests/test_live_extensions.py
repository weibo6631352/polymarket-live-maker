"""Tests for the LIVE additions to LiveMakerBot: real-fill inventory + the
``external_fills`` switch (dry-run keeps the original mid-cross inference)."""

from __future__ import annotations

from pm_trader.maker_live import LiveMakerBot
from pm_trader.models import OrderBook, OrderBookLevel


def _book(bid=0.49, ask=0.51, size=1000):
    return OrderBook(bids=[OrderBookLevel(price=bid, size=size)],
                     asks=[OrderBookLevel(price=ask, size=size)])


def _bot(**kw):
    return LiveMakerBot(token_id="tok", max_spread_c=4.5, min_size=50.0, tick=0.01, **kw)


class TestApplyRealFill:
    def test_buy_makes_long(self):
        bot = _bot()
        bot.apply_real_fill("BUY", 50)
        assert bot.inventory == 50.0

    def test_sell_makes_short(self):
        bot = _bot()
        bot.apply_real_fill("SELL", 50)
        assert bot.inventory == -50.0

    def test_clamped_to_cap(self):
        bot = _bot(max_inventory=50.0)
        bot.apply_real_fill("BUY", 50)
        bot.apply_real_fill("BUY", 50)
        assert bot.inventory == 50.0


class TestExternalFillsSwitch:
    def test_external_fills_skips_inference(self):
        # live path: inventory comes from real fills, NOT inferred from mid moves
        bot = _bot(external_fills=True)
        bot.step(_book(), 0.50)               # quotes bid 0.49 / ask 0.51
        bot.step(_book(0.52, 0.54), 0.53)     # mid past our ask, but inference is OFF
        assert bot.inventory == 0.0           # unchanged until a real fill is applied

    def test_default_still_infers(self):
        # dry-run / paper path is unchanged (一模一样): mid-cross inference still runs
        bot = _bot()                          # external_fills defaults False
        bot.step(_book(), 0.50)
        bot.step(_book(0.52, 0.54), 0.53)     # 3c move past our ask -> inferred sale
        assert bot.inventory == -50.0
