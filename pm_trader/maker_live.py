"""Live maker-quoting infrastructure for the liquidity-rewards strategy.

This is the bridge from the validated PAPER strategy to a real CLOB market-making
bot.  It computes the two-sided quotes to rest in a reward pool and the
cancel/re-quote actions as the midpoint moves (the fast-cancel behaviour that, on
colocated infra, shrinks adverse selection).

SAFETY — real money is hard-gated:
  - ``dry_run=True`` is the default.  In dry-run the bot computes and returns the
    orders it WOULD place/cancel and submits NOTHING to any network.
  - Real submission requires ALL of: ``dry_run=False``, an injected ``signer``
    (a configured py-clob-client), and the operator's funded wallet.  The
    ``build_clob_signer`` factory lazily imports py-clob-client and refuses
    unless ``PM_TRADER_LIVE=1`` and a private key are present in the environment.
  - This module never funds a wallet or holds keys; the operator wires the signer
    and flips the switch.  Nothing here places a real order on its own.
"""

from __future__ import annotations

import logging
import os

from pm_trader.models import ApiError, OrderBook
from pm_trader.orderbook import book_inband_qmin, committed_capital, maker_reward_share


def compute_two_sided_quotes(
    mid: float,
    *,
    half_spread_c: float,
    size: float,
    tick: float,
    max_spread_c: float,
    skew_ticks: float = 0.0,
) -> list[dict]:
    """Return the two resting orders (a YES bid + a YES ask) to quote at ``mid``.

    Quotes rest ``half_spread_c`` cents either side of mid, rounded to ``tick``,
    clamped into [tick, 1-tick].  Raises if the offset is outside the reward band.

    ``skew_ticks`` shifts BOTH quotes by that many ticks to work inventory back to
    flat (this is "using the other side"): when long (positive skew) both quotes
    move DOWN — the ask nears mid (sells eagerly to shed the long) while the bid
    backs off (stops adding) — and symmetrically up when short.
    """
    if not 0.0 < mid < 1.0:
        raise ValueError(f"mid must be in (0, 1), got {mid}")
    if half_spread_c <= 0 or half_spread_c > max_spread_c:
        raise ValueError(
            f"half_spread_c must be in (0, {max_spread_c}], got {half_spread_c}"
        )
    offset = half_spread_c / 100.0
    shift = skew_ticks * tick
    bid = max(tick, round((mid - offset - shift) / tick) * tick)
    ask = min(1.0 - tick, round((mid + offset - shift) / tick) * tick)
    return [
        {"side": "BUY", "price": round(bid, 4), "size": size},
        {"side": "SELL", "price": round(ask, 4), "size": size},
    ]


def plan_requote(
    mid_prev: float, mid_now: float, *, half_spread_c: float, tick: float
) -> bool:
    """Whether the mid moved enough to warrant cancelling and re-centering.

    A move of at least one tick means a resting quote is now off-center and (if
    the move is toward a side) at risk of being picked off — re-quote.
    """
    return abs(mid_now - mid_prev) >= tick


