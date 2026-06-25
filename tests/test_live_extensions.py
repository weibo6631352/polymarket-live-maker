"""Tests for the LIVE additions to LiveMakerBot: real-fill inventory + the
``external_fills`` switch (dry-run keeps the original mid-cross inference)."""

from __future__ import annotations

import time

from pm_trader.maker_live import ConnectionWarmer, DryRunSubmitter, LiveMakerBot
from pm_trader.models import OrderBook, OrderBookLevel


class TestConnectionWarmer:
    def test_pings_periodically_then_stops(self):
        calls = []
        w = ConnectionWarmer(lambda: calls.append(1), interval=0.02)
        w.start()
        time.sleep(0.12)            # ~5-6 intervals
        w.stop()
        n = len(calls)
        assert n >= 2               # pinged several times while running
        time.sleep(0.06)
        assert len(calls) == n      # no further pings after stop

    def test_ping_errors_swallowed_thread_survives(self):
        def boom():
            raise RuntimeError("cold")
        w = ConnectionWarmer(boom, interval=0.02)
        w.start()
        time.sleep(0.06)
        w.stop()
        assert w.ticks == 0         # errors don't count, but didn't crash the thread

    def test_start_is_idempotent(self):
        w = ConnectionWarmer(lambda: None, interval=0.02)
        w.start()
        w.start()                   # second start is a no-op (one thread)
        w.stop()


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


class TestDryRunSubmitter:
    def test_logs_and_sends_nothing(self):
        sub = DryRunSubmitter()
        ack = sub({"action": "PLACE", "token_id": "t", "side": "BUY",
                   "price": 0.49, "size": 50})
        assert ack["dry_run"] is True and ack["status"] == "OK"
        assert len(sub.sent) == 1 and sub.sent[0]["action"] == "PLACE"

    def test_no_fills(self):
        assert DryRunSubmitter().poll_fills() == []
