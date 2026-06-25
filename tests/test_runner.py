"""Tests for the autonomous engine-driven runner (no network, no SDK, no MCP)."""

from __future__ import annotations

from pm_trader.runner import LiveRunner, RunnerConfig, load_dotenv


def _scan_pool(token, q, *, daily=400.0, share=0.2, min_size=50.0):
    return {
        "question": q, "condition_id": "0x" + token, "token": token,
        "daily": daily, "share": share, "min_size": min_size, "tick": 0.01,
        "max_spread_c": 4.5, "jump_verdict": "SAFE", "empty_band": False,
        "days_wiped": 2.0, "reward_per_day": share * daily,
    }


class FakeEngine:
    """Stand-in for Engine: records placements + returns canned accrue rows."""

    def __init__(self, *, summary=None, accrue_rows=None):
        self.placed = []          # (condition_id, half_spread, live?)
        self._summary = summary or {"inventory_pnl": 0.0}
        self._rows = accrue_rows or []
        self.last_fills = None
        self.closed = False
        self.cancelled = []
        self.quotes = []

    def init_account(self, balance):
        self.balance = balance

    def place_maker_quote(self, cond, *, half_spread_cents=None):
        self.placed.append((cond, half_spread_cents, False))
        return {"token_id": "tok_" + cond}

    def place_maker_quote_live(self, cond, *, submitter, half_spread_cents=None):
        self.placed.append((cond, half_spread_cents, True))
        return {"token_id": "tok_" + cond}

    def accrue_maker_rewards(self):
        return self._rows

    def accrue_maker_rewards_live(self, *, submitter, fills_by_token=None):
        self.last_fills = fills_by_token
        return self._rows

    def get_maker_summary(self):
        return self._summary

    def get_maker_quotes(self):
        return self.quotes

    def cancel_maker_quote(self, qid):
        self.cancelled.append(qid)

    def _flatten_live(self, submitter, token, inv):
        pass

    def close(self):
        self.closed = True


class FakeScanner:
    def __init__(self, markets=None):
        self.markets = markets or []
        self.closed = False

    def sampling_markets(self, *, max_pages=100):
        return self.markets

    def book(self, t):
        return {}

    def prices_history(self, t, **k):
        return []

    def close(self):
        self.closed = True


class FakeSubmitter:
    def __init__(self, fills=None):
        self.calls = []
        self._fills = fills or []

    def __call__(self, action):
        self.calls.append(action)
        return {"status": "OK", **action}

    def poll_fills(self):
        f, self._fills = self._fills, []
        return f


def _runner(*, engine=None, submitter=None, **cfg):
    return LiveRunner(RunnerConfig(**cfg), engine=engine or FakeEngine(),
                      scanner_client=FakeScanner(), submitter=submitter,
                      sleeper=lambda _s: None)


class TestSelection:
    def test_reselect_places_new_pools(self):
        eng = FakeEngine()
        r = _runner(engine=eng, capital=10_000.0, max_pools=3)
        r.report = {"pools": [_scan_pool("a", "Alpha"), _scan_pool("b", "Bravo")]}
        r.reselect()
        assert {p[0] for p in eng.placed} == {"0xa", "0xb"}
        assert set(r.placed) == {"0xa", "0xb"}

    def test_reselect_skips_already_placed(self):
        eng = FakeEngine()
        r = _runner(engine=eng, capital=10_000.0, max_pools=3)
        r.report = {"pools": [_scan_pool("a", "Alpha")]}
        r.reselect()
        r.reselect()  # second pass should NOT re-place 'a'
        assert len(eng.placed) == 1

    def test_reselect_skips_cooldown(self):
        eng = FakeEngine()
        r = _runner(engine=eng, capital=10_000.0, max_pools=3)
        r.report = {"pools": [_scan_pool("a", "Alpha"), _scan_pool("b", "Bravo")]}
        r.cooldown = {"0xa": 2}
        r.reselect()
        assert {p[0] for p in eng.placed} == {"0xb"}

    def test_live_uses_live_placement(self):
        eng = FakeEngine()
        r = _runner(engine=eng, submitter=FakeSubmitter(), capital=10_000.0, live=True)
        r.report = {"pools": [_scan_pool("a", "Alpha")]}
        r.reselect()
        assert eng.placed[0][2] is True  # placed via place_maker_quote_live


