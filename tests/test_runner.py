"""Tests for the runner: selection sync, event->decision->execution, fills/MTM,
reconcile-driven exit, cooldown, and the kill-switch. No network, no SDK."""

from __future__ import annotations

import os

from live_maker.config import Config
from live_maker.models import EXIT, HOLD, QUOTE, BookEvent, Decision, FillEvent
from live_maker.runner import Runner
from live_maker.strategy import PoolMaker
from tests.conftest import FakeExecution, book


def _scan_pool(token, question, *, daily=400.0, share=0.2):
    return {
        "question": question, "condition_id": "0x" + token, "token": token,
        "daily": daily, "share": share, "min_size": 50.0, "tick": 0.01,
        "max_spread_c": 4.5, "jump_verdict": "SAFE", "empty_band": False,
        "days_wiped": 2.0, "reward_per_day": share * daily,
    }


def _runner(exe=None, **cfg_kw):
    cfg = Config(**cfg_kw)
    return Runner(cfg, execution=exe or FakeExecution())


class TestSelectAndSync:
    async def test_adds_selected_pools(self):
        r = _runner(capital=10_000.0, max_pools=3)
        r.safe_report = {"pools": [_scan_pool("a", "Event alpha"),
                                   _scan_pool("b", "Event beta")]}
        await r._select_and_sync()
        assert set(r.makers) == {"a", "b"}
        assert r.daily_rate["a"] == 400.0
        assert isinstance(r.makers["a"], PoolMaker)

    async def test_drops_deselected_and_cancels(self):
        exe = FakeExecution()
        r = _runner(exe, capital=10_000.0, max_pools=3)
        r.safe_report = {"pools": [_scan_pool("a", "Event alpha")]}
        await r._select_and_sync()
        assert "a" in r.makers
        # next scan no longer has 'a'
        r.safe_report = {"pools": [_scan_pool("b", "Event beta")]}
        await r._select_and_sync()
        assert "a" not in r.makers and "b" in r.makers
        assert ("cancel_all", "a") in exe.calls


class TestExecute:
    async def test_quote_cancels_then_places(self):
        exe = FakeExecution()
        r = _runner(exe)
        d = Decision(token_id="tok", action=QUOTE, mid=0.50, cancel_first=True,
                     orders=[{"side": "BUY", "price": 0.49, "size": 50},
                             {"side": "SELL", "price": 0.51, "size": 50}])
        await r._execute(d)
        kinds = [c[0] for c in exe.calls]
        assert kinds == ["cancel_all", "place", "place"]

    async def test_hold_does_nothing(self):
        exe = FakeExecution()
        r = _runner(exe)
        await r._execute(Decision(token_id="tok", action=HOLD, mid=0.50))
        assert exe.calls == []

    async def test_exit_cancels_flattens_retires_cooldown(self):
        exe = FakeExecution()
        r = _runner(exe)
        r.makers["tok"] = PoolMaker(token_id="tok", max_spread_c=4.5, min_size=50, tick=0.01)
        r.meta["tok"] = {"condition_id": "0xtok"}
        r.inventory["tok"] = 50.0
        await r._execute(Decision(token_id="tok", action=EXIT, mid=0.50,
                                  reason="jump", cooldown=True, inventory=50.0))
        assert ("cancel_all", "tok") in exe.calls
        assert ("flatten", "tok", 50.0) in exe.calls
        assert "tok" not in r.makers
        assert r.cooldown.get("0xtok") == r.cfg.cooldown_rounds


