# polymarket-live-maker

Autonomous **live liquidity-rewards market maker** for Polymarket. This is the
proven `polymarket-paper-trader` codebase, **REST/API-based** (`py-clob-client` —
no official SDK, no WebSocket), **polling-driven on the same cadence as before**,
with **MCP removed** and one thing added: it actually **places real orders**
through the CLOB REST API — gated behind an explicit operator switch (dry-run by
default). No LLM anywhere in the loop.

> The strategy and its economics are unchanged and documented in
> [`docs/research/04-lp-rewards-edge.md`](docs/research/04-lp-rewards-edge.md):
> Polymarket pays a daily USDC pool to two-sided limit orders resting within
> `max_spread` of mid. The bot discovers safe, low-jump mid-tail reward pools,
> quotes a small decorrelated book, manages inventory with skewed quotes, and
> exits + benches a pool on a catalyst jump. Read it before risking money.

## What changed vs the paper-trader

It is the same package (`pm_trader`, same modules, same logic, same tests) **minus
MCP**, plus the live wiring:

- **`maker_live.ClobSubmitter`** — implements the previously-stubbed live signer:
  real `place` / `cancel` via `py-clob-client`, and `poll_fills()` to read the
  account's **real trades** (you chose REST trade-polling over a WS user channel).
- **`LiveMakerBot(external_fills=True)`** — in live mode inventory comes from those
  real fills; dry-run keeps the original mid-cross inference (一模一样).
- **`runner.py`** — the autonomous, single-process **polling loop** that replaces
  the agent/MCP that used to call the maker poll by hand: rediscover → select →
  step each bot every `poll_seconds`, with cooldown + kill-switch.

Everything else (reward math, scanner, discovery, portfolio selection, engine
ledger, CLI) is the paper-trader as-is.

## Install

```bash
python -m venv .venv && source .venv/bin/activate
pip install -e .              # click + httpx (dry-run + CLI)
pip install -e ".[live]"      # adds py-clob-client (real order submission)
pip install -e ".[dev]"       # test extras
```

## Configure

```bash
cp .env.example .env          # .env is gitignored — NEVER commit it
$EDITOR .env                  # POLYMARKET_PRIVATE_KEY (+ funder/signature type)
```

## Run — dry-run (default, no orders sent)

```bash
python -m pm_trader.runner    # or: live-maker
```

It discovers pools, polls books/mids on the cadence, computes the exact two-sided
quotes and cancels, and **logs every order it WOULD place** — sending nothing.
Confirm it picks sane pools and reacts to moves before going live.

The original CLI is still here too (`pm-trader scan`, `pm-trader maker ...`,
`pm-trader watch ...`, etc.) for manual inspection.

## Go live — the operator's explicit switch

Real orders flow **only** when you set, in `.env` (or the environment):

```bash
PM_TRADER_LIVE=1
```

Nothing flips this for you. The live submitter derives API creds from your key and
posts/cancels real orders. **Stage the ramp:** start tiny (`LM_CAPITAL=200`,
`LM_MAX_POOLS=3`), confirm **jump survival over days**, then scale.

### Safety controls

- **Kill-switch** (`runner._kill_check`) — trips on a `KILL` file (`touch KILL` or
  in `state/`) or the day's conservative book mark-to-market (from real fills)
  breaching `LM_MAX_LOSS_PER_DAY`. On a trip it cancels all orders and stands down.
- **Per-pool inventory cap** + inventory-skewed quotes (`LiveMakerBot`).
- **Jump-halt + cooldown** — a pool that moves beyond the reward band is cancelled
  and benched for several discovery rounds (`LiveMakerBot` halt + runner cooldown).
- **SIGINT/SIGTERM** — graceful shutdown cancels every order.

### Before real money: smoke-test the API path

`py-clob-client` is not exercised by the test suite (it's behind `PM_TRADER_LIVE`
and imported lazily). The exact field names for order responses / `get_trades`
should be confirmed on a tiny live run before scaling — `ClobSubmitter` is written
defensively but unverified end-to-end here.

## Tests

```bash
pip install -e ".[dev]"
pytest -q -m "not live"       # 890+ tests; no network, no SDK, no MCP
```