class TestPoll:
    def test_paper_mode_uses_simulator(self):
        eng = FakeEngine(accrue_rows=[{"quote": {"market_condition_id": "0xa"}, "reward": 0.1}])
        r = _runner(engine=eng, dry_live=False)   # PAPER simulator
        rows = r.poll_once()
        assert rows and eng.last_fills is None     # accrue_maker_rewards, no fills polled

    def test_dry_live_rehearses_live_path(self):
        from pm_trader.maker_live import DryRunSubmitter
        eng = FakeEngine()
        r = _runner(engine=eng, submitter=DryRunSubmitter(), dry_live=True, capital=10_000.0)
        r.report = {"pools": [_scan_pool("a", "Alpha")]}
        r.reselect()
        assert eng.placed[0][2] is True            # placed via place_maker_quote_live
        r.poll_once()
        assert eng.last_fills == {}                # accrue_maker_rewards_live ran; no real fills

    def test_live_polls_fills_grouped_by_token(self):
        eng = FakeEngine()
        sub = FakeSubmitter(fills=[{"token_id": "tok_0xa", "side": "BUY", "size": 50, "price": 0.49},
                                   {"token_id": "tok_0xa", "side": "SELL", "size": 10, "price": 0.51}])
        r = _runner(engine=eng, submitter=sub, live=True)
        r.poll_once()
        assert set(eng.last_fills) == {"tok_0xa"}
        assert len(eng.last_fills["tok_0xa"]) == 2

    def test_reconcile_exit_removes_and_no_cooldown(self):
        rows = [{"quote": {"market_condition_id": "0xa"}, "reconciled": "rewards_ended"}]
        eng = FakeEngine(accrue_rows=rows)
        r = _runner(engine=eng)
        r.placed = {"0xa": {}}
        r.poll_once()
        assert "0xa" not in r.placed and "0xa" not in r.cooldown

    def test_drift_exit_sets_cooldown(self):
        rows = [{"quote": {"market_condition_id": "0xa"}, "reconciled": "drift_exit"}]
        eng = FakeEngine(accrue_rows=rows)
        r = _runner(engine=eng, cooldown_rounds=3)
        r.placed = {"0xa": {}}
        r.poll_once()
        assert "0xa" not in r.placed and r.cooldown.get("0xa") == 3

    def test_exit_failed_keeps_pool_tracked(self):
        # engine couldn't cancel/flatten -> quote still active; runner must NOT drop it
        rows = [{"quote": {"market_condition_id": "0xa"}, "exit_failed": "drift_exit"}]
        eng = FakeEngine(accrue_rows=rows)
        r = _runner(engine=eng)
        r.placed = {"0xa": {}}
        r.poll_once()
        assert "0xa" in r.placed and "0xa" not in r.cooldown


class TestKillSwitch:
    def test_kill_on_inventory_loss(self):
        eng = FakeEngine(summary={"inventory_pnl": -25.0})
        r = _runner(engine=eng, max_loss=20.0)
        assert r._kill_check() is not None

    def test_clean(self):
        eng = FakeEngine(summary={"inventory_pnl": -5.0})
        r = _runner(engine=eng, max_loss=20.0)
        assert r._kill_check() is None

    def test_kill_file(self, tmp_path):
        r = _runner(max_loss=1e9)
        r.cfg.state_dir = str(tmp_path)
        (tmp_path / "KILL").write_text("")
        assert r._kill_check() == "kill-file"

    def test_trip_kill_idempotent(self):
        r = _runner()
        r.trip_kill("a")
        r.trip_kill("b")
        assert r._stop and r._kill_reason == "a"


class TestCooldown:
    def test_tick_expires(self):
        r = _runner()
        r.cooldown = {"x": 1}
        r._tick_cooldowns()
        assert "x" not in r.cooldown


