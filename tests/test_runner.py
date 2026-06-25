"""Tests for the autonomous engine-driven runner (no network, no SDK, no MCP)."""

from __future__ import annotations

import time

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
        self._has_account = True

    def get_account(self):
        if not getattr(self, "_has_account", False):
            from pm_trader.models import NotInitializedError
            raise NotInitializedError()
        return object()

    def place_maker_quote(self, cond, *, half_spread_cents=None, size=None):
        self.placed.append((cond, half_spread_cents, False))
        self.placed_size = size
        return {"token_id": "tok_" + cond}

    def place_maker_quote_live(self, cond, *, submitter, half_spread_cents=None, size=None):
        self.placed.append((cond, half_spread_cents, True))
        self.placed_size = size
        return {"token_id": "tok_" + cond}

    def accrue_maker_rewards(self):
        return self._rows

    def accrue_maker_rewards_live(self, *, submitter, fills_by_token=None,
                                  recenter_ticks=1, force_recenter=None):
        self.last_fills = fills_by_token
        self.last_recenter_ticks = recenter_ticks
        self.last_force_recenter = force_recenter
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
    import time as _t
    r = LiveRunner(RunnerConfig(**cfg), engine=engine or FakeEngine(),
                   scanner_client=FakeScanner(), submitter=submitter,
                   sleeper=lambda _s: None)
    r._last_scan_ok = _t.monotonic()   # tests drive reselect() directly (not stale)
    return r


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

    def test_wallet_floor_trips_kill(self):
        class BalSub(FakeSubmitter):
            def usdc_balance(self):
                return 5.0
        eng = FakeEngine(summary={"inventory_pnl": 0.0})
        r = _runner(engine=eng, submitter=BalSub(), live=True,
                    min_wallet_usdc=10.0, max_loss=1e9)
        assert "wallet-floor" in (r._kill_check() or "")

    def test_wallet_floor_clean_above(self):
        class BalSub(FakeSubmitter):
            def usdc_balance(self):
                return 100.0
        eng = FakeEngine(summary={"inventory_pnl": 0.0})
        r = _runner(engine=eng, submitter=BalSub(), live=True,
                    min_wallet_usdc=10.0, max_loss=1e9)
        assert r._kill_check() is None


