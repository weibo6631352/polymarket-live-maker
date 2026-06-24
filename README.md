# polymarket-live-maker

Autonomous **live liquidity-rewards market maker** for Polymarket, built on the
official unified SDK [`polymarket-client`](https://docs.polymarket.com/dev-tooling/python)
(`AsyncSecureClient`). One async process, **WebSocket-driven with a REST
order-book failsafe**, **no LLM in the hot path**. Real-money submission is gated
behind the operator's explicit `PM_LIVE=1` switch — the default is dry-run.

> **What it does.** Polymarket pays a fixed daily USDC pool, from its own
> treasury, to limit orders resting within `max_spread` of the midpoint
> (two-sided, size-weighted). This bot continuously discovers the safe, low-jump,
> high-share **mid-tail** reward pools, quotes a small two-sided book across many
> uncorrelated pools, manages inventory with inventory-skewed quotes, and exits +
> benches a pool the moment a catalyst jump shows up. The validated economics are
> in [`docs/research/04-lp-rewards-edge.md`](docs/research/04-lp-rewards-edge.md)
> — read it before risking money.

## Architecture (one async process)

```
config.py       pools/capital/risk params + secrets from env (key, wallet); PM_LIVE gate
reward_math.py  pure reward/bleed/skew/optimal-quote math            (migrated, 100% tested)
scanner.py      async public-CLOB reader + pool scoring + jump-risk   (migrated)
discovery.py    periodic SAFE-pool re-scan of the full universe       (migrated)
portfolio.py    select_pools: risk-adjusted, decorrelated, cooldown   (migrated)
strategy.py     per-pool DECISIONS: quote/skew, requote, reconcile,   (migrated +
                jump/drift exit                                        engine reconcile)
execution.py    AsyncSecureClient adapter: place/cancel/flatten + the dry-run gate   (NEW)
feed.py         WS subscribe + REST order-book failsafe -> one event stream          (NEW)
runner.py       the loop: discover->select->subscribe->on-event->execute; reconcile;
                kill-switch; cooldown                                                (NEW)
```

The migrated modules are the **validated decision/reward logic** from the
paper-trader research build (no MCP, no paper accrual engine, no SQLite). The new
layer is just the live plumbing.

### WebSocket + order-book failsafe

The loop is driven by the SDK WebSocket (`client.subscribe([MarketSpec, UserSpec])`):
book snapshots, price changes, and **real wallet fills**. In parallel, `feed.py`
**pulls the full order book over REST** on a fixed cadence (`LM_BOOK_POLL_S`) and
immediately whenever the socket has been silent for `LM_WS_STALE_S`. Both sources
feed one normalized event stream, so if the WS drops, lags, or silently stalls,
the bot keeps seeing fresh books and keeps quoting/cancelling correctly. The WS
reader auto-reconnects with backoff while the REST failsafe carries the loop.

## Install

```bash
python -m venv .venv && source .venv/bin/activate
pip install -e .            # installs polymarket-client + httpx
# dev/test extras:
pip install -e ".[dev]"
```

## Configure

```bash
cp .env.example .env        # .env is gitignored — NEVER commit it
$EDITOR .env                # set POLYMARKET_PRIVATE_KEY (read from env only)
```

The private key is read from the environment **only**; the code never writes,
logs, or transmits it anywhere but the official SDK.

## Run — dry-run (default, no orders sent)

```bash
python -m live_maker        # or: live-maker
```

In dry-run the bot does everything real — discovers pools, subscribes to the WS,
pulls books, computes the exact two-sided quotes and cancels — but every
place/cancel/flatten is **logged and dropped**, not sent. Watch the logs to
confirm it picks sane pools and reacts to moves before risking funds.

## Go live — the operator's explicit switch

Real orders flow **only** when you set, in `.env` (or the environment):

```bash
PM_LIVE=1
```

Nothing flips this for you. On the first live start the bot runs
`setup_trading_approvals()` once (wallet deploy + USDC approvals). **Stage the
ramp** (see [the brief](docs/research/LIVE-MIGRATION-BRIEF.md) §5):

1. Start tiny — `LM_CAPITAL=200`, `LM_MAX_POOLS=3`, `min_size` quotes.
2. Confirm **jump survival over days** (a catalyst jump can erase a week of
   reward; the strategy exits + benches on a jump, but you must verify it live).
3. Only then scale capital and pools.

### Safety controls (all in `runner.py`)

- **Kill-switch** — trips on either a `KILL` file (`touch KILL`, or in `state/`)
  or the day's conservative book mark-to-market breaching `LM_MAX_LOSS_PER_DAY`.
  On a trip it cancels every order, flattens inventory, and stands down.
- **Per-pool inventory cap** — `LM_MAX_INVENTORY_MULT * min_size` shares;
  inventory-skewed quotes lean the book back to flat before the cap.
- **Reconcile** — every `LM_RECONCILE_INTERVAL_S` it re-checks each pool's reward
  config and exits any pool that left the program or resolved.
- **Cooldown** — a pool that jumped is benched for several discovery rounds before
  it can be re-selected.
- **SIGINT/SIGTERM** — graceful shutdown: cancel all, flatten, close.

### Honest status (from the research)

The edge is **thin and capacity-constrained**: net-positive in calm windows, but
a catalyst jump can erase days of reward. In calm markets, optimal-width quoting
already neutralizes bleed, so colocation/proximity adds ~0% to the MM itself —
don't quote tight chasing share unless you can cancel fast. This is a real but
modest edge; treat the annualized figures in the research as regime-dependent and
optimistic until proven across weeks of jumps.

## Tests

```bash
pip install -e ".[dev]"
pytest -q --cov=live_maker --cov-report=term-missing
```

The migrated pure logic (`reward_math`, `scanner` scoring, `discovery`,
`portfolio`, `strategy`) is kept 100%-covered. No test touches the network or
requires the SDK (the SDK is imported lazily in `execution`/`feed`).
