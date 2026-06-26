# Live smoke-test checklist

Before letting the bot trade real money unattended, run this **tiny, supervised**
checklist with a funded wallet and a few dollars. It confirms the handful of things
that genuinely cannot be verified offline (the live CLOB response shapes and the
fill-side perspective). Everything else was verified against the py-clob-client
0.17.x source and the official docs.

> **Reminder:** the execution layer uses py-clob-client's RECOMMENDED helpers
> (`create_and_post_order` GTC for resting quotes; `create_market_order` FOK for
> flatten). Latest published is 0.34.x and works against the live CLOB; its GitHub
> repo is archived (future dev is in the unified `py-sdk`), so plan to migrate
> eventually. This checklist confirms the live response shapes either way.

## 0. Setup
```bash
pip install -e ".[live,dev]"
cp .env.example .env   # set POLYMARKET_PRIVATE_KEY (+ funder / signature type)
```
- [ ] Fund the wallet with a small amount of USDC (e.g. $20–50).
- [ ] One-time approvals: the bot calls `setup_trading_approvals()` only when live,
      OR run it once yourself. Confirm USDC allowance is set for the CLOB exchange.
- [ ] Set conservative limits in `.env`: `LM_CAPITAL=20` (tiny capital is the
      exposure throttle — it only funds ~1 pool), `LM_MAX_LOSS_PER_DAY=5`, and the
      hard wallet floor `LM_MIN_WALLET_USDC=<your floor>`.

## 1. Signer / identity (one Python REPL)
```python
from py_clob_client.client import ClobClient
c = ClobClient("https://clob.polymarket.com", key=PK, chain_id=137,
               funder=FUNDER or None, signature_type=SIG or None)
c.set_api_creds(c.create_or_derive_api_creds())
print("maker address:", c.get_address())
```
- [ ] `get_address()` is the wallet you funded (NOT a wrong/derived address).
- [ ] If you use a Polymarket proxy wallet, `funder` + `signature_type` (1 or 2)
      are set so orders hit the funded wallet.

## 2. Place + cancel ONE real order (read the raw response)
```python
from py_clob_client.clob_types import OrderArgs, OrderType, OpenOrderParams
o = c.create_order(OrderArgs(token_id=TOK, price=0.10, size=5, side="BUY"))  # far from mid
resp = c.post_order(o, OrderType.GTC)
print(resp)                       # <-- inspect EVERY key
print(c.get_orders(OpenOrderParams()))
c.cancel(resp["orderID"])
```
- [ ] Confirm the response keys the bot reads: **`success`**, **`orderID`**,
      **`status`** (one of `matched`/`live`/`delayed`/`unmatched`). The bot's
      `_order_filled` treats EXACTLY `status == "matched"` as filled — confirm that
      string. (`execution`/`maker_live.py:_order_filled`.)
- [ ] `get_orders(OpenOrderParams())` lists the resting order (used for restart
      reconciliation). Note the **`asset_id`** + **`id`/`orderID`** field names.

## 3. FOK flatten (the safety-critical op)
```python
# with a tiny long position, market it out:
f = c.create_order(OrderArgs(token_id=TOK, price=0.01, size=POS, side="SELL"))
print(c.post_order(f, OrderType.FOK))    # status should be 'matched' if it filled
```
- [ ] A fully-filled FOK returns `status == "matched"`; an unfilled one returns
      `unmatched`. The bot **fails closed** on anything but `matched` (keeps the
      position, retries) — confirm the status string so flatten actually closes.

## 4. Fills / `get_trades` — the ONE perspective check
After step 2/3 produced at least one fill:
```python
from py_clob_client.clob_types import TradeParams
ts = c.get_trades(TradeParams(maker_address=c.get_address()))
print(ts[0])     # <-- inspect: side, size, price, asset_id, maker/taker fields
```
- [ ] Each trade returned has YOU as maker (the bot filters by `maker_address`).
- [ ] **Side perspective:** confirm the `side` field matches YOUR maker action
      (your resting BUY that got hit shows up as BUY). If it's inverted (taker
      perspective), set `POLYMARKET_FILL_SIDE_INVERT=1`. **This is the single most
      important check** — a wrong sign flips inventory and skews/flattens the wrong
      way.
- [ ] Field names: `side`, `size`, `price`, and `asset_id`/`token_id` match what
      `poll_fills` reads (`maker_live.py:poll_fills`).

## 5. Wallet-balance kill-switch
```python
from py_clob_client.clob_types import BalanceAllowanceParams, AssetType
print(c.get_balance_allowance(BalanceAllowanceParams(asset_type=AssetType.COLLATERAL)))
```
- [ ] Confirm the `balance` field is raw USDC (6 decimals) so `usdc_balance()`
      divides by 1e6 correctly, and that `LM_MIN_WALLET_USDC` will trip as intended.

## 6. Supervised dry-live → live
- [ ] Run **dry-live** first (`python -m pm_trader.runner`, default) and watch the
      logged `DRY PLACE/CANCEL` decisions against the real book for a while.
- [ ] Flip `PM_TRADER_LIVE=1`, start with `LM_CAPITAL=20`, and WATCH
      the first place → re-center → exit cycle end-to-end. Verify on the Polymarket
      UI that orders appear/cancel and inventory matches the ledger.
- [ ] Test the kill-switch: `touch KILL` → confirm it cancels everything and stops.
- [ ] Only after a clean multi-hour (ideally multi-day, across a jump) run, scale
      `LM_CAPITAL` up.

## What this checklist de-risks (already verified, not blocking)
OrderType GTC/FOK/GTD/FAK exist; `OrderArgs.size` is shares (flatten is
denomination-safe); `create_order` auto-resolves tick_size + neg_risk + fee and
validates price; `cancel_market_orders(asset_id=)` is correct; restart no longer
wipes the ledger and reconciles orphaned orders. The open items above are only the
response-shape / side-perspective confirmations that need one real round-trip.