class TestEventRouting:
    async def test_book_event_quotes_first_time(self):
        exe = FakeExecution()
        r = _runner(exe)
        r.makers["a"] = PoolMaker(token_id="a", max_spread_c=4.5, min_size=50, tick=0.01)
        r.daily_rate["a"] = 400.0
        await r._on_event(BookEvent(token_id="a", book=book(), source="ws"))
        assert any(c[0] == "place" for c in exe.calls)
        assert r.last_book["a"] is not None

    async def test_unknown_token_ignored(self):
        exe = FakeExecution()
        r = _runner(exe)
        await r._on_event(BookEvent(token_id="ghost", book=book()))
        assert exe.calls == []

    async def test_bad_mid_no_action(self):
        exe = FakeExecution()
        r = _runner(exe)
        r.makers["a"] = PoolMaker(token_id="a", max_spread_c=4.5, min_size=50, tick=0.01)
        r.daily_rate["a"] = 400.0
        # mid 0/1 invalid -> skip
        await r._decide_and_act("a", book(0.0, 0.0), 0.0)
        assert exe.calls == []


class TestFillsAndMtm:
    async def test_fill_updates_inventory_and_cashflow(self):
        r = _runner()
        r.makers["a"] = PoolMaker(token_id="a", max_spread_c=4.5, min_size=50, tick=0.01)
        r.last_book["a"] = book()  # mid 0.50
        r._apply_fill(FillEvent(token_id="a", side="BUY", size=50, price=0.50))
        assert r.inventory["a"] == 50.0
        assert r.cash_flow["a"] == -25.0  # paid 50*0.50

    def test_book_mtm_loss(self):
        r = _runner()
        r.makers["a"] = PoolMaker(token_id="a", max_spread_c=4.5, min_size=50, tick=0.01)
        r.inventory["a"] = 50.0
        r.cash_flow["a"] = -25.0      # bought 50 @ 0.50
        r.last_book["a"] = book(0.39, 0.41)  # mid drops to 0.40
        assert r.book_mtm() == -5.0   # 50*0.40 - 25 = -5

    async def test_fill_unknown_token_ignored(self):
        r = _runner()
        r._apply_fill(FillEvent(token_id="ghost", side="BUY", size=50, price=0.50))
        assert r.inventory == {}


class TestKillSwitch:
    def test_trip_kill_sets_stop(self):
        r = _runner()
        r.trip_kill("test")
        assert r._stop.is_set() and r._kill_reason == "test"

    def test_kill_check_max_loss(self):
        r = _runner(max_loss_per_day=4.0)
        r.makers["a"] = PoolMaker(token_id="a", max_spread_c=4.5, min_size=50, tick=0.01)
        r.inventory["a"] = 50.0
        r.cash_flow["a"] = -25.0
        r.last_book["a"] = book(0.39, 0.41)  # mtm -5 < -4
        assert r._kill_check() is not None

    def test_kill_check_clean(self):
        r = _runner(max_loss_per_day=100.0)
        assert r._kill_check() is None

    def test_kill_check_file(self, tmp_path):
        r = _runner(max_loss_per_day=100.0)
        r.cfg.state_dir = str(tmp_path)
        (tmp_path / "KILL").write_text("")
        assert r._kill_check() == "kill-file"

    async def test_kill_switch_loop_trips_on_loss(self):
        r = _runner(max_loss_per_day=4.0)
        r.makers["a"] = PoolMaker(token_id="a", max_spread_c=4.5, min_size=50, tick=0.01)
        r.inventory["a"] = 50.0
        r.cash_flow["a"] = -25.0
        r.last_book["a"] = book(0.39, 0.41)  # mtm -5 < -4 -> should trip

        async def _nosleep(_):
            return
        r._sleep = _nosleep  # type: ignore[assignment]
        import asyncio
        await asyncio.wait_for(r._kill_switch_loop(), timeout=1.0)
        assert r._stop.is_set() and "max-loss" in r._kill_reason


class TestReconcileDrivenExit:
    async def test_zero_daily_rate_exits_pool(self):
        exe = FakeExecution()
        r = _runner(exe)
        r.makers["a"] = PoolMaker(token_id="a", max_spread_c=4.5, min_size=50, tick=0.01)
        r.meta["a"] = {"condition_id": "0xa"}
        r.daily_rate["a"] = 0.0          # reconcile found the pool left the program
        await r._decide_and_act("a", book(), 0.50)
        assert ("cancel_all", "a") in exe.calls
        assert "a" not in r.makers       # retired
