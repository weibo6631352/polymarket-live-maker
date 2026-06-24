"""Tests for periodic discovery (async refresh/watch)."""

from __future__ import annotations

import json

from live_maker import discovery
from live_maker.scanner import score_pool
from tests.conftest import flat_hist, market, raw_book


class FakeClient:
    """Async stand-in for AsyncRewardsClient."""

    def __init__(self, markets, books, histories):
        self.markets, self.books, self.histories = markets, books, histories
        self.scans = 0

    async def sampling_markets(self, *, max_pages=100):
        self.scans += 1
        return self.markets

    async def book(self, token):
        return self.books.get(token, {})

    async def prices_history(self, token, *, interval="max", fidelity=1440):
        return self.histories.get(token, [])


def _client():
    markets = [market("a"), market("b")]
    return FakeClient(markets, {"a": raw_book(), "b": raw_book()},
                      {"a": flat_hist(), "b": flat_hist()})


async def test_refresh_returns_safe_summary():
    out = await discovery.refresh(_client(), min_daily=80.0)
    assert "safe_count" in out and out["pools_scored"] >= 1


async def test_refresh_writes_file(tmp_path):
    path = tmp_path / "latest.json"
    await discovery.refresh(_client(), out_path=str(path), min_daily=80.0)
    saved = json.loads(path.read_text())
    assert "safe" in saved and "safe_count" in saved


async def test_watch_loops_rounds():
    c = _client()
    results = await discovery.watch(c, interval_s=0.0, rounds=3, min_daily=80.0)
    assert len(results) == 3 and c.scans == 3


async def test_watch_zero_rounds():
    c = _client()
    assert await discovery.watch(c, interval_s=0.0, rounds=0) == []


def test_score_pool_used_by_refresh_is_sane():
    # sanity: the migrated scoring still classifies a flat pool SAFE
    row = score_pool(
        {"daily": 400, "max_spread": 4.5, "min_size": 50, "tick": 0.01,
         "token": "a", "question": "q", "condition_id": "0xa"},
        raw_book(), flat_hist())
    assert row["jump_verdict"] == "SAFE"
