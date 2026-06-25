"""Real-time CLOB WebSocket channels (threaded; websocket-client).

  Market channel -> live order book per token (``book`` snapshot + ``price_change``
    deltas) so the maker loop reacts to a mid move in ~ms instead of REST-polling,
    and the 149/s REST budget is freed for order writes + discovery.
  User channel   -> the account's real fills (``trade`` events) in real time,
    replacing REST ``get_trades`` polling.

Threaded to fit the sync engine: a reader thread per channel parses frames into
thread-safe state (a maintained book cache / a fills queue); the engine consumes
it via the same interfaces it already uses (``get_book`` / ``get_midpoint`` /
``poll_fills``). The PURE message handlers + subscribe builders are split from the
socket loop so they unit-test with no network; the connect/reconnect loop is
``pragma: no cover`` (needs a live WS). ``websocket-client`` is imported lazily
there, so this module imports fine without the dep (same pattern as ClobSubmitter).
"""

from __future__ import annotations

import json
import logging
import threading
import time
from collections import deque

from pm_trader.models import OrderBook, OrderBookLevel

MARKET_WS = "wss://ws-subscriptions-clob.polymarket.com/ws/market"
USER_WS = "wss://ws-subscriptions-clob.polymarket.com/ws/user"
_PING_INTERVAL_S = 10.0   # docs: send "PING" every 10s, server replies "PONG"

log = logging.getLogger("pm_trader.ws")


def _f(x, default=0.0):
    try:
        return float(x)
    except (TypeError, ValueError):
        return default