class TestReevaluateHeld:
    """Continuous re-evaluation: degradation exit + opportunity rotation."""

    def _fresh(self, *, daily=400.0, share=0.2, verdict="SAFE", empty=False, rpd=None):
        return {"daily": daily, "share": share, "jump_verdict": verdict,
                "empty_band": empty, "reward_per_day": rpd if rpd is not None else share * daily}

    # -- pure degradation policy --------------------------------------------
    def test_keeps_a_healthy_pool(self):
        r = _runner(min_daily=80.0, min_share=0.02)
        assert r._degrade_reason(self._fresh()) is None

    def test_exits_on_daily_cut(self):
        r = _runner(min_daily=80.0)
        assert r._degrade_reason(self._fresh(daily=50.0)) == "daily_cut"

    def test_exits_on_rewards_ended(self):
        r = _runner()
        assert r._degrade_reason({"daily": 0.0}) == "rewards_ended"

    def test_exits_on_jump_risk_rise(self):
        r = _runner()
        assert r._degrade_reason(self._fresh(verdict="WATCH")) == "jump_risk_rose"

    def test_exits_on_share_collapse(self):
        r = _runner(min_share=0.05)
        assert r._degrade_reason(self._fresh(share=0.01)) == "share_collapsed"

    def test_exits_on_empty_band(self):
        r = _runner()
        assert r._degrade_reason(self._fresh(empty=True)) == "empty_band"

    # -- rotation is handled by reselect converging to the ideal selection ---
    def test_reselect_drops_pool_no_longer_ideal(self):
        eng = FakeEngine()
        eng.quotes = [{"id": 9, "market_condition_id": "0xa", "token_id": "tok_a",
                       "inventory": 0.0}]
        r = _runner(engine=eng, capital=10_000.0, max_pools=3)
        r.placed = {"0xa": {"token": "tok_a"}}
        r.placed_at = {"0xa": 0.0}
        r.report = {"pools": [_scan_pool("b", "Bravo")]}   # 0xa no longer ideal
        r.reselect()
        assert "0xa" not in r.placed and 9 in eng.cancelled   # dropped + cancelled
        assert "0xb" in r.placed                              # better pool funded
        assert "0xa" not in r.cooldown                        # rank-out -> may return

    def test_reselect_keeps_still_ideal_pool(self):
        eng = FakeEngine()
        eng.quotes = [{"id": 9, "market_condition_id": "0xa", "token_id": "tok_a",
                       "inventory": 0.0}]
        r = _runner(engine=eng, capital=10_000.0, max_pools=3)
        r.placed = {"0xa": {"token": "tok_a"}}
        r.placed_at = {"0xa": 0.0}
        r.report = {"pools": [_scan_pool("a", "Alpha")]}   # still ideal
        r.reselect()
        assert "0xa" in r.placed and eng.cancelled == []   # no churn

    # -- orchestration (engine-driven, all modes) ----------------------------
    def _held_engine(self):
        eng = FakeEngine()
        eng.quotes = [{"id": 1, "market_condition_id": "0xa", "token_id": "tok_a",
                       "inventory": 0.0}]
        return eng

    def test_exits_degraded_held_pool(self, monkeypatch):
        eng = self._held_engine()
        r = _runner(engine=eng, min_hold_s=0.0)
        r.placed = {"0xa": {"token": "tok_a"}}
        r.placed_at = {"0xa": 0.0}
        monkeypatch.setattr(r, "_rescore", lambda c, t: self._fresh(daily=10.0))  # cut
        r.reevaluate_held()
        assert 1 in eng.cancelled and "0xa" not in r.placed

    def test_respects_min_hold(self, monkeypatch):
        import time as _t
        eng = self._held_engine()
        r = _runner(engine=eng, min_hold_s=1e9)
        r.placed = {"0xa": {"token": "tok_a"}}
        r.placed_at = {"0xa": _t.monotonic()}   # just placed
        monkeypatch.setattr(r, "_rescore", lambda c, t: self._fresh(daily=10.0))
        r.reevaluate_held()
        assert eng.cancelled == [] and "0xa" in r.placed   # too young to soft-exit

    def test_keeps_healthy_held_pool(self, monkeypatch):
        eng = self._held_engine()
        r = _runner(engine=eng, min_hold_s=0.0)
        r.placed = {"0xa": {"token": "tok_a"}}
        r.placed_at = {"0xa": 0.0}
        monkeypatch.setattr(r, "_rescore", lambda c, t: self._fresh())  # healthy
        r.reevaluate_held()
        assert eng.cancelled == [] and "0xa" in r.placed

    def test_live_exit_cancels_and_flattens(self, monkeypatch):
        eng = self._held_engine()
        eng.quotes[0]["inventory"] = 30.0
        sub = FakeSubmitter()
        r = _runner(engine=eng, submitter=sub, live=True, min_hold_s=0.0)
        r.placed = {"0xa": {"token": "tok_a"}}
        r.placed_at = {"0xa": 0.0}
        monkeypatch.setattr(r, "_rescore", lambda c, t: self._fresh(verdict="KILL"))
        r.reevaluate_held()
        assert any(c["action"] == "CANCEL_ALL" for c in sub.calls)
        assert 1 in eng.cancelled and r.cooldown.get("0xa") == r.cfg.cooldown_rounds

    def test_disabled_noop(self, monkeypatch):
        eng = self._held_engine()
        r = _runner(engine=eng, reeval_enabled=False)
        r.placed = {"0xa": {"token": "tok_a"}}
        r.placed_at = {"0xa": 0.0}
        called = {"n": 0}
        monkeypatch.setattr(r, "_rescore", lambda c, t: called.__setitem__("n", 1))
        r.reevaluate_held()
        assert called["n"] == 0 and eng.cancelled == []


class TestRunLoop:
    def test_run_stops_on_kill(self):
        eng = FakeEngine()
        r = LiveRunner(RunnerConfig(discovery_interval_s=1e9), engine=eng,
                       scanner_client=FakeScanner(markets=[]), sleeper=None)
        n = {"i": 0}

        def _sleeper(_s):
            n["i"] += 1
            r.trip_kill("test-stop")
        r._sleep_fn = _sleeper
        r.run()
        assert r._stop and eng.closed and n["i"] == 1


def test_config_gate(monkeypatch):
    monkeypatch.delenv("PM_TRADER_LIVE", raising=False)
    assert RunnerConfig.from_env().live is False
    monkeypatch.setenv("PM_TRADER_LIVE", "1")
    assert RunnerConfig.from_env().live is True
    monkeypatch.setenv("PM_TRADER_LIVE", "yes")
    assert RunnerConfig.from_env().live is False


def test_load_dotenv(tmp_path, monkeypatch):
    monkeypatch.delenv("LM_X", raising=False)
    (tmp_path / ".env").write_text('LM_X="hi"\n')
    load_dotenv(tmp_path / ".env")
    import os
    assert os.environ["LM_X"] == "hi"
