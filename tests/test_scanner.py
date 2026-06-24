"""Tests for the pool scanner — pure scoring + the async CLOB reader."""

from __future__ import annotations

import httpx
import pytest

from live_maker import scanner
from live_maker.models import ApiError
from live_maker.scanner import (
    AsyncRewardsClient,
    _best,
    classify_jump_risk,
    inband_score,
    parse_rewards,
    reward_share,
    scan,
    score_pool,
)
from tests.conftest import flat_hist, market, raw_book


# ---- pure scoring ---------------------------------------------------------

class TestParseRewards:
    def test_basic(self):
        p = parse_rewards(market("a"))
        assert p["daily"] == 400.0 and p["token"] == "a" and p["max_spread"] == 4.5

    def test_no_rewards_none(self):
        assert parse_rewards({"rewards": {"rates": []}, "tokens": [{"token_id": "a"}]}) is None

    def test_zero_daily_none(self):
        m = market("a", daily=0.0)
        assert parse_rewards(m) is None

    def test_no_token_none(self):
        m = market("a")
        m["tokens"] = []
        assert parse_rewards(m) is None

    def test_bad_rate_skipped(self):
        m = market("a")
        m["rewards"]["rates"] = [{"rewards_daily_rate": "oops"}, {"rewards_daily_rate": 100}]
        assert parse_rewards(m)["daily"] == 100.0

    def test_bad_spread_config_none(self):
        m = market("a")
        m["rewards"]["max_spread"] = "not-a-number"
        assert parse_rewards(m) is None


class TestInbandScore:
    def test_in_band_counts(self):
        score, notional = inband_score([{"price": 0.49, "size": 100}], 0.50, 4.5, True)
        assert score > 0 and notional == pytest.approx(49.0)

    def test_out_of_band_excluded(self):
        score, _ = inband_score([{"price": 0.40, "size": 100}], 0.50, 4.5, True)
        assert score == 0.0

    def test_bad_level_skipped(self):
        score, _ = inband_score([{"price": "x", "size": 100}], 0.50, 4.5, True)
        assert score == 0.0


class TestRewardShareAndJump:
    def test_reward_share_empty(self):
        assert reward_share(50, 0.01, 4.5, 0.0) == 1.0

    def test_classify_no_history(self):
        assert classify_jump_risk([0.5] * 3, 10, 50)["verdict"] == "no-history"

    def test_classify_safe_flat(self):
        assert classify_jump_risk([0.5] * 20, 10, 50)["verdict"] == "SAFE"

    def test_classify_kill_on_big_jump(self):
        # max jump 0.4 -> loss 50*0.4=20; at $0.5/day reward that wipes 40 days > 20 -> KILL
        prices = [0.5] * 10 + [0.9] + [0.9] * 10
        out = classify_jump_risk(prices, 0.5, 50)
        assert out["verdict"] == "KILL"

    def test_classify_watch_band(self):
        # loss 50*0.2=10; at $1/day that wipes 10 days: >7 (WATCH) but <=20 (not KILL)
        prices = [0.5] * 10 + [0.7] + [0.7] * 10
        out = classify_jump_risk(prices, 1.0, 50)
        assert out["verdict"] == "WATCH"

    def test_best(self):
        assert _best([{"price": 0.49}, {"price": 0.48}], is_bid=True) == 0.49
        assert _best([{"price": 0.51}, {"price": 0.52}], is_bid=False) == 0.51
        assert _best([], is_bid=True) is None


class TestScorePool:
    def test_one_sided_returns_none(self):
        pool = parse_rewards(market("a"))
        assert score_pool(pool, {"bids": [], "asks": [{"price": 0.51, "size": 1}]}, []) is None

    def test_scores_two_sided(self):
        pool = parse_rewards(market("a"))
        row = score_pool(pool, raw_book(), flat_hist())
        assert row["jump_verdict"] == "SAFE"
        assert 0 < row["share"] <= 1
        assert row["token"] == "a"


# ---- async HTTP reader ----------------------------------------------------

def _transport(markets, books, histories, reward_cfgs=None):
    def handler(request: httpx.Request) -> httpx.Response:
        path = request.url.path
        if path == "/sampling-markets":
            return httpx.Response(200, json={"data": markets, "next_cursor": "LTE="})
        if path == "/book":
            tok = request.url.params.get("token_id")
            return httpx.Response(200, json=books.get(tok, {}))
        if path == "/prices-history":
            tok = request.url.params.get("market")
            return httpx.Response(200, json={"history": histories.get(tok, [])})
        if path.startswith("/markets/"):
            cond = path.split("/markets/")[1]
            return httpx.Response(200, json=(reward_cfgs or {}).get(cond, {}))
        return httpx.Response(404, json={})
    return httpx.MockTransport(handler)


async def _client(markets, books, histories, reward_cfgs=None):
    http = httpx.AsyncClient(transport=_transport(markets, books, histories, reward_cfgs))
    return AsyncRewardsClient(http=http)


async def test_sampling_markets_paginates_terminates():
    c = await _client([market("a"), market("b")], {}, {})
    out = await c.sampling_markets()
    assert len(out) == 2
    await c.close()


async def test_book_and_history():
    c = await _client([], {"a": raw_book()}, {"a": flat_hist()})
    assert (await c.book("a"))["bids"]
    assert len(await c.prices_history("a")) == 15
    await c.close()


async def test_reward_config_present_and_absent():
    c = await _client([], {}, {}, reward_cfgs={"0xa": market("a")})
    cfg = await c.reward_config("0xa")
    assert cfg["daily"] == 400.0
    assert await c.reward_config("0xmissing") is None
    await c.close()


async def test_scan_ranks_safe_first():
    markets = [market("a"), market("b", daily=100.0)]
    books = {"a": raw_book(), "b": raw_book()}
    hists = {"a": flat_hist(), "b": flat_hist()}
    c = await _client(markets, books, hists)
    report = await scan(c, min_daily=80.0, top=10)
    assert report["pools_scored"] == 2
    assert report["safe_count"] >= 1
    await c.close()


async def test_http_error_raises_apierror():
    def handler(request):
        return httpx.Response(500, text="boom")
    c = AsyncRewardsClient(http=httpx.AsyncClient(transport=httpx.MockTransport(handler)))
    with pytest.raises(ApiError):
        await c.book("a")
    await c.close()


async def test_context_manager_closes():
    transport = httpx.MockTransport(lambda _: httpx.Response(200, json={}))
    async with AsyncRewardsClient(http=httpx.AsyncClient(transport=transport)) as c:
        assert c is not None


async def test_scan_skips_book_error(monkeypatch):
    # a pool whose book 500s is dropped, not fatal
    markets = [market("a"), market("b")]
    books = {"b": raw_book()}  # 'a' missing -> {} -> one-sided -> None

    def handler(request):
        path = request.url.path
        if path == "/sampling-markets":
            return httpx.Response(200, json={"data": markets, "next_cursor": "LTE="})
        if path == "/book":
            tok = request.url.params.get("token_id")
            if tok == "a":
                return httpx.Response(500, text="boom")
            return httpx.Response(200, json=books.get(tok, {}))
        if path == "/prices-history":
            return httpx.Response(200, json={"history": flat_hist()})
        return httpx.Response(404, json={})

    c = AsyncRewardsClient(http=httpx.AsyncClient(transport=httpx.MockTransport(handler)))
    report = await scan(c, min_daily=80.0, top=10)
    assert report["pools_scored"] == 1
    await c.close()