class MarketChannel:
    """Maintains a live order book per subscribed token from the market channel.

    Implements the engine's book-source interface (``get_book`` / ``get_midpoint``):
    returns the WS-maintained book, or ``None`` / ``0.0`` when a token has no fresh
    snapshot yet (just subscribed) so the caller falls back to REST.
    """

    def __init__(self, url: str = MARKET_WS) -> None:
        self.url = url
        self._lock = threading.Lock()
        # token -> {"bids": {price: size}, "asks": {price: size}}
        self._levels: dict[str, dict[str, dict[float, float]]] = {}
        self._ts: dict[str, float] = {}           # token -> last update (monotonic)
        self._tokens: set[str] = set()            # desired subscription set
        self._on_price = None                     # callback(token, mid) on updates
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._ws = None                           # live connection (set in the loop)

    def set_price_callback(self, fn) -> None:
        """Register ``fn(token_id, mid)``, fired (off-lock) on each mid update for a
        subscribed token. Used by the runner's reflex to cancel on a fast move."""
        self._on_price = fn

    # -- pure message handling (unit-tested) --------------------------------

    @staticmethod
    def subscribe_msg(tokens) -> str:
        return json.dumps({"assets_ids": list(tokens), "type": "market",
                           "custom_feature_enabled": True})

    def handle_message(self, raw) -> None:
        """Parse one WS frame (a JSON event or array of events) into the cache.
        Non-JSON frames (e.g. ``PONG``) are ignored."""
        try:
            data = json.loads(raw)
        except (ValueError, TypeError):
            return
        touched: set[str] = set()
        for ev in (data if isinstance(data, list) else [data]):
            if not isinstance(ev, dict):
                continue
            et = ev.get("event_type")
            if et == "book":
                t = self._on_book(ev)
                if t:
                    touched.add(t)
            elif et == "price_change":
                touched.update(self._on_price_change(ev))
        # fire the price callback OUTSIDE the lock (it may do I/O / take other locks)
        cb = self._on_price
        if cb is not None:
            for t in touched:
                if t in self._tokens:
                    mid = self.get_midpoint(t)
                    if mid > 0:
                        cb(t, mid)

    def _on_book(self, ev: dict) -> str | None:
        token = ev.get("asset_id")
        if not token:
            return None
        bids = {_f(l.get("price")): _f(l.get("size")) for l in ev.get("bids") or []}
        asks = {_f(l.get("price")): _f(l.get("size")) for l in ev.get("asks") or []}
        with self._lock:
            self._levels[token] = {"bids": bids, "asks": asks}   # full snapshot
            self._ts[token] = time.monotonic()
        return token

    def _on_price_change(self, ev: dict) -> set[str]:
        changes = ev.get("price_changes") or []
        touched: set[str] = set()
        with self._lock:
            for ch in changes:
                token = ch.get("asset_id")
                if not token:
                    continue
                touched.add(token)
                book = self._levels.setdefault(token, {"bids": {}, "asks": {}})
                side = "bids" if str(ch.get("side", "")).upper() == "BUY" else "asks"
                price, size = _f(ch.get("price")), _f(ch.get("size"))
                if size <= 0:
                    book[side].pop(price, None)        # size 0 -> level removed
                else:
                    book[side][price] = size
                self._ts[token] = time.monotonic()
        return touched

    # -- engine book-source interface ---------------------------------------

    def get_book(self, token_id: str) -> OrderBook | None:
        with self._lock:
            lv = self._levels.get(token_id)
            if not lv or not lv["bids"] or not lv["asks"]:
                return None                           # no fresh two-sided snapshot
            bids = [OrderBookLevel(p, s) for p, s in lv["bids"].items() if s > 0]
            asks = [OrderBookLevel(p, s) for p, s in lv["asks"].items() if s > 0]
        if not bids or not asks:
            return None
        return OrderBook(bids=bids, asks=asks)

    def get_midpoint(self, token_id: str) -> float:
        with self._lock:
            lv = self._levels.get(token_id)
            if not lv or not lv["bids"] or not lv["asks"]:
                return 0.0
            best_bid = max(lv["bids"])
            best_ask = min(lv["asks"])
        return (best_bid + best_ask) / 2.0

    def fresh(self, token_id: str, max_age_s: float = 5.0) -> bool:
        with self._lock:
            ts = self._ts.get(token_id)
        return ts is not None and (time.monotonic() - ts) <= max_age_s

    # -- subscription management --------------------------------------------

    def set_tokens(self, tokens) -> None:
        """Update the desired subscription set; the reader (re)subscribes on connect.
        Drops cached books for tokens no longer wanted."""
        new = set(t for t in tokens if t)
        with self._lock:
            self._tokens = new
            for t in list(self._levels):        # drop any cached book no longer wanted
                if t not in new:
                    self._levels.pop(t, None)
                    self._ts.pop(t, None)
        self._resubscribe()

    def _resubscribe(self) -> None:  # pragma: no cover - needs a live socket
        ws = self._ws
        if ws is None:
            return
        with self._lock:
            tokens = list(self._tokens)
        if tokens:
            try:
                ws.send(self.subscribe_msg(tokens))
            except Exception as e:  # noqa: BLE001
                log.warning("market resubscribe failed: %s", e)

    # -- lifecycle (live socket loop; pragma no cover) ----------------------

    def start(self) -> None:  # pragma: no cover - needs a live WS
        if self._thread is not None:
            return
        self._thread = threading.Thread(target=self._run, name="ws-market", daemon=True)
        self._thread.start()

    def stop(self) -> None:  # pragma: no cover - needs a live WS
        self._stop.set()
        ws = self._ws
        if ws is not None:
            try:
                ws.close()
            except Exception:  # noqa: BLE001
                pass
        if self._thread is not None:
            self._thread.join(timeout=5.0)

    def _run(self) -> None:  # pragma: no cover - needs a live WS
        import websocket  # lazy: only the live path needs the dep
        while not self._stop.is_set():
            try:
                self._ws = websocket.create_connection(self.url, timeout=15)
                self._ws.settimeout(1.0)
                self._resubscribe()
                last_ping = time.monotonic()
                while not self._stop.is_set():
                    now = time.monotonic()
                    if now - last_ping >= _PING_INTERVAL_S:
                        self._ws.send("PING")
                        last_ping = now
                    try:
                        msg = self._ws.recv()
                    except Exception:  # timeout or transient -> loop to ping/recv
                        continue
                    if msg:
                        self.handle_message(msg)
            except Exception as e:  # noqa: BLE001 - reconnect after a backoff
                if self._stop.is_set():
                    break
                log.warning("market WS reconnecting after error: %s", e)
                self._stop.wait(2.0)
            finally:
                try:
                    if self._ws is not None:
                        self._ws.close()
                except Exception:  # noqa: BLE001
                    pass
                self._ws = None