class TestStaleness:
    def test_stale_report_skips_reselect(self):
        eng = FakeEngine()
        r = _runner(engine=eng, capital=10_000.0)
        r._last_scan_ok = None            # never had a good scan
        r.report = {"pools": [_scan_pool("a", "Alpha")]}
        r.reselect()
        assert eng.placed == []           # don't place into a stale/empty universe


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
        r = _runner(min_daily=80.0)
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

    def test_exits_on_reward_collapse(self):
        r = _runner(min_pool_reward=1.0)
        # 0.2% share of a $300 pool = $0.60/day < $1 floor -> exit
        assert r._degrade_reason(self._fresh(share=0.002, daily=300.0)) == "reward_collapsed"

    def test_keeps_small_share_of_big_pool(self):
        r = _runner(min_pool_reward=1.0)
        # 1.1% share of a $618 pool = $6.80/day -> KEEP despite tiny share
        # (the old 2%-share floor would have wrongly churned this profitable pool)
        assert r._degrade_reason(self._fresh(share=0.011, daily=618.0)) is None

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

    def test_reselect_skips_dead_reward_pool_but_keeps_small_share_big_pool(self):
        eng = FakeEngine()
        r = _runner(engine=eng, capital=10_000.0, max_pools=3, min_pool_reward=0.5)
        r.report = {"pools": [
            _scan_pool("a", "Alpha", daily=400.0, share=0.20),    # $80/day -> keep
            _scan_pool("b", "Beta", daily=300.0, share=0.018),    # crowded but $5.4/day -> KEEP
            _scan_pool("c", "Gamma", daily=100.0, share=0.001)]}  # $0.10/day dead -> skip
        r.reselect()
        assert "0xa" in r.placed and "0xb" in r.placed   # small share of a big pool still pays
        assert "0xc" not in r.placed                     # est reward < $0.50/day -> skipped

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
        monkeypatch.setattr(r, "_rescore", lambda c, t, **kw: self._fresh(daily=10.0))  # cut
        r.reevaluate_held()
        assert 1 in eng.cancelled and "0xa" not in r.placed

    def test_respects_min_hold(self, monkeypatch):
        import time as _t
        eng = self._held_engine()
        r = _runner(engine=eng, min_hold_s=1e9)
        r.placed = {"0xa": {"token": "tok_a"}}
        r.placed_at = {"0xa": _t.monotonic()}   # just placed
        monkeypatch.setattr(r, "_rescore", lambda c, t, **kw: self._fresh(daily=10.0))
        r.reevaluate_held()
        assert eng.cancelled == [] and "0xa" in r.placed   # too young to soft-exit

    def test_keeps_healthy_held_pool(self, monkeypatch):
        eng = self._held_engine()
        r = _runner(engine=eng, min_hold_s=0.0)
        r.placed = {"0xa": {"token": "tok_a"}}
        r.placed_at = {"0xa": 0.0}
        monkeypatch.setattr(r, "_rescore", lambda c, t, **kw: self._fresh())  # healthy
        r.reevaluate_held()
        assert eng.cancelled == [] and "0xa" in r.placed

    def test_live_exit_cancels_and_flattens(self, monkeypatch):
        eng = self._held_engine()
        eng.quotes[0]["inventory"] = 30.0
        sub = FakeSubmitter()
        r = _runner(engine=eng, submitter=sub, live=True, min_hold_s=0.0)
        r.placed = {"0xa": {"token": "tok_a"}}
        r.placed_at = {"0xa": 0.0}
        monkeypatch.setattr(r, "_rescore", lambda c, t, **kw: self._fresh(verdict="KILL"))
        r.reevaluate_held()
        assert any(c["action"] == "CANCEL_ALL" for c in sub.calls)
        assert 1 in eng.cancelled and r.cooldown.get("0xa") == r.cfg.cooldown_rounds

    def test_exits_fast_book_on_high_velocity(self, monkeypatch):
        import collections
        import time as _t
        eng = self._held_engine()
        r = _runner(engine=eng, min_hold_s=0.0, max_mid_vel_cps=2.0)
        r.placed = {"0xa": {"token": "tok_a"}}
        r.placed_at = {"0xa": 0.0}
        monkeypatch.setattr(r, "_rescore", lambda c, t, **kw: self._fresh())  # healthy
        now = _t.monotonic()
        r._mid_hist["tok_a"] = collections.deque(             # 10c swings in 1s = 10c/s
            [(now, 0.50), (now + 0.5, 0.55), (now + 1.0, 0.50)], maxlen=120)
        r.reevaluate_held()
        assert 1 in eng.cancelled and "0xa" not in r.placed   # exited fast_book
        assert r.cooldown.get("0xa") == r.cfg.cooldown_rounds  # benched

    def test_calm_book_not_exited(self, monkeypatch):
        import collections
        import time as _t
        eng = self._held_engine()
        r = _runner(engine=eng, min_hold_s=0.0, max_mid_vel_cps=2.0)
        r.placed = {"0xa": {"token": "tok_a"}}
        r.placed_at = {"0xa": 0.0}
        monkeypatch.setattr(r, "_rescore", lambda c, t, **kw: self._fresh())
        now = _t.monotonic()
        r._mid_hist["tok_a"] = collections.deque(             # flat -> ~0 c/s
            [(now, 0.50), (now + 1.0, 0.50)], maxlen=120)
        r.reevaluate_held()
        assert eng.cancelled == [] and "0xa" in r.placed      # calm -> kept

    def test_disabled_noop(self, monkeypatch):
        eng = self._held_engine()
        r = _runner(engine=eng, reeval_enabled=False)
        r.placed = {"0xa": {"token": "tok_a"}}
        r.placed_at = {"0xa": 0.0}
        called = {"n": 0}
        monkeypatch.setattr(r, "_rescore", lambda c, t, **kw: called.__setitem__("n", 1))
        r.reevaluate_held()
        assert called["n"] == 0 and eng.cancelled == []


