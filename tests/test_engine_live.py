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
    def __init__(self, fail_on=()):
        self.calls = []
        self.fail_on = set(fail_on)

    def __call__(self, action):
        self.calls.append(action)
        status = "ERROR" if action["action"] in self.fail_on else "OK"
        return {"status": status, **action}

    def actions(self):
        return [c["action"] for c in self.calls]

    def places(self):
        return [c for c in self.calls if c["action"] == "PLACE"]


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


class _FakeBookSource:
    def __init__(self, book, mid):
        self._book, self._mid = book, mid
    def get_book(self, token_id):
        return self._book
    def get_midpoint(self, token_id):
        return self._mid


class TestBookSource:
    """The WS book source is used for book/mid when fresh; REST is the fallback."""

    def test_uses_book_source_not_rest_when_fresh(self, eng):
        sub = FakeSubmitter()
        _mock_api(eng, mid=0.50)
        eng.place_maker_quote_live("0xabc", submitter=sub, half_spread_cents=1.0)
        eng.book_source = _FakeBookSource(_book(bid=0.50, ask=0.52), 0.51)
        # any REST book/mid read now is a failure — must come from the WS source
        eng.api.get_order_book = MagicMock(side_effect=AssertionError("used REST book"))
        eng.api.get_midpoint = MagicMock(side_effect=AssertionError("used REST mid"))
        rows = eng.accrue_maker_rewards_live(submitter=sub, fills_by_token={})
        assert rows and rows[0]["mid"] == 0.51            # came from book_source

    def test_falls_back_to_rest_when_source_empty(self, eng):
        sub = FakeSubmitter()
        _mock_api(eng, mid=0.50)
        eng.place_maker_quote_live("0xabc", submitter=sub, half_spread_cents=1.0)
        eng.book_source = _FakeBookSource(None, 0.0)        # not fresh -> REST
        eng.api.get_order_book = MagicMock(return_value=_book())
        eng.api.get_midpoint = MagicMock(return_value=0.50)
        rows = eng.accrue_maker_rewards_live(submitter=sub, fills_by_token={})
        assert rows and rows[0]["mid"] == 0.50
        eng.api.get_order_book.assert_called()             # REST fallback fired


