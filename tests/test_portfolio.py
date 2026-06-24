"""Tests for portfolio selection (risk-adjusted, decorrelated, cooldown, budget)."""

from __future__ import annotations

from live_maker.portfolio import (
    _cluster_key,
    _risk_adjusted_score,
    _significant_tokens,
    select_pools,
)


def _pool(token, question, *, daily=400.0, share=0.2, min_size=50.0, tick=0.01,
          verdict="SAFE", days_wiped=2.0, empty=False, reward=None):
    return {
        "question": question, "condition_id": "0x" + token, "token": token,
        "daily": daily, "share": share, "min_size": min_size, "tick": tick,
        "max_spread_c": 4.5, "jump_verdict": verdict, "empty_band": empty,
        "days_wiped": days_wiped, "reward_per_day": reward if reward is not None else share * daily,
    }


def _report(pools):
    return {"pools": pools}


class TestHelpers:
    def test_significant_tokens_drops_stopwords(self):
        toks = _significant_tokens("Will the Fed cut rates in December?")
        assert "fed" in toks and "the" not in toks and "will" not in toks

    def test_cluster_key_fed(self):
        assert _cluster_key("Will the FOMC cut rates?") == "us-fed"

    def test_cluster_key_none(self):
        assert _cluster_key("Will it rain tomorrow?") is None

    def test_risk_adjusted_prefers_safe(self):
        safe = _pool("a", "q", days_wiped=1.0)
        jumpy = _pool("b", "q", days_wiped=50.0)
        assert _risk_adjusted_score(safe, 7.0) > _risk_adjusted_score(jumpy, 7.0)

    def test_risk_adjusted_missing_fields(self):
        p = {"share": 0.2, "daily": 400}
        assert _risk_adjusted_score(p, 7.0) > 0


class TestSelectPools:
    def test_picks_within_capital(self):
        # one distinct significant token per pool -> no correlation dedup
        pools = [_pool(f"t{i}", f"alpha{i}") for i in range(10)]
        sel = select_pools(_report(pools), capital=100.0, max_pools=40)
        # each committed ~49 -> only ~2 fit in $100
        assert sum(s["committed_capital"] for s in sel) <= 100.0
        assert len(sel) == 2

    def test_max_pools_cap(self):
        pools = [_pool(f"t{i}", f"alpha{i}") for i in range(10)]
        sel = select_pools(_report(pools), capital=10_000.0, max_pools=3)
        assert len(sel) == 3

    def test_cooldown_excludes(self):
        pools = [_pool("a", "Event one"), _pool("b", "Event two")]
        sel = select_pools(_report(pools), capital=10_000.0, cooldown={"a", "0xa"})
        assert all(s["token"] != "a" for s in sel)

    def test_requires_safe(self):
        pools = [_pool("a", "Event one", verdict="KILL"), _pool("b", "Event two")]
        sel = select_pools(_report(pools), capital=10_000.0)
        assert [s["token"] for s in sel] == ["b"]

    def test_cluster_dedup(self):
        pools = [
            _pool("a", "Will the Fed cut in July?"),
            _pool("b", "Will the FOMC hold in September?"),  # same us-fed cluster
        ]
        sel = select_pools(_report(pools), capital=10_000.0)
        assert len(sel) == 1

    def test_token_overlap_dedup(self):
        pools = [
            _pool("a", "Will Zohran Mamdani win the primary?"),
            _pool("b", "Will Zohran Mamdani win the general?"),  # shares mamdani
        ]
        sel = select_pools(_report(pools), capital=10_000.0, max_token_overlap=1)
        assert len(sel) == 1

    def test_empty_band_excluded(self):
        pools = [_pool("a", "Event one", empty=True), _pool("b", "Event two")]
        sel = select_pools(_report(pools), capital=10_000.0)
        assert [s["token"] for s in sel] == ["b"]

    def test_degenerate_capital_skipped(self):
        # half_spread wider than 50c per side -> committed_capital 0 -> skipped
        pools = [_pool("a", "Event", tick=0.6)]  # 1 tick * 100 = 60c half-spread
        sel = select_pools(_report(pools), capital=10_000.0)
        assert sel == []