class LiveMakerBot:
    """Plans live maker quotes for one reward pool; submits only when ungated.

    ``submitter`` is a callable ``(action: dict) -> dict`` (inject a real
    CLOB-backed one for live; the default dry-run submitter returns a plan echo
    and touches no network).
    """

    def __init__(
        self,
        *,
        token_id: str,
        max_spread_c: float,
        min_size: float,
        tick: float,
        half_spread_c: float | None = None,
        size: float | None = None,
        dry_run: bool = True,
        submitter=None,
        max_inventory: float | None = None,
        skew_strength_ticks: float = 2.0,
        jump_exit_ticks: float | None = None,
        external_fills: bool = False,
    ) -> None:
        self.token_id = token_id
        # When True, inventory is fed from REAL fills via ``apply_real_fill`` (the
        # live path: poll the account's trades) instead of inferred from mid moves.
        # Default False keeps the original paper behaviour (mid-cross inference).
        self.external_fills = external_fills
        self.max_spread_c = max_spread_c
        self.min_size = min_size
        self.tick = tick
        self.half_spread_c = (tick * 100.0) if half_spread_c is None else half_spread_c
        self.size = min_size if size is None else size
        self.dry_run = dry_run
        if not dry_run and submitter is None:
            raise ApiError("Live mode requires an injected signer/submitter")
        self._submitter = submitter or self._dry_run_submit
        self.last_mid: float | None = None
        # A move beyond the reward band is a catalyst jump, not normal drift: the
        # market just proved it's no longer the quiet SAFE pool we entered, so we
        # exit and flag it for cooldown instead of blindly re-quoting into it.
        self.jump_exit_ticks = (
            (max_spread_c / (tick * 100.0)) if jump_exit_ticks is None else jump_exit_ticks
        )
        self.halted = False
        # Inventory state: net YES shares held (+ long, − short), built when a
        # resting side is filled (picked off).  max_inventory caps the position;
        # skew_strength_ticks is how far quotes shift at full inventory.
        self.max_inventory = max_inventory if max_inventory is not None else 5.0 * self.size
        self.skew_strength_ticks = skew_strength_ticks
        self.inventory = 0.0
        self.last_bid: float | None = None
        self.last_ask: float | None = None

    @staticmethod
    def _dry_run_submit(action: dict) -> dict:
        """Default submitter: echo the action as a planned (un-sent) order."""
        return {"status": "DRY_RUN", **action}

    def _skew_ticks(self) -> float:
        """Quote shift (ticks) to pull inventory toward flat; 0 when flat.

        Positive (shift quotes DOWN) when long, negative (UP) when short, scaled
        by how close inventory is to its cap and clamped at ``skew_strength_ticks``.
        """
        if self.max_inventory <= 0:
            return 0.0
        ratio = max(-1.0, min(1.0, self.inventory / self.max_inventory))
        return self.skew_strength_ticks * ratio

    def plan(self, book: OrderBook, mid: float) -> dict:
        """Compute the actions for this poll: (re)quote if needed, with reward est."""
        skew = self._skew_ticks()
        quotes = compute_two_sided_quotes(
            mid, half_spread_c=self.half_spread_c, size=self.size,
            tick=self.tick, max_spread_c=self.max_spread_c, skew_ticks=skew,
        )
        requote = self.last_mid is None or plan_requote(
            self.last_mid, mid, half_spread_c=self.half_spread_c, tick=self.tick
        )
        existing_qmin = book_inband_qmin(book, mid, self.max_spread_c)
        share = maker_reward_share(
            self.size, self.half_spread_c, self.max_spread_c, existing_qmin
        )
        return {
            "token_id": self.token_id,
            "mid": mid,
            "requote": requote,
            "orders": quotes,
            "est_reward_share": round(share, 4),
            "committed_capital": round(committed_capital(self.size, self.half_spread_c), 2),
            "inventory": round(self.inventory, 4),
            "skew_ticks": round(skew, 4),
            "dry_run": self.dry_run,
        }

    def _detect_fill(self, mid: float) -> None:
        """Update inventory if the mid crossed a resting quote since last step.

        Mid up through our ask ⇒ ask lifted ⇒ we SOLD ⇒ inventory falls (short);
        mid down through our bid ⇒ bid hit ⇒ we BOUGHT ⇒ inventory rises (long).
        Clamped to ±max_inventory (beyond the cap the bot would stop/flatten).
        """
        if self.last_ask is not None and mid >= self.last_ask:
            self.inventory -= self.size
        elif self.last_bid is not None and mid <= self.last_bid:
            self.inventory += self.size
        if self.max_inventory > 0:
            self.inventory = max(-self.max_inventory, min(self.max_inventory, self.inventory))

    def apply_real_fill(self, side: str, size: float) -> None:
        """Apply a REAL fill (from the account's trade feed) to inventory.

        BUY -> long YES (inventory up), SELL -> short YES (inventory down), clamped
        to the position cap. Used in live mode (``external_fills=True``) so the skew
        works the true on-chain position, not a mid-cross guess.
        """
        if str(side).upper() == "BUY":
            self.inventory += size
        else:
            self.inventory -= size
        if self.max_inventory > 0:
            self.inventory = max(-self.max_inventory, min(self.max_inventory, self.inventory))

    def _halt_plan(self, mid: float, submitted: list) -> dict:
        """The output shape when the market is halted (exited, awaiting cooldown)."""
        return {
            "token_id": self.token_id, "mid": mid, "requote": False, "orders": [],
            "submitted": submitted, "halted": True, "recommend": "exit_cooldown",
            "inventory": round(self.inventory, 4),
        }

    def step(self, book: OrderBook, mid: float) -> dict:
        """Plan and (only if requote needed) submit the cancel+repost actions.

        Order of decisions each poll:
          1. If already halted → stay out (idle); the pool is on cooldown.
          2. Mark any fill from the move (updating inventory) so the next quote is
             skewed to work that inventory back toward flat.
          3. If the move was a catalyst-size JUMP (beyond the reward band) → the
             market is no longer the quiet SAFE pool we entered: cancel and HALT,
             recommending exit + cooldown rather than blindly re-quoting.
          4. Otherwise re-quote (skewed) as normal.
        """
        if self.halted:
            self.last_mid = mid
            return self._halt_plan(mid, [])
        moved = abs(mid - self.last_mid) if self.last_mid is not None else 0.0
        if not self.external_fills:
            self._detect_fill(mid)
        if moved >= self.jump_exit_ticks * self.tick:
            self.halted = True
            submitted = [self._submitter({"action": "CANCEL_ALL", "token_id": self.token_id})]
            self.last_mid = mid
            return self._halt_plan(mid, submitted)

        plan = self.plan(book, mid)
        plan["halted"] = False
        submitted = []
        if plan["requote"]:
            if self.last_mid is not None:
                submitted.append(self._submitter({"action": "CANCEL_ALL", "token_id": self.token_id}))
            for o in plan["orders"]:
                submitted.append(self._submitter({"action": "PLACE", "token_id": self.token_id, **o}))
        self.last_mid = mid
        self.last_bid = plan["orders"][0]["price"]
        self.last_ask = plan["orders"][1]["price"]
        plan["submitted"] = submitted
        return plan


