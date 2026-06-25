"""Tests for the post-hoc review & reward-reconciliation tool."""

from __future__ import annotations

import json
from datetime import datetime, timezone
from types import SimpleNamespace

from pm_trader import review


def _q(cond, slug, *, reward=0.0, bleed=0.0, inv_pnl=0.0, fills=0, status="active"):
    return SimpleNamespace(market_condition_id=cond, market_slug=slug,
                           accrued_rewards=reward, realized_bleed=bleed,
                           inventory_pnl=inv_pnl, fills=fills, status=status)


class _Resp:
    def __init__(self, data):
        self._data = data
    def raise_for_status(self):
        pass
    def json(self):
        return self._data


class _FakeHTTP:
    """Returns successive pages, then []. Records calls."""
    def __init__(self, pages):
        self.pages = list(pages)
        self.calls = []
    def get(self, url, params=None):
        self.calls.append((url, params))
        return _Resp(self.pages.pop(0) if self.pages else [])


class TestFetchActualRewards:
    def test_parses_and_sums(self):
        page = [
            {"transactionHash": "0x1", "conditionId": "0xa", "usdcSize": 2.5,
             "timestamp": 100},
            {"transactionHash": "0x2", "conditionId": "0xa", "usdcSize": 1.5,
             "timestamp": 200},
        ]
        http = _FakeHTTP([page, []])
        out = review.fetch_actual_rewards("0xwallet", http=http)
        assert [o["usdc"] for o in out] == [2.5, 1.5]
        assert out[0]["condition_id"] == "0xa" and out[0]["tx"] == "0x1"
        # type=REWARD filter is sent
        assert http.calls[0][1]["type"] == "REWARD"
        assert http.calls[0][1]["user"] == "0xwallet"

    def test_dedups_on_repeated_offset(self):
        dup = [{"transactionHash": "0x1", "conditionId": "0xa", "usdcSize": 2.0,
                "timestamp": 1}]
        # same row returned twice (offset didn't advance) -> must stop, not loop
        http = _FakeHTTP([dup, dup, dup])
        out = review.fetch_actual_rewards("0xw", http=http, page=1)
        assert len(out) == 1

    def test_empty_first_page(self):
        assert review.fetch_actual_rewards("0xw", http=_FakeHTTP([[]])) == []


class TestLoadEvents:
    def test_loads_and_windows(self, tmp_path):
        d = tmp_path / "events"
        d.mkdir()
        (d / "events-20000101.jsonl").write_text('{"kind":"poll","old":1}\n')
        today = datetime.now(timezone.utc).strftime("%Y%m%d")
        (d / f"events-{today}.jsonl").write_text(
            '{"kind":"place","cond":"0xa"}\n{"kind":"exit","reason":"jump_risk_rose"}\n')
        # window excludes the year-2000 file
        ev = review.load_events(d, days=10)
        assert all(e.get("old") != 1 for e in ev)
        assert {e["kind"] for e in ev} == {"place", "exit"}
        # no window -> includes the old file too
        assert len(review.load_events(d)) == 3

    def test_missing_dir(self, tmp_path):
        assert review.load_events(tmp_path / "nope") == []


class TestSummarize:
    def test_joins_estimate_actual_and_decisions(self):
        quotes = [
            _q("0xa", "alpha", reward=2.0, bleed=0.3, fills=1),
            _q("0xa", "alpha", reward=1.0, bleed=0.0, fills=0, status="cancelled"),
            _q("0xb", "beta", reward=4.0, bleed=0.5, inv_pnl=-0.2, fills=2),
        ]
        events = [
            {"kind": "place", "cond": "0xa"}, {"kind": "place", "cond": "0xb"},
            {"kind": "exit", "reason": "share_collapsed"},
            {"kind": "discovery", "safe": 8}, {"kind": "discovery", "safe": 6},
            {"kind": "poll", "reward": 0.01},
        ]
        actual = [
            {"condition_id": "0xa", "usdc": 1.5}, {"condition_id": "0xa", "usdc": 0.5},
            {"condition_id": "0xb", "usdc": 5.0},
        ]
        rep = review.summarize(quotes, events, actual)
        assert rep["estimated_reward_total"] == 7.0          # 2+1+4
        assert rep["actual_reward_total"] == 7.0             # 2+5
        assert rep["reconciliation_ratio"] == 1.0
        assert rep["bleed_total"] == 0.8
        assert rep["decisions"] == {"places": 2,
                                    "exits_by_reason": {"share_collapsed": 1}}
        assert rep["discovery"] == {"scans": 2, "avg_safe_pools": 7.0}
        pool_a = next(p for p in rep["per_pool"] if p["condition_id"] == "0xa")
        assert pool_a["est_reward"] == 3.0 and pool_a["actual_reward"] == 2.0
        assert pool_a["n_quotes"] == 2

    def test_actual_only_market_appears(self):
        # a market that paid actual rewards but has no ledger quote still shows up
        rep = review.summarize([], [], [{"condition_id": "0xz", "usdc": 3.0}])
        assert rep["actual_reward_total"] == 3.0
        assert rep["reconciliation_ratio"] is None          # no estimate
        assert rep["per_pool"][0]["condition_id"] == "0xz"

    def test_format_report_runs_and_flags_overestimate(self):
        quotes = [_q("0xa", "alpha", reward=10.0)]
        actual = [{"condition_id": "0xa", "usdc": 2.0}]      # ratio 0.2 -> warn
        text = review.format_report(review.summarize(quotes, [], actual))
        assert "MAKER REVIEW" in text and "actual/est = 0.20x" in text
        assert "over-estimated" in text