class TestReflex:
    def test_cancels_on_big_move(self):
        sub = FakeSubmitter()
        r = _runner(submitter=sub)
        r._reflex_refs = {"tok": (0.50, 0.02)}
        r._on_ws_price("tok", 0.55)            # 0.05 move >= 0.02 band
        assert any(c["action"] == "CANCEL_ALL" and c["token_id"] == "tok"
                   for c in sub.calls)
        assert "tok" in r._reflex_cancelled

    def test_ignores_small_move(self):
        sub = FakeSubmitter()
        r = _runner(submitter=sub)
        r._reflex_refs = {"tok": (0.50, 0.02)}
        r._on_ws_price("tok", 0.505)           # < band
        assert sub.calls == [] and "tok" not in r._reflex_cancelled

    def test_dedups_until_repost(self):
        sub = FakeSubmitter()
        r = _runner(submitter=sub)
        r._reflex_refs = {"tok": (0.50, 0.02)}
        r._on_ws_price("tok", 0.55)
        r._on_ws_price("tok", 0.57)            # already pulled -> no second cancel
        assert sum(1 for c in sub.calls if c["action"] == "CANCEL_ALL") == 1

    def test_unknown_token_noop(self):
        sub = FakeSubmitter()
        r = _runner(submitter=sub)
        r._on_ws_price("tok", 0.99)            # no ref -> nothing
        assert sub.calls == []

    def test_poll_passes_reflex_cancelled_as_force_recenter(self):
        eng = FakeEngine()
        eng.quotes = [{"id": 1, "market_condition_id": "0xa", "token_id": "tok",
                       "inventory": 0.0, "tick": 0.01, "last_mid": 0.5}]
        r = _runner(engine=eng, submitter=FakeSubmitter())
        r._reflex_cancelled = {"tok"}
        r.poll_once()
        assert eng.last_force_recenter == {"tok"}   # forwarded to the engine
        assert r._reflex_cancelled == set()         # consumed

    def test_dead_man_refresh_replaces_aged_orders(self):
        eng = FakeEngine()
        eng.quotes = [{"id": 1, "market_condition_id": "0xa", "token_id": "tok",
                       "inventory": 0.0, "tick": 0.01, "last_mid": 0.5}]
        r = _runner(engine=eng, submitter=FakeSubmitter(), order_expiry_s=120.0)
        r.placed = {"0xa": {"token": "tok"}}
        r._refresh_at = {"tok": time.monotonic() - 999}   # older than expiry/2 = 60s -> due
        r.poll_once()
        assert "tok" in (eng.last_force_recenter or set())  # re-placed before expiry

    def test_dead_man_refresh_skips_fresh_orders(self):
        eng = FakeEngine()
        eng.quotes = [{"id": 1, "market_condition_id": "0xa", "token_id": "tok",
                       "inventory": 0.0, "tick": 0.01, "last_mid": 0.5}]
        r = _runner(engine=eng, submitter=FakeSubmitter(), order_expiry_s=120.0)
        r.placed = {"0xa": {"token": "tok"}}
        r.poll_once()                                # first sight -> start clock, don't force
        assert "tok" not in (eng.last_force_recenter or set())