CLOB_HOST = "https://clob.polymarket.com"
POLYGON_CHAIN_ID = 137


class ClobSubmitter:  # pragma: no cover - requires external lib + live creds
    """Real CLOB order submitter over the public REST API (``py-clob-client``).

    Callable as ``submitter(action)`` so it drops straight into ``LiveMakerBot``'s
    ``submitter`` slot (same interface as the dry-run echo). Actions:
      - ``{"action": "PLACE", "token_id", "side", "price", "size"}`` -> GTC limit order
      - ``{"action": "CANCEL_ALL", "token_id"}``                     -> cancel that token's orders

    It also tracks live order ids per token and exposes :meth:`poll_fills` (polls
    the account's REAL trades — the operator chose REST trade-polling over the WS
    user channel) so the runner can reconcile real inventory.

    Hard-gated: nothing constructs this unless ``PM_TRADER_LIVE=1`` and a key are
    in the environment. Excluded from coverage — it touches real funds.
    """

    def __init__(self) -> None:
        if os.environ.get("PM_TRADER_LIVE") != "1":
            raise ApiError("Refusing live signer: set PM_TRADER_LIVE=1 to opt in")
        pk = os.environ.get("POLYMARKET_PRIVATE_KEY")
        if not pk:
            raise ApiError("Refusing live signer: POLYMARKET_PRIVATE_KEY not set")
        try:
            from py_clob_client.client import ClobClient
        except ImportError as e:
            raise ApiError("py-clob-client not installed; pip install py-clob-client") from e

        funder = os.environ.get("POLYMARKET_FUNDER") or None
        sig_type = os.environ.get("POLYMARKET_SIGNATURE_TYPE")
        kwargs = {"key": pk, "chain_id": POLYGON_CHAIN_ID}
        if funder:
            kwargs["funder"] = funder
        if sig_type is not None:
            kwargs["signature_type"] = int(sig_type)
        self._client = ClobClient(CLOB_HOST, **kwargs)
        # L2 API creds for authenticated order ops (derived from the key, idempotent)
        self._client.set_api_creds(self._client.create_or_derive_api_creds())
        # Some CLOB deployments report a trade's `side` from the TAKER's perspective.
        # If a smoke-test shows our maker fills arrive with the inverted side (e.g. a
        # BUY fill labelled SELL), set POLYMARKET_FILL_SIDE_INVERT=1 to correct it.
        self._invert_side = os.environ.get("POLYMARKET_FILL_SIDE_INVERT", "0") == "1"
        self._own_taker_ids: set = set()  # our flatten (taker) order ids -> exclude from fills
        try:
            self._own_address = self._client.get_address()  # to fetch only OUR maker fills
        except Exception:  # noqa: BLE001
            self._own_address = None
        # Prime the trade cursor to the NEWEST existing trade so the first poll_fills
        # returns only trades AFTER startup (never replays the account's history).
        try:
            seed = self._client.get_trades() or []
            self._last_trade_id = (seed[0].get("id") or seed[0].get("trade_id")) if seed else None
        except Exception:  # noqa: BLE001 — best effort; worst case first poll is empty-safe
            self._last_trade_id = None

    def __call__(self, action: dict) -> dict:
        kind = action.get("action")
        token = action.get("token_id")
        try:
            if kind == "PLACE":
                return self._place(token, action["side"], action["price"], action["size"])
            if kind == "CANCEL_ALL":
                return self._cancel_all(token)
            if kind == "FLATTEN":
                return self._flatten(token, action["side"], action["size"])
        except Exception as e:  # noqa: BLE001 — surface, never crash the poll loop
            return {"status": "ERROR", "error": str(e), **action}
        return {"status": "IGNORED", **action}

    def _place(self, token_id, side, price, size) -> dict:
        # RECOMMENDED one-step helper; posts GTC (a resting maker quote). create_order
        # auto-resolves tick_size + neg_risk + fee and validates the price.
        from py_clob_client.clob_types import OrderArgs
        from py_clob_client.order_builder.constants import BUY, SELL

        resp = self._client.create_and_post_order(OrderArgs(
            token_id=token_id, price=float(price), size=float(size),
            side=(BUY if str(side).upper() == "BUY" else SELL)))
        r = resp or {}
        oid = r.get("orderID") or r.get("order_id")
        status = str(r.get("status", "")).strip().lower()
        # a GTC place succeeds if it rests (live) or matches immediately
        # (matched/delayed); only 'unmatched' / no-success is a reject.
        ok = bool(r.get("success", oid is not None)) and status != "unmatched"
        return {"status": "PLACED" if ok else "REJECTED", "order_id": oid,
                "token_id": token_id, "side": side, "price": price, "size": size,
                "resp": resp}

    def _cancel_all(self, token_id) -> dict:
        # the latency-sensitive op; cancel every resting order on this token
        resp = self._client.cancel_market_orders(asset_id=token_id)
        return {"status": "CANCELLED", "token_id": token_id, "resp": resp}

    def _flatten(self, token_id, side, size) -> dict:
        """Go flat with the RECOMMENDED market-order helper, FOK (all-or-nothing).

        Per the docs, market orders use create_market_order(MarketOrderArgs) where
        ``amount`` is SHARES to sell (SELL) or USDC to spend (BUY) — so a short
        cover sizes the USDC as shares*marketable-ask via get_price. create_market_order
        auto-resolves tick_size/neg_risk and the marketable price. FAIL CLOSED: if it
        doesn't fill, return ERROR so the engine keeps the position (never zeroes a
        live position). Confirm the fill-status parsing on the smoke-test.
        """
        from py_clob_client.clob_types import MarketOrderArgs, OrderType
        from py_clob_client.order_builder.constants import BUY, SELL

        s = str(side).upper()
        if s == "SELL":
            args = MarketOrderArgs(token_id=token_id, amount=float(size),
                                   side=SELL, order_type=OrderType.FOK)
        else:
            try:  # BUY cover: amount is USDC ~= shares * marketable ask
                px = float((self._client.get_price(token_id, "BUY") or {}).get("price", 0)) or 0.99
            except Exception:  # noqa: BLE001
                px = 0.99
            args = MarketOrderArgs(token_id=token_id, amount=float(size) * px,
                                   side=BUY, order_type=OrderType.FOK)
        signed = self._client.create_market_order(args)
        resp = self._client.post_order(signed, OrderType.FOK)
        if not self._order_filled(resp):
            # FOK killed (book couldn't fully absorb) -> position SURVIVES; fail closed.
            return {"status": "ERROR", "error": "flatten_unfilled", "token_id": token_id,
                    "side": side, "size": size, "resp": resp}
        oid = (resp or {}).get("orderID") or (resp or {}).get("order_id")
        if oid:
            self._own_taker_ids.add(oid)
            if len(self._own_taker_ids) > 500:
                self._own_taker_ids = set(list(self._own_taker_ids)[-200:])
        return {"status": "FLATTENED", "token_id": token_id, "side": side,
                "size": size, "order_id": oid, "resp": resp}

    @staticmethod
    def _order_filled(resp) -> bool:
        """Filled per the documented order statuses: 'matched' (matched immediately)
        or 'delayed' (accepted into async matching) count as filled; 'live' (resting)
        and 'unmatched' (marketable but failed) do NOT. Compare EXACTLY (note:
        'unmatched' contains the substring 'match'). Fail closed otherwise."""
        if not isinstance(resp, dict):
            return False
        status = str(resp.get("status", "")).strip().lower()
        if status in ("matched", "delayed"):
            return True
        for k in ("size_matched", "sizeMatched"):
            v = resp.get(k)
            try:
                if v is not None and float(v) > 0:
                    return True
            except (TypeError, ValueError):
                pass
        return False

    def _is_own_taker(self, t: dict) -> bool:
        """True if this trade is one of OUR flatten (taker) legs — exclude it from
        fills so we don't double-count our own taker as an inventory change."""
        for k in ("taker_order_id", "takerOrderId", "order_id", "orderID"):
            if t.get(k) in self._own_taker_ids:
                return True
        return False

    def list_open_orders(self) -> list[dict]:
        """All resting orders for this API key (for restart broker-reconciliation)."""
        from py_clob_client.clob_types import OpenOrderParams
        return self._client.get_orders(OpenOrderParams()) or []

    def cancel_order(self, order_id) -> dict:
        resp = self._client.cancel(order_id)
        return {"status": "CANCELLED", "order_id": order_id, "resp": resp}

    def usdc_balance(self) -> float | None:
        """The wallet's free USDC collateral (an INDEPENDENT figure vs the engine's
        self-reported ledger) for the wallet-floor kill-switch. USDC has 6 decimals."""
        from py_clob_client.clob_types import AssetType, BalanceAllowanceParams
        try:
            ba = self._client.get_balance_allowance(
                BalanceAllowanceParams(asset_type=AssetType.COLLATERAL))
            bal = ba.get("balance") if isinstance(ba, dict) else None
            return float(bal) / 1_000_000.0 if bal is not None else None
        except Exception:  # noqa: BLE001
            return None

    def poll_fills(self) -> list[dict]:
        """Return REAL maker fills since the last poll. Each: ``{token_id, side,
        size, price, id}``. Fetches only trades where WE are the MAKER
        (maker_address filter) — this excludes our own taker/flatten legs and pins
        the side to our maker perspective. The trade 'side' field perspective still
        needs a one-line smoke-test confirm (POLYMARKET_FILL_SIDE_INVERT corrects it)."""
        from py_clob_client.clob_types import TradeParams
        params = TradeParams(maker_address=self._own_address) if self._own_address else None
        trades = self._client.get_trades(params) or []
        out: list[dict] = []
        seen_new = False
        for t in trades:  # newest-first per CLOB convention
            tid = t.get("id") or t.get("trade_id")
            if tid is not None and tid == self._last_trade_id:
                break
            if not seen_new:
                self._last_trade_id = tid
                seen_new = True
            if self._is_own_taker(t):
                continue   # our own flatten leg, not a maker fill
            side = (t.get("side") or "").upper()
            if self._invert_side:
                side = "SELL" if side == "BUY" else "BUY"
            out.append({
                "id": tid,
                "token_id": t.get("asset_id") or t.get("token_id"),
                "side": side,
                "size": float(t.get("size", 0) or 0),
                "price": float(t.get("price", 0) or 0),
            })
        return out