class UserChannel:
    """Streams the account's real fills (``trade`` events) into a fills queue.

    ``poll_fills`` drains it in the same shape as ``ClobSubmitter.poll_fills`` so the
    runner uses it as a drop-in for the REST fill poll.
    """

    def __init__(self, creds: dict, condition_ids=None, *, url: str = USER_WS,
                 invert_side: bool = False) -> None:
        # creds: {"apiKey","secret","passphrase"}
        self.url = url
        self.creds = creds
        self._invert = invert_side
        self._lock = threading.Lock()
        self._queue: deque = deque()
        self._seen: set = set()                   # trade ids already enqueued (dedup)
        self._markets = set(c for c in (condition_ids or []) if c)
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._ws = None

    @staticmethod
    def subscribe_msg(creds: dict, markets) -> str:
        return json.dumps({
            "type": "user",
            "auth": {"apiKey": creds.get("apiKey") or creds.get("api_key"),
                     "secret": creds.get("secret") or creds.get("api_secret"),
                     "passphrase": creds.get("passphrase") or creds.get("api_passphrase")},
            "markets": list(markets),
        })

    def handle_message(self, raw) -> None:
        try:
            data = json.loads(raw)
        except (ValueError, TypeError):
            return
        for ev in (data if isinstance(data, list) else [data]):
            if not isinstance(ev, dict) or ev.get("event_type") != "trade":
                continue
            tid = ev.get("id")
            with self._lock:
                if tid is not None and tid in self._seen:
                    continue                       # dedup repeated status updates
                if tid is not None:
                    self._seen.add(tid)
                    if len(self._seen) > 5000:     # bound the dedup set
                        self._seen = set(list(self._seen)[-2000:])
                side = str(ev.get("side", "")).upper()
                if self._invert:
                    side = "SELL" if side == "BUY" else "BUY"
                self._queue.append({
                    "id": tid,
                    "token_id": ev.get("asset_id"),
                    "side": side,
                    "size": _f(ev.get("size")),
                    "price": _f(ev.get("price")),
                })

    def poll_fills(self) -> list[dict]:
        """Drain and return all fills received since the last call."""
        with self._lock:
            out = list(self._queue)
            self._queue.clear()
        return out

    def set_markets(self, condition_ids) -> None:
        self._markets = set(c for c in condition_ids if c)
        self._resubscribe()

    def _resubscribe(self) -> None:  # pragma: no cover - needs a live socket
        ws = self._ws
        if ws is None or not self._markets:
            return
        try:
            ws.send(self.subscribe_msg(self.creds, list(self._markets)))
        except Exception as e:  # noqa: BLE001
            log.warning("user resubscribe failed: %s", e)

    def start(self) -> None:  # pragma: no cover - needs a live WS
        if self._thread is not None:
            return
        self._thread = threading.Thread(target=self._run, name="ws-user", daemon=True)
        self._thread.start()

    def stop(self) -> None:  # pragma: no cover - needs a live WS
        self._stop.set()
        ws = self._ws
        if ws is not None:
            try:
                ws.close()
            except Exception:  # noqa: BLE001
                pass
        if self._thread is not None:
            self._thread.join(timeout=5.0)

    def _run(self) -> None:  # pragma: no cover - needs a live WS
        import websocket
        while not self._stop.is_set():
            try:
                self._ws = websocket.create_connection(self.url, timeout=15)
                self._ws.settimeout(1.0)
                self._resubscribe()
                last_ping = time.monotonic()
                while not self._stop.is_set():
                    now = time.monotonic()
                    if now - last_ping >= _PING_INTERVAL_S:
                        self._ws.send("PING")
                        last_ping = now
                    try:
                        msg = self._ws.recv()
                    except Exception:
                        continue
                    if msg:
                        self.handle_message(msg)
            except Exception as e:  # noqa: BLE001
                if self._stop.is_set():
                    break
                log.warning("user WS reconnecting after error: %s", e)
                self._stop.wait(2.0)
            finally:
                try:
                    if self._ws is not None:
                        self._ws.close()
                except Exception:  # noqa: BLE001
                    pass
                self._ws = None