class TestPollEventThrottle:
    def _rows(self):
        return [{"quote": {"market_condition_id": "0xa", "token_id": "tok"},
                 "mid": 0.5, "reward": 0.01}]

    def test_poll_heartbeat_throttled(self, tmp_path):
        import json

        from pm_trader.events import EventLog
        eng = FakeEngine(accrue_rows=self._rows())
        r = _runner(engine=eng, submitter=FakeSubmitter(), event_poll_every_s=1e9)
        r._events = EventLog(tmp_path / "events", retention_days=30)
        r.poll_once()
        r.poll_once()                       # within the window -> throttled
        r._events.close()
        lines = [json.loads(l) for f in (tmp_path / "events").glob("events-*.jsonl")
                 for l in f.read_text().splitlines()]
        assert sum(1 for e in lines if e["kind"] == "poll") == 1

    def test_notable_rows_always_logged(self, tmp_path):
        import json

        from pm_trader.events import EventLog
        rows = [{"quote": {"market_condition_id": "0xa", "token_id": "tok"},
                 "mid": 0.5, "fills_applied": 1}]            # a fill -> always logged
        eng = FakeEngine(accrue_rows=rows)
        r = _runner(engine=eng, submitter=FakeSubmitter(), event_poll_every_s=1e9)
        r._events = EventLog(tmp_path / "events", retention_days=30)
        r.poll_once()
        r.poll_once()
        r._events.close()
        lines = [json.loads(l) for f in (tmp_path / "events").glob("events-*.jsonl")
                 for l in f.read_text().splitlines()]
        assert sum(1 for e in lines if e["kind"] == "poll") == 2   # both logged


class TestProfitEstimate:
    def test_logs_live_profit_under_competition(self, tmp_path, caplog):
        import json
        import logging

        from pm_trader.events import EventLog
        from pm_trader.models import OrderBook, OrderBookLevel

        class _MC:
            def get_book(self, t):
                return OrderBook(
                    bids=[OrderBookLevel(0.49, 100), OrderBookLevel(0.48, 200)],
                    asks=[OrderBookLevel(0.51, 100), OrderBookLevel(0.52, 200)])
            def get_midpoint(self, t):
                return 0.50
            def updates(self, t):
                return 0

        eng = FakeEngine(summary={"reward_income": 1.2, "adverse_bleed": 0.3,
                                  "net_maker_pnl": 0.9})
        eng.quotes = [{"token_id": "tok", "size": 50.0, "half_spread_c": 1.0,
                       "max_spread_c": 4.5, "daily_rate": 400.0,
                       "committed_capital": 49.0}]
        r = _runner(engine=eng, submitter=FakeSubmitter())
        r._market_ch = _MC()
        r._events = EventLog(tmp_path / "events", retention_days=30)
        with caplog.at_level(logging.INFO, logger="pm_trader.runner"):
            r._log_pool_metrics()
        r._events.close()
        assert any("PROFIT (live competition)" in m for m in caplog.messages)
        evs = [json.loads(l) for f in (tmp_path / "events").glob("events-*.jsonl")
               for l in f.read_text().splitlines()]
        prof = [e for e in evs if e["kind"] == "profit"]
        assert prof and prof[0]["gross_day"] > 0 and prof[0]["pools"] == 1


class TestStats:
    def test_log_stats_emits_rate(self, caplog):
        import logging
        r = _runner(submitter=FakeSubmitter())
        r.rate_limiter.granted, r.rate_limiter.granted_low = 100, 70
        r._log_stats()                                  # baseline, no emit
        r.rate_limiter.granted, r.rate_limiter.granted_low = 220, 150
        with caplog.at_level(logging.INFO, logger="pm_trader.runner"):
            r._log_stats()
        assert any("stats:" in m and "req/s" in m for m in caplog.messages)


class TestBookResync:
    def test_resync_once_pulls_rest_and_applies_to_cache(self):
        applied = []

        class _MC:
            def apply_rest_snapshot(self, tok, book):
                applied.append((tok, book))

        class _Scanner:
            def book(self, tok):
                return {"bids": [{"price": "0.49", "size": "100"}],
                        "asks": [{"price": "0.51", "size": "100"}]}

        r = _runner(submitter=FakeSubmitter())
        r.scanner = _Scanner()
        r._market_ch = _MC()
        r._resync_once("tokX")
        assert applied and applied[0][0] == "tokX" and applied[0][1]["bids"]

    def test_resync_once_swallows_errors(self):
        class _Scanner:
            def book(self, tok):
                raise RuntimeError("rest down")

        r = _runner(submitter=FakeSubmitter())
        r.scanner = _Scanner()
        r._market_ch = object()           # never reached
        r._resync_once("tokX")            # must not raise


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


