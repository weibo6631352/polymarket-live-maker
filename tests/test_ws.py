"""Tests for the WS channel message handlers + state (no network)."""

from __future__ import annotations

import json

from pm_trader.ws import MarketChannel, UserChannel, _proxy_kwargs


class TestProxyKwargs:
    def test_none_and_empty(self):
        assert _proxy_kwargs(None) == {}
        assert _proxy_kwargs("") == {}

    def test_socks5(self):
        kw = _proxy_kwargs("socks5://127.0.0.1:1080")
        assert kw == {"http_proxy_host": "127.0.0.1", "http_proxy_port": 1080,
                      "proxy_type": "socks5"}

    def test_http_with_auth(self):
        kw = _proxy_kwargs("http://user:pass@proxy.local:8080")
        assert kw["http_proxy_host"] == "proxy.local" and kw["http_proxy_port"] == 8080
        assert kw["proxy_type"] == "http" and kw["http_proxy_auth"] == ("user", "pass")

    def test_missing_port_ignored(self):
        assert _proxy_kwargs("socks5://hostonly") == {}


def _book_msg(token, bids, asks):
    return json.dumps({"event_type": "book", "asset_id": token,
                       "bids": [{"price": str(p), "size": str(s)} for p, s in bids],
                       "asks": [{"price": str(p), "size": str(s)} for p, s in asks]})


class TestMarketChannel:
    def test_book_snapshot_builds_book_and_mid(self):
        mc = MarketChannel()
        mc.handle_message(_book_msg("t1", [(0.49, 100), (0.48, 200)],
                                          [(0.51, 150), (0.52, 50)]))
        book = mc.get_book("t1")
        assert book is not None
        assert max(l.price for l in book.bids) == 0.49
        assert min(l.price for l in book.asks) == 0.51
        assert mc.get_midpoint("t1") == 0.50

    def test_price_change_updates_levels_and_mid(self):
        mc = MarketChannel()
        mc.handle_message(_book_msg("t1", [(0.49, 100)], [(0.51, 100)]))
        # new best bid 0.50 arrives
        mc.handle_message(json.dumps({"event_type": "price_change", "price_changes": [
            {"asset_id": "t1", "side": "BUY", "price": "0.50", "size": "80",
             "best_bid": "0.50", "best_ask": "0.51"}]}))
        assert mc.get_midpoint("t1") == 0.505
        book = mc.get_book("t1")
        assert max(l.price for l in book.bids) == 0.50

    def test_price_change_size_zero_removes_level(self):
        mc = MarketChannel()
        mc.handle_message(_book_msg("t1", [(0.49, 100), (0.50, 80)], [(0.51, 100)]))
        assert mc.get_midpoint("t1") == 0.505               # best bid 0.50
        mc.handle_message(json.dumps({"event_type": "price_change", "price_changes": [
            {"asset_id": "t1", "side": "BUY", "price": "0.50", "size": "0"}]}))
        assert mc.get_midpoint("t1") == 0.50                 # 0.50 removed -> bid 0.49

    def test_one_sided_or_unknown_returns_none_zero(self):
        mc = MarketChannel()
        assert mc.get_book("nope") is None
        assert mc.get_midpoint("nope") == 0.0
        mc.handle_message(_book_msg("t1", [(0.49, 100)], []))   # one-sided
        assert mc.get_book("t1") is None

    def test_non_json_and_unknown_events_ignored(self):
        mc = MarketChannel()
        mc.handle_message("PONG")                            # heartbeat, not JSON
        mc.handle_message(json.dumps({"event_type": "tick_size_change"}))
        assert mc.get_book("t1") is None

    def test_array_frame_of_events(self):
        mc = MarketChannel()
        mc.handle_message(json.dumps([
            json.loads(_book_msg("a", [(0.4, 10)], [(0.6, 10)])),
            json.loads(_book_msg("b", [(0.2, 10)], [(0.3, 10)]))]))
        assert mc.get_midpoint("a") == 0.5 and mc.get_midpoint("b") == 0.25

    def test_set_tokens_drops_stale_cache(self):
        mc = MarketChannel()
        mc.handle_message(_book_msg("t1", [(0.49, 10)], [(0.51, 10)]))
        mc.set_tokens(["t2"])                                # t1 no longer wanted
        assert mc.get_book("t1") is None

    def test_subscribe_msg_shape(self):
        m = json.loads(MarketChannel.subscribe_msg(["x", "y"]))
        assert m == {"assets_ids": ["x", "y"], "type": "market",
                     "custom_feature_enabled": True}

    def test_price_callback_fires_for_watched_token(self):
        mc = MarketChannel()
        mc.set_tokens(["t1"])
        seen = []
        mc.set_price_callback(lambda t, m: seen.append((t, m)))
        mc.handle_message(_book_msg("t1", [(0.49, 10)], [(0.51, 10)]))
        assert seen and seen[-1] == ("t1", 0.50)

    def test_price_callback_skips_unwatched_token(self):
        mc = MarketChannel()
        mc.set_tokens(["t1"])
        seen = []
        mc.set_price_callback(lambda t, m: seen.append(t))
        mc.handle_message(_book_msg("t2", [(0.4, 10)], [(0.6, 10)]))   # not subscribed
        assert seen == []


class TestUserChannel:
    def _creds(self):
        return {"apiKey": "k", "secret": "s", "passphrase": "p"}

    def test_trade_event_enqueues_fill(self):
        uc = UserChannel(self._creds())
        uc.handle_message(json.dumps({"event_type": "trade", "id": "x1",
            "asset_id": "tok", "side": "BUY", "size": "5", "price": "0.5"}))
        fills = uc.poll_fills()
        assert fills == [{"id": "x1", "token_id": "tok", "side": "BUY",
                          "size": 5.0, "price": 0.5}]
        assert uc.poll_fills() == []                         # drained

    def test_dedup_repeated_status_updates(self):
        uc = UserChannel(self._creds())
        for status in ("MATCHED", "MINED", "CONFIRMED"):     # same id, 3 updates
            uc.handle_message(json.dumps({"event_type": "trade", "id": "x1",
                "asset_id": "tok", "side": "SELL", "size": "5", "price": "0.5",
                "status": status}))
        assert len(uc.poll_fills()) == 1                     # counted once

    def test_invert_side(self):
        uc = UserChannel(self._creds(), invert_side=True)
        uc.handle_message(json.dumps({"event_type": "trade", "id": "x1",
            "asset_id": "tok", "side": "BUY", "size": "1", "price": "0.5"}))
        assert uc.poll_fills()[0]["side"] == "SELL"

    def test_non_trade_events_ignored(self):
        uc = UserChannel(self._creds())
        uc.handle_message(json.dumps({"event_type": "order", "id": "o1"}))
        uc.handle_message("PONG")
        assert uc.poll_fills() == []

    def test_subscribe_msg_auth_shape(self):
        m = json.loads(UserChannel.subscribe_msg(self._creds(), ["0xabc"]))
        assert m["type"] == "user" and m["markets"] == ["0xabc"]
        assert m["auth"] == {"apiKey": "k", "secret": "s", "passphrase": "p"}
