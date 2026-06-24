"""Tests for per-pool decisions: quote/skew, requote, reconcile, jump/drift exit."""

from __future__ import annotations

import pytest

from live_maker.models import EXIT, HOLD, IDLE, QUOTE
from live_maker.strategy import PoolMaker, compute_two_sided_quotes, plan_requote
from tests.conftest import book


def _maker(**kw):
    return PoolMaker(token_id="tok", max_spread_c=4.5, min_size=50.0, tick=0.01, **kw)


class TestComputeQuotes:
    def test_basic(self):
        q = compute_two_sided_quotes(0.50, half_spread_c=1.0, size=50, tick=0.01, max_spread_c=4.5)
        assert q[0] == {"side": "BUY", "price": 0.49, "size": 50}
        assert q[1] == {"side": "SELL", "price": 0.51, "size": 50}

    def test_bad_mid(self):
        with pytest.raises(ValueError):
            compute_two_sided_quotes(0.0, half_spread_c=1.0, size=50, tick=0.01, max_spread_c=4.5)

    def test_bad_half_spread(self):
        with pytest.raises(ValueError):
            compute_two_sided_quotes(0.5, half_spread_c=0.0, size=50, tick=0.01, max_spread_c=4.5)
        with pytest.raises(ValueError):
            compute_two_sided_quotes(0.5, half_spread_c=9.0, size=50, tick=0.01, max_spread_c=4.5)

    def test_clamps_edges(self):
        q = compute_two_sided_quotes(0.02, half_spread_c=4.0, size=50, tick=0.01, max_spread_c=4.5)
        assert q[0]["price"] >= 0.01
        q2 = compute_two_sided_quotes(0.98, half_spread_c=4.0, size=50, tick=0.01, max_spread_c=4.5)
        assert q2[1]["price"] <= 0.99

    def test_skew_long_shifts_down(self):
        flat = compute_two_sided_quotes(0.50, half_spread_c=1.0, size=50, tick=0.01, max_spread_c=4.5)
        skewed = compute_two_sided_quotes(0.50, half_spread_c=1.0, size=50, tick=0.01,
                                          max_spread_c=4.5, skew_ticks=2.0)
        assert skewed[1]["price"] < flat[1]["price"]  # ask pulled down to sell the long


class TestPlanRequote:
    def test_move_triggers(self):
        assert plan_requote(0.50, 0.52, half_spread_c=1.0, tick=0.01) is True

    def test_small_move_no_requote(self):
        assert plan_requote(0.50, 0.505, half_spread_c=1.0, tick=0.01) is False


class TestDecide:
    def test_first_event_quotes(self):
        d = _maker().decide(book(), 0.50, daily_rate=400.0)
        assert d.action == QUOTE and d.cancel_first is True and len(d.orders) == 2
        assert 0 <= d.est_share <= 1

    def test_unchanged_mid_holds(self):
        m = _maker()
        m.decide(book(), 0.50, daily_rate=400.0)
        d = m.decide(book(), 0.50, daily_rate=400.0)
        assert d.action == HOLD

    def test_tick_move_requotes(self):
        m = _maker()
        m.decide(book(), 0.50, daily_rate=400.0)
        d = m.decide(book(0.51, 0.53), 0.52, daily_rate=400.0)
        assert d.action == QUOTE

    def test_rewards_ended_exits(self):
        d = _maker().decide(book(), 0.50, daily_rate=0.0)
        assert d.action == EXIT and d.reason == "rewards_ended" and d.cooldown is False

    def test_catalyst_jump_exits_cooldown(self):
        m = _maker()
        m.decide(book(), 0.50, daily_rate=400.0)
        d = m.decide(book(0.56, 0.58), 0.57, daily_rate=400.0)  # 7c jump > band
        assert d.action == EXIT and d.reason == "jump" and d.cooldown is True
        assert m.halted is True

    def test_drift_exit_cooldown(self):
        m = _maker()
        m.decide(book(), 0.50, daily_rate=400.0)            # entry 0.50
        m.decide(book(0.51, 0.53), 0.52, daily_rate=400.0)  # +2c
        m.decide(book(0.53, 0.55), 0.54, daily_rate=400.0)  # +2c (drift 0.04)
        d = m.decide(book(0.54, 0.56), 0.55, daily_rate=400.0)  # drift 0.05 >= band
        assert d.action == EXIT and d.reason == "drift_exit" and d.cooldown is True

    def test_idle_after_halt(self):
        m = _maker()
        m.decide(book(), 0.50, daily_rate=400.0)
        m.decide(book(0.56, 0.58), 0.57, daily_rate=400.0)  # halts
        d = m.decide(book(0.60, 0.62), 0.61, daily_rate=400.0)
        assert d.action == IDLE

    def test_invalid_half_spread_config_raises(self):
        with pytest.raises(ValueError):
            _maker(half_spread_c=9.0).decide(book(), 0.50, daily_rate=400.0)


class TestInventoryAndSkew:
    def test_buy_fill_makes_long(self):
        m = _maker()
        m.on_fill("BUY", 50)
        assert m.inventory == 50.0

    def test_sell_fill_makes_short(self):
        m = _maker()
        m.on_fill("SELL", 50)
        assert m.inventory == -50.0

    def test_inventory_clamped(self):
        m = _maker(max_inventory=50.0)
        m.on_fill("BUY", 50)
        m.on_fill("BUY", 50)
        assert m.inventory == 50.0  # capped

    def test_long_skews_quotes_down(self):
        m = _maker(max_inventory=50.0)
        m.on_fill("BUY", 50)  # long at cap
        d = m.decide(book(), 0.50, daily_rate=400.0)
        assert d.skew_ticks > 0
        assert d.orders[1]["price"] < 0.51  # ask pulled below unskewed to sell

    def test_short_skews_quotes_up(self):
        m = _maker(max_inventory=50.0)
        m.on_fill("SELL", 50)
        d = m.decide(book(), 0.50, daily_rate=400.0)
        assert d.skew_ticks < 0
        assert d.orders[0]["price"] > 0.49  # bid pulled above unskewed to buy back

    def test_skew_zero_when_no_cap(self):
        m = _maker(max_inventory=0.0)
        m.inventory = 100.0
        assert m._skew_ticks() == 0.0