class TestRestartSafety:
    def test_rehydrate_adopts_surviving_quotes(self):
        eng = FakeEngine()
        eng.quotes = [{"id": 1, "market_condition_id": "0xa", "token_id": "tok_a",
                       "inventory": 0.0, "committed_capital": 49.0, "daily_rate": 400.0,
                       "half_spread_c": 1.0, "market_slug": "m"}]
        r = _runner(engine=eng)
        r._rehydrate()
        assert "0xa" in r.placed and "0xa" in r.placed_at

    def test_ensure_engine_does_not_wipe_existing_ledger(self, tmp_path):
        from unittest.mock import MagicMock
        from pm_trader.engine import Engine
        from pm_trader.models import Market
        eng = Engine(tmp_path)
        eng.init_account(200.0)
        eng.api.get_market = MagicMock(return_value=Market(
            condition_id="0xabc", slug="m", question="Q", description="",
            outcomes=["Yes", "No"], outcome_prices=[0.5, 0.5],
            tokens=[{"token_id": "tok_yes", "outcome": "Yes"},
                    {"token_id": "tok_no", "outcome": "No"}],
            active=True, closed=False, tick_size=0.01))
        eng.api.get_reward_config = MagicMock(return_value={
            "daily": 400.0, "max_spread": 4.5, "min_size": 50.0, "tick": 0.01,
            "token": "tok_yes", "question": "Q", "condition_id": "0xabc"})
        eng.api.get_midpoint = MagicMock(return_value=0.50)
        eng.place_maker_quote("0xabc", half_spread_cents=1.0)
        assert len(eng.get_maker_quotes()) == 1
        eng.close()
        # RESTART: fresh runner, engine=None, same state_dir
        r = LiveRunner(RunnerConfig(state_dir=str(tmp_path), capital=200.0),
                       scanner_client=FakeScanner(), sleeper=lambda _s: None)
        r._ensure_engine()
        assert len(r.engine.get_maker_quotes()) == 1   # NOT wiped
        r._rehydrate()
        assert "0xabc" in r.placed                      # adopted
        r.engine.close()


def test_reconcile_broker_cancels_orphans():
    class OOSubmitter(FakeSubmitter):
        def __init__(self, orders):
            super().__init__()
            self.orders = orders
            self.cancelled = []

        def list_open_orders(self):
            return self.orders

        def cancel_order(self, oid):
            self.cancelled.append(oid)
            return {"status": "CANCELLED"}

    sub = OOSubmitter([{"id": "o1", "asset_id": "tok_held"},
                       {"id": "o2", "asset_id": "tok_orphan"}])
    r = _runner(submitter=sub, live=True)
    r.placed = {"0xa": {"token": "tok_held"}}
    r._reconcile_broker_orders()
    assert sub.cancelled == ["o2"]   # only the orphaned order is cancelled


def test_shutdown_runs_on_exception():
    eng = FakeEngine()
    r = LiveRunner(RunnerConfig(discovery_interval_s=1e9, dry_live=False),
                   engine=eng, scanner_client=FakeScanner(markets=[]), sleeper=None)

    def _boom(_s):
        raise RuntimeError("crash mid-loop")
    r._sleep_fn = _boom
    try:
        r.run()
    except RuntimeError:
        pass
    assert eng.closed is True   # _shutdown ran via try/finally despite the crash


def test_one_sided_book_is_a_degradation():
    r = _runner()
    assert r._degrade_reason({"one_sided": True}) == "one_sided"
    assert "one_sided" in LiveRunner._COOLDOWN_REASONS


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