class TestAccrueLive:
    def _place(self, eng, sub, mid=0.50, max_inventory=None):
        _mock_api(eng, mid=mid)
        eng.place_maker_quote_live("0xabc", submitter=sub, half_spread_cents=1.0,
                                   max_inventory=max_inventory)

    def test_force_recenter_reposts_without_a_move(self, eng):
        sub = FakeSubmitter()
        self._place(eng, sub, mid=0.50)
        sub.calls.clear()                       # same mid -> normally no re-center
        eng.accrue_maker_rewards_live(submitter=sub, fills_by_token={},
                                      force_recenter={"tok_yes"})
        assert "CANCEL_ALL" in sub.actions() and "PLACE" in sub.actions()

    def test_no_recenter_without_force_or_move(self, eng):
        sub = FakeSubmitter()
        self._place(eng, sub, mid=0.50)
        sub.calls.clear()
        eng.accrue_maker_rewards_live(submitter=sub, fills_by_token={})
        assert "CANCEL_ALL" not in sub.actions()   # no move, no force

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

    # -- H1: a failed cancel/flatten must NOT zero the ledger / mark exited --------
    def test_exit_failure_keeps_quote_active(self, eng):
        sub = FakeSubmitter(fail_on={"CANCEL_ALL"})
        self._place(eng, sub)                       # PLACE ok
        eng.api.get_midpoint = MagicMock(return_value=0.55)  # drift
        rows = eng.accrue_maker_rewards_live(submitter=sub, fills_by_token={})
        assert rows[0].get("exit_failed") == "drift_exit"
        assert eng.get_maker_summary()["active_quotes"] == 1   # still managed, not exited

    def test_reconcile_failure_keeps_quote_active(self, eng):
        sub = FakeSubmitter(fail_on={"CANCEL_ALL"})
        self._place(eng, sub)
        eng.api.get_reward_config = MagicMock(return_value=None)
        rows = eng.accrue_maker_rewards_live(submitter=sub, fills_by_token={})
        assert rows[0].get("exit_failed") == "rewards_ended"
        assert eng.get_maker_summary()["active_quotes"] == 1

    # -- M1: fill-price P&L is booked (kill-switch sees the real edge/cost) --------
    def test_fill_price_pnl_booked(self, eng):
        sub = FakeSubmitter()
        self._place(eng, sub)                       # entry mid 0.50
        # bought 50 @ 0.49 vs mid 0.50 -> +0.5 mark
        eng.accrue_maker_rewards_live(
            submitter=sub,
            fills_by_token={"tok_yes": [{"side": "BUY", "size": 50, "price": 0.49}]})
        assert eng.get_maker_summary()["inventory_pnl"] > 0

    # -- M2: real inventory not clamped; quote goes one-sided at the cap -----------
    def test_inventory_not_clamped_past_cap(self, eng):
        sub = FakeSubmitter()
        self._place(eng, sub, max_inventory=50.0)   # cap = 50 shares
        rows = eng.accrue_maker_rewards_live(
            submitter=sub,
            fills_by_token={"tok_yes": [{"side": "BUY", "size": 60, "price": 0.50}]})
        assert rows[0]["inventory"] == 60.0          # true position, NOT clamped to 50

    def test_min_notional_rejected(self, eng):
        # at mid 0.99 the YES-ask / NO leg notional is ~0 -> below the $1 exchange min
        from pm_trader.models import OrderRejectedError
        _mock_api(eng, mid=0.99)
        with pytest.raises(OrderRejectedError):
            eng.place_maker_quote_live("0xabc", submitter=FakeSubmitter(), half_spread_cents=1.0)

    def test_stale_accrual_clamped(self, eng):
        sub = FakeSubmitter()
        self._place(eng, sub)
        # simulate a restart after long downtime: last_accrued_at far in the past
        eng.db.conn.execute("UPDATE maker_quotes SET last_accrued_at = ?",
                            ("2020-01-01T00:00:00+00:00",))
        eng.db.conn.commit()
        rows = eng.accrue_maker_rewards_live(submitter=sub, fills_by_token={})
        assert rows[0]["seconds"] == 600.0   # clamped to MAX_ACCRUAL_SECONDS, not ~days

    def test_crossing_cost_debited_on_exit(self, eng):
        from pm_trader.orders import get_active_maker_quotes
        self._place(eng, FakeSubmitter())
        quote_obj = get_active_maker_quotes(eng.db.conn)[0]
        cash_before = eng.get_account().cash
        results = []
        eng._exit_maker_quote(quote_obj, 0.50, "test", results, crossing_cost=5.0)
        cash_after = eng.get_account().cash
        assert cash_after == pytest.approx(cash_before + quote_obj.committed_capital - 5.0)
        assert results[0]["crossing_cost"] == 5.0

    def test_recenter_hysteresis_holds(self, eng):
        sub = FakeSubmitter()
        self._place(eng, sub)                 # entry 0.50
        sub.calls.clear()
        eng.api.get_midpoint = MagicMock(return_value=0.52)   # 2-tick move
        eng.accrue_maker_rewards_live(submitter=sub, fills_by_token={}, recenter_ticks=3)
        assert sub.calls == []                # 2 ticks < hysteresis 3 -> no churn

    def test_recenter_hysteresis_fires(self, eng):
        sub = FakeSubmitter()
        self._place(eng, sub)
        sub.calls.clear()
        eng.api.get_midpoint = MagicMock(return_value=0.53)   # 3-tick move
        eng.accrue_maker_rewards_live(submitter=sub, fills_by_token={}, recenter_ticks=3)
        assert any(c["action"] == "CANCEL_ALL" for c in sub.calls)   # fires at 3 ticks

    def test_one_sided_quote_at_cap(self, eng):
        sub = FakeSubmitter()
        self._place(eng, sub, max_inventory=50.0)
        sub.calls.clear()
        eng.api.get_midpoint = MagicMock(return_value=0.52)   # tick move -> re-center
        eng.accrue_maker_rewards_live(
            submitter=sub,
            fills_by_token={"tok_yes": [{"side": "BUY", "size": 60, "price": 0.50}]})
        places = sub.places()
        assert len(places) == 1 and places[0]["side"] == "SELL"  # only the flattening side
