"""Tests for the migrated pure reward math."""

from __future__ import annotations

import math

import pytest

from live_maker.models import OrderBook, OrderBookLevel
from live_maker.reward_math import (
    _inband_weight,
    adverse_bleed,
    book_inband_qmin,
    committed_capital,
    expected_excess_move,
    maker_quote_score,
    maker_reward_share,
    optimal_half_spread,
    realized_sigma_c_from_history,
    reward_accrual,
    skewed_center,
)


def _book(bid=0.49, ask=0.51, size=1000.0):
    return OrderBook(bids=[OrderBookLevel(bid, size)], asks=[OrderBookLevel(ask, size)])


class TestInbandWeight:
    def test_tightest_is_one(self):
        assert _inband_weight(0.0, 4.5) == 1.0

    def test_at_band_edge_is_zero(self):
        assert _inband_weight(4.5, 4.5) == pytest.approx(0.0, abs=1e-9)

    def test_outside_band_zero(self):
        assert _inband_weight(5.0, 4.5) == 0.0
        assert _inband_weight(-0.5, 4.5) == 0.0

    def test_nonpositive_spread_zero(self):
        assert _inband_weight(1.0, 0.0) == 0.0

    def test_quadratic_midband(self):
        # s = c/2 -> ((c - c/2)/c)^2 = 0.25
        assert _inband_weight(2.25, 4.5) == pytest.approx(0.25)


class TestBookQmin:
    def test_binding_side_is_min(self):
        b = OrderBook(bids=[OrderBookLevel(0.49, 1000)],
                      asks=[OrderBookLevel(0.51, 200)])
        q = book_inband_qmin(b, 0.50, 4.5)
        # ask side is lighter -> binding
        assert q == pytest.approx(200 * _inband_weight(1.0, 4.5))

    def test_empty_book_zero(self):
        assert book_inband_qmin(OrderBook(), 0.5, 4.5) == 0.0


class TestQuoteScoreAndShare:
    def test_quote_score(self):
        assert maker_quote_score(50, 1.0, 4.5) == pytest.approx(50 * _inband_weight(1.0, 4.5))

    def test_out_of_band_scores_zero(self):
        assert maker_quote_score(50, 9.0, 4.5) == 0.0

    def test_share_empty_book_is_one(self):
        assert maker_reward_share(50, 1.0, 4.5, 0.0) == 1.0

    def test_share_out_of_band_zero(self):
        assert maker_reward_share(50, 9.0, 4.5, 100.0) == 0.0

    def test_share_half_against_equal(self):
        mine = maker_quote_score(50, 1.0, 4.5)
        assert maker_reward_share(50, 1.0, 4.5, mine) == pytest.approx(0.5)


class TestAccrualAndBleed:
    def test_reward_accrual_prorata(self):
        # full day at share 0.5 of $400 = $200
        assert reward_accrual(0.5, 400.0, 86_400.0) == pytest.approx(200.0)

    def test_reward_accrual_guards(self):
        assert reward_accrual(0.0, 400, 100) == 0.0
        assert reward_accrual(0.5, 0, 100) == 0.0
        assert reward_accrual(0.5, 400, 0) == 0.0

    def test_bleed_within_offset_zero(self):
        assert adverse_bleed(50, 1.0, 0.50, 0.505) == 0.0  # 0.5c < 1c offset

    def test_bleed_beyond_offset(self):
        # move 2c, offset 1c -> excess 1c=0.01 price -> 50*0.01
        assert adverse_bleed(50, 1.0, 0.50, 0.52) == pytest.approx(0.5)

    def test_bleed_cancel_efficiency(self):
        assert adverse_bleed(50, 1.0, 0.50, 0.52, cancel_efficiency=0.9) == pytest.approx(0.05)


class TestSkewAndCapital:
    def test_skew_long_centers_down(self):
        c = skewed_center(0.50, inventory=100, size=50, half_spread_c=1.0, skew_strength=2.0)
        assert c < 0.50

    def test_skew_zero_size(self):
        assert skewed_center(0.50, 100, 0, 1.0, 2.0) == 0.50

    def test_committed_capital(self):
        assert committed_capital(50, 1.0) == pytest.approx(50 * (1 - 0.02))

    def test_committed_capital_degenerate(self):
        assert committed_capital(50, 60.0) == 0.0


class TestOptimalQuoting:
    def test_excess_move_zero_sigma(self):
        assert expected_excess_move(0.0, 1.0) == 0.0

    def test_excess_move_positive(self):
        v = expected_excess_move(2.0, 1.0)
        assert v > 0

    def test_excess_move_closed_form(self):
        sigma, a = 2.0, 1.0
        phi = math.exp(-0.5 * (a / sigma) ** 2) / math.sqrt(2 * math.pi)
        cdf = 0.5 * (1 + math.erf((a / sigma) / math.sqrt(2)))
        expected = 2 * sigma * phi - 2 * a * (1 - cdf)
        assert expected_excess_move(sigma, a) == pytest.approx(expected)

    def test_sigma_from_history_degenerate(self):
        assert realized_sigma_c_from_history([], 60) == 0.0
        assert realized_sigma_c_from_history([{"t": 1, "p": 0.5}], 60) == 0.0

    def test_sigma_from_history_flat_is_zero(self):
        h = [{"t": i * 60, "p": 0.5} for i in range(10)]
        assert realized_sigma_c_from_history(h, 60) == 0.0

    def test_sigma_from_history_moves(self):
        h = [{"t": i * 60, "p": 0.5 + 0.01 * (i % 2)} for i in range(10)]
        assert realized_sigma_c_from_history(h, 60) > 0

    def test_sigma_from_history_skips_bad_points(self):
        h = [{"t": 0, "p": 0.5}, {"bad": 1}, {"t": 60, "p": 0.52}]
        assert realized_sigma_c_from_history(h, 60) >= 0.0

    def test_sigma_from_history_nonincreasing_ts_zero(self):
        # all timestamps equal -> no positive gaps -> 0.0
        h = [{"t": 5, "p": 0.5}, {"t": 5, "p": 0.7}, {"t": 5, "p": 0.9}]
        assert realized_sigma_c_from_history(h, 60) == 0.0

    def test_optimal_zero_sigma_picks_tightest(self):
        out = optimal_half_spread(
            daily_rate=400, max_spread_c=4.5, min_size=50, tick_c=0.1,
            existing_qmin=0.0, sigma_c=0.0, periods_per_day=1440,
        )
        assert out["half_spread_c"] == pytest.approx(0.1)
        assert out["bleed_per_day"] == 0.0

    def test_optimal_high_vol_widens(self):
        tight = optimal_half_spread(
            daily_rate=400, max_spread_c=4.5, min_size=50, tick_c=0.1,
            existing_qmin=10.0, sigma_c=0.0, periods_per_day=1440)
        wide = optimal_half_spread(
            daily_rate=400, max_spread_c=4.5, min_size=50, tick_c=0.1,
            existing_qmin=10.0, sigma_c=5.0, periods_per_day=1440)
        assert wide["half_spread_c"] >= tight["half_spread_c"]

    def test_optimal_grid_floor(self):
        out = optimal_half_spread(
            daily_rate=400, max_spread_c=0.1, min_size=50, tick_c=0.1,
            existing_qmin=0.0, sigma_c=0.0, periods_per_day=1440, grid=0)
        assert out["half_spread_c"] == pytest.approx(0.1)
