"""Tests for the autonomous polling runner (no network, no SDK, no MCP)."""

from __future__ import annotations

from pm_trader.runner import LiveRunner, RunnerConfig, load_dotenv


def _scan_pool(token, q, *, daily=400.0, share=0.2, min_size=50.0):
    return {
        "question": q, "condition_id": "0x" + token, "token": token,
        "daily": daily, "share": share, "min_size": min_size, "tick": 0.01,
        "max_spread_c": 4.5, "jump_verdict": "SAFE", "empty_band": False,
        "days_wiped": 2.0, "reward_per_day": share * daily,
    }


def _raw_book(bid=0.49, ask=0.51, size=1000.0):
    return {"bids": [{"price": bid, "size": size}], "asks": [{"price": ask, "size": size}]}


class FakeClient:
    """Stand-in for RewardsClient: serves books + an (optional) scan universe."""

    def __init__(self, books=None, markets=None, histories=None):
        self.books = books or {}
        self.markets = markets or []
        self.histories = histories or {}
        self.closed = False

    def sampling_markets(self, *, max_pages=100):
        return self.markets

    def book(self, token):
        return self.books.get(token, {})

    def prices_history(self, token, *, interval="max", fidelity=1440):
        return self.histories.get(token, [])

    def close(self):
        self.closed = True


class FakeSubmitter:
    """Callable like a real submitter; records actions + serves canned fills."""

    def __init__(self, fills=None):
        self.calls = []
        self._fills = fills or []

    def __call__(self, action):
        self.calls.append(action)
        return {"status": "OK", **action}

    def poll_fills(self):
        f, self._fills = self._fills, []
        return f


def _runner(*, books=None, submitter=None, **cfg):
    client = FakeClient(books=books or {})
    return LiveRunner(RunnerConfig(**cfg), client=client, submitter=submitter,
                      sleeper=lambda _s: None)


class TestSelection:
    def test_reselect_builds_portfolio(self):
        r = _runner(capital=10_000.0, max_pools=3)
        r.report = {"pools": [_scan_pool("a", "Alpha"), _scan_pool("b", "Bravo")]}
        r.reselect()
        assert set(r.portfolio.bots) == {"a", "b"}

    def test_reselect_cancels_dropped(self):
        sub = FakeSubmitter()
        r = _runner(capital=10_000.0, max_pools=3, live=True, submitter=sub)
        r.report = {"pools": [_scan_pool("a", "Alpha")]}
        r.reselect()
        r.report = {"pools": [_scan_pool("b", "Bravo")]}
        r.reselect()
        assert any(c == {"action": "CANCEL_ALL", "token_id": "a"} for c in sub.calls)


class TestPoll:
    def test_poll_steps_each_bot(self):
        r = _runner(books={"a": _raw_book(), "b": _raw_book()},
                    capital=10_000.0, max_pools=3)
        r.report = {"pools": [_scan_pool("a", "Alpha"), _scan_pool("b", "Bravo")]}
        r.reselect()
        plans = r.poll_once()
        assert len(plans) == 2
        assert r.last_mid["a"] == 0.50

    def test_poll_empty_when_no_selection(self):
        r = _runner()
        assert r.poll_once() == []


class TestLiveFills:
    def test_real_fills_update_inventory_and_cashflow(self):
        sub = FakeSubmitter(fills=[{"token_id": "a", "side": "BUY", "size": 50,
                                    "price": 0.50, "id": "t1"}])
        r = _runner(capital=10_000.0, live=True, submitter=sub)
        r.report = {"pools": [_scan_pool("a", "Alpha")]}
        r.reselect()
        r.last_mid["a"] = 0.50
        r._apply_real_fills()
        assert r.portfolio.bots["a"].inventory == 50.0
        assert r.cash_flow["a"] == -25.0


class TestRetireAndCooldown:
    def test_retire_sets_cooldown(self):
        r = _runner(capital=10_000.0)
        r.report = {"pools": [_scan_pool("a", "Alpha")]}
        r.reselect()
        r._retire("a", cooldown=True, reason="jump")
        assert "a" not in r.portfolio.bots
        assert r.cooldown.get("0xa") == r.cfg.cooldown_rounds

    def test_cooldown_excludes_on_reselect(self):
        r = _runner(capital=10_000.0)
        r.report = {"pools": [_scan_pool("a", "Alpha"), _scan_pool("b", "Bravo")]}
        r.cooldown = {"0xa": 2, "a": 2}
        r.reselect()
        assert "a" not in r.portfolio.bots and "b" in r.portfolio.bots

    def test_tick_cooldowns_expire(self):
        r = _runner()
        r.cooldown = {"x": 1}
        r._tick_cooldowns()
        assert "x" not in r.cooldown


class TestKillSwitch:
    def test_book_mtm_loss(self):
        r = _runner(capital=10_000.0)
        r.report = {"pools": [_scan_pool("a", "Alpha")]}
        r.reselect()
        r.portfolio.bots["a"].inventory = 50.0
        r.cash_flow["a"] = -25.0
        r.last_mid["a"] = 0.40
        assert r.book_mtm() == -5.0

    def test_kill_check_max_loss(self):
        r = _runner(capital=10_000.0, max_loss=4.0)
        r.report = {"pools": [_scan_pool("a", "Alpha")]}
        r.reselect()
        r.portfolio.bots["a"].inventory = 50.0
        r.cash_flow["a"] = -25.0
        r.last_mid["a"] = 0.40   # mtm -5 < -4
        assert r._kill_check() is not None

    def test_kill_check_clean(self):
        r = _runner(max_loss=100.0)
        assert r._kill_check() is None

    def test_kill_file(self, tmp_path):
        r = _runner(max_loss=100.0)
        r.cfg.state_dir = str(tmp_path)
        (tmp_path / "KILL").write_text("")
        assert r._kill_check() == "kill-file"

    def test_trip_kill_idempotent(self):
        r = _runner()
        r.trip_kill("a")
        r.trip_kill("b")
        assert r._stop is True and r._kill_reason == "a"


class TestRunLoop:
    def test_run_stops_on_kill(self):
        # empty universe -> empty book; sleeper trips the kill so run() returns
        client = FakeClient(markets=[])
        r = LiveRunner(RunnerConfig(capital=200.0, discovery_interval_s=1e9),
                       client=client, sleeper=None)
        calls = {"n": 0}

        def _sleeper(_s):
            calls["n"] += 1
            r.trip_kill("test-stop")
        r._sleep_fn = _sleeper
        r.run()
        assert r._stop is True and client.closed is True and calls["n"] == 1


def test_config_from_env_gate(monkeypatch):
    monkeypatch.delenv("PM_TRADER_LIVE", raising=False)
    assert RunnerConfig.from_env().live is False
    monkeypatch.setenv("PM_TRADER_LIVE", "1")
    assert RunnerConfig.from_env().live is True
    monkeypatch.setenv("PM_TRADER_LIVE", "yes")
    assert RunnerConfig.from_env().live is False


def test_load_dotenv(tmp_path, monkeypatch):
    monkeypatch.delenv("LM_X", raising=False)
    (tmp_path / ".env").write_text('LM_X="hi"\n# c\n')
    load_dotenv(tmp_path / ".env")
    import os
    assert os.environ["LM_X"] == "hi"
