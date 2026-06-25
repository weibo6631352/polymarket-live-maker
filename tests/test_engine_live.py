"""Tests for the LIVE engine maker methods: place_maker_quote_live and
accrue_maker_rewards_live (real orders via a fake submitter, real fills,
reconcile-exit, drift-exit, re-center). No network — api is mocked."""

from __future__ import annotations

from pathlib import Path
from unittest.mock import MagicMock

import pytest

from pm_trader.engine import Engine
from pm_trader.models import Market, OrderBook, OrderBookLevel


def _book(bid=0.49, ask=0.51, size=1000):
    return OrderBook(bids=[OrderBookLevel(bid, size)], asks=[OrderBookLevel(ask, size)])


def _market():
    return Market(
        condition_id="0xabc", slug="m", question="Q?", description="",
        outcomes=["Yes", "No"], outcome_prices=[0.5, 0.5],
        tokens=[{"token_id": "tok_yes", "outcome": "Yes"},
                {"token_id": "tok_no", "outcome": "No"}],
        active=True, closed=False, tick_size=0.01,
    )


def _pool(daily=400.0):
    return {"daily": daily, "max_spread": 4.5, "min_size": 50.0, "tick": 0.01,
            "token": "tok_yes", "question": "Q?", "condition_id": "0xabc"}


def _mock_api(engine, *, mid=0.50, reward_config=None, book=None):
    engine.api.get_market = MagicMock(return_value=_market())
    engine.api.get_reward_config = MagicMock(
        return_value=_pool() if reward_config is None else reward_config)
    engine.api.get_order_book = MagicMock(return_value=book or _book())
    engine.api.get_midpoint = MagicMock(return_value=mid)
    engine.api.prices_history = MagicMock(return_value=[])


class FakeSubmitter:
    def __init__(self):
        self.calls = []

    def __call__(self, action):
        self.calls.append(action)
        return {"status": "OK", **action}

    def actions(self):
        return [c["action"] for c in self.calls]


@pytest.fixture
def eng(tmp_data_dir: Path):
    e = Engine(tmp_data_dir)
    e.init_account(10_000.0)
    yield e
    e.close()


class TestPlaceLive:
    def test_places_two_real_orders(self, eng):
        _mock_api(eng, mid=0.50)
        sub = FakeSubmitter()
        q = eng.place_maker_quote_live("0xabc", submitter=sub, half_spread_cents=1.0)
        assert sub.actions() == ["PLACE", "PLACE"]
        assert {c["side"] for c in sub.calls} == {"BUY", "SELL"}
        assert q["token_id"] == "tok_yes"
        assert len(eng.get_maker_quotes()) == 1


class TestAccrueLive:
    def _place(self, eng, sub, mid=0.50):
        _mock_api(eng, mid=mid)
        eng.place_maker_quote_live("0xabc", submitter=sub, half_spread_cents=1.0)

    def test_reconcile_exit_when_rewards_end(self, eng):
        sub = FakeSubmitter()
        self._place(eng, sub)
        eng.api.get_reward_config = MagicMock(return_value=None)  # left the program
        rows = eng.accrue_maker_rewards_live(submitter=sub, fills_by_token={})
        assert rows[0]["reconciled"] == "rewards_ended"
        assert "CANCEL_ALL" in sub.actions()
        assert eng.get_maker_summary()["active_quotes"] == 0

    def test_drift_exit_on_full_band_move(self, eng):
        sub = FakeSubmitter()
        self._place(eng, sub)
        eng.api.get_midpoint = MagicMock(return_value=0.55)  # 5c > 4.5c band from entry
        rows = eng.accrue_maker_rewards_live(submitter=sub, fills_by_token={})
        assert rows[0]["reconciled"] == "drift_exit"
        assert "CANCEL_ALL" in sub.actions()

    def test_real_fill_updates_inventory(self, eng):
        sub = FakeSubmitter()
        self._place(eng, sub)  # entry mid 0.50
        rows = eng.accrue_maker_rewards_live(
            submitter=sub,
            fills_by_token={"tok_yes": [{"side": "BUY", "size": 50, "price": 0.49}]})
        assert rows[0]["inventory"] == 50.0
        assert rows[0]["fills_applied"] == 1

    def test_recenter_on_tick_move(self, eng):
        sub = FakeSubmitter()
        self._place(eng, sub)        # entry 0.50
        sub.calls.clear()
        eng.api.get_midpoint = MagicMock(return_value=0.52)  # 2c move, < band -> re-center
        rows = eng.accrue_maker_rewards_live(submitter=sub, fills_by_token={})
        assert rows[0].get("reconciled") is None
        assert sub.actions() == ["CANCEL_ALL", "PLACE", "PLACE"]

    def test_no_recenter_when_mid_unchanged(self, eng):
        sub = FakeSubmitter()
        self._place(eng, sub)
        sub.calls.clear()
        rows = eng.accrue_maker_rewards_live(submitter=sub, fills_by_token={})  # mid still 0.50
        assert sub.calls == []           # no churn
        assert rows[0].get("reconciled") is None

    def test_flatten_on_exit_with_inventory(self, eng):
        sub = FakeSubmitter()
        self._place(eng, sub)
        # build inventory, then force a drift-exit -> should flatten the real position
        eng.accrue_maker_rewards_live(
            submitter=sub,
            fills_by_token={"tok_yes": [{"side": "BUY", "size": 50, "price": 0.49}]})
        sub.calls.clear()
        eng.api.get_midpoint = MagicMock(return_value=0.55)
        eng.accrue_maker_rewards_live(submitter=sub, fills_by_token={})
        assert "FLATTEN" in sub.actions() and "CANCEL_ALL" in sub.actions()