def build_clob_signer():  # pragma: no cover - requires external lib + live creds
    """Build a real ``py-clob-client`` submitter. Hard-gated; never used in dry-run.

    Returns a :class:`ClobSubmitter` (callable like the old echo submitter, but it
    actually posts/cancels orders and can poll real fills). Refuses unless
    ``PM_TRADER_LIVE=1`` and ``POLYMARKET_PRIVATE_KEY`` are set.
    """
    return ClobSubmitter()


class DryRunSubmitter:
    """A submitter that LOGS every order it WOULD send and sends NOTHING.

    Lets the runner rehearse the EXACT live code path (``accrue_maker_rewards_live``
    — place / cancel / re-center / reconcile / drift-exit) without touching the
    network or funds. ``poll_fills`` returns ``[]`` (no real fills in dry mode), so
    inventory stays flat: the order-generation + exit logic are fully exercised;
    inventory skew / flatten are not (those need real fills, i.e. going live).
    """

    def __init__(self, logger: logging.Logger | None = None) -> None:
        self._log = logger or logging.getLogger("pm_trader.dryrun")
        self.sent: list[dict] = []

    def __call__(self, action: dict) -> dict:
        self.sent.append(action)
        rest = {k: v for k, v in action.items() if k != "action"}
        self._log.info("DRY %s %s", action.get("action"), rest)
        return {"status": "OK", "dry_run": True, **action}

    def poll_fills(self) -> list[dict]:
        return []
