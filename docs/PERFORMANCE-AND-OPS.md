# Performance, Profitability & Ops — go-live reference

*Investigation + hardening pass, 2026-06-25. All measurements are real (live CLOB
reads through the operator's proxy, or the real engine driven offline). No orders
were ever placed; `PM_TRADER_LIVE` stayed `0` throughout.*

This is the consolidated record of a four-track review (performance/IO, dynamic
testing+latency, profitability, infra sizing), the optimizations it drove, and the
long-run stability verification. Read [`research/04-lp-rewards-edge.md`](research/04-lp-rewards-edge.md)
first for the strategy economics.

---

## 0. TL;DR

- **The one profit lever is cancel latency** (faster cancels → less adverse bleed).
  Everything below serves that.
- The poll loop was **fully synchronous**: per-pool reads serialized, a fixed 60s
  sleep stacked on top of the work. At scale a cycle blew past the interval.
- **Fixed:** concurrent per-pool I/O (9.5× measured), deadline scheduling,
  background discovery, `synchronous=NORMAL`, bounded dry-run buffer, equity-curve
  pruning. **933/933 tests green; soak-tested leak-free over 50k+ cycles.**
- **Profit is a small-account edge:** best at **$200–$400** (~$9/day expected,
  ~1,700%/yr on capital). It **saturates at ~6 pools / ~$265 committed** — $3000
  earns roughly the same dollars as $1000.
- **Infra:** a **$5 Lightsail box in us-east-1** is not the bottleneck; the
  **proxy hop is** (~0.5s/GET). Region + proxy placement matter far more than CPU.
- **Reward income shown by the bot is a real-time ESTIMATE; Polymarket settles &
  pays daily.** The bot ledger and the on-chain wallet are NOT auto-reconciled.

---

## 1. Performance / IO audit (static)

The loop is `runner.LiveRunner.run`: a single sync process. Per logical cycle each
held pool touched the network in three places (reeval: config+book+history; poll:
config+book+midpoint; plus cancel/replace on a re-center) — **all serialized**.

Top findings (by profit impact):

1. **Cancel latency was throttled by the fixed 60s sleep AND scaled with pool
   count.** A stale order after a mid jump was only cancelled on the next poll,
   after prior pools' I/O drained. Worst case ≈ `poll_seconds + Σ preceding-pool
   RTTs`: ~60s at N=3, **~104–150s at N=45**. Per the edge doc, bleed scales
   linearly with `(1 − cancel_efficiency)` — a 60–150s cancel makes it ≈ 0.
2. **Per-cycle round-trips fully serialized, ~5–6 per pool.** A combined cycle at
   N=45 issued ~270–400 serialized RTTs ≈ 54–81s of I/O **on top of** the 60s
   sleep → effective period drifted 66s → 141s.
3. **`get_reward_config` re-fetched per pool per poll, never cached;** book fetched
   twice on reeval beats via two clients; midpoint a separate call though derivable
   from the book.

Already-good: explicit httpx timeouts (no hung-socket risk), WAL + reused sqlite
connection, bounded `_own_taker_ids`. Single-process ⇒ no lock contention.

## 2. Dynamic testing + measured latency

Test suite (`pytest -q -m "not live"`): **933 passed, 0 failed, ~4.5s**, fully
hermetic. No flakiness.

Measured CLOB read latency **through the proxy** (20 iters each, p50):

| Call | p50 | p90 | max |
|---|---|---|---|
| Single GET (book / reward_config) | ~320–560 ms | ~570 ms | ~930 ms |
| Per-pool serial (config+book+history) | 1.41 s | 1.50 s | 1.83 s |
| Full `/sampling-markets` scan (8 pages, ~7.8k mkts) | ~5.2 s | — | 6.4 s |

Per-cycle work vs the (old) 60s interval, **serial**: N=3 → 4.2s (7%); N=15 → 21s
(35%); **N=45 → 63s (overruns)**. The ~0.5s/GET floor is proxy-inflated; a direct
US route would be materially lower. **The cancel path rides this same floor.**

## 3. Profitability ($200 / $1000 / $3000)

Live scan 2026-06-25: 7,763 reward pools; the marquee pools have millions in-band
(your share ≈ 0). The business is the mid-tail (~$100–500/day, min_size 50). Net,
using the realistic-set per-pool anchor (NOT the selection-biased snapshot share):

| Capital | Pools | Committed | **Net $/day (cons/exp/opt)** | Annualized on capital |
|---|---|---|---|---|
| $200 | 4 | $167 | **+5.2 / +9.2 / +15.2** | 950% / 1,680% / 2,770% |
| $1000 | 6 | $265 | **+7.8 / +13.8 / +22.8** | 285% / 500% / 830% |
| $3000 | 6 | $265 | **+7.8 / +13.8 / +22.8** | 95% / 168% / 277% |

**Capacity ceiling (the headline): the book saturates at ~6 uncorrelated SAFE
mid-tail pools / ~$265 committed.** Beyond ~$300 the marginal dollar earns ≈ 0 at
acceptable risk — $3000 mostly parks idle cash. This is a **small-account edge**;
to scale you need the colocation/latency edge (more size, tighter spreads, low
bleed), not more capital. Biggest swing factors: (1) sustainable share after your
quote reveals the pool (doc shows ~20× compression vs snapshot), (2) cancel
latency / jump frequency (the bleed term), (3) count of uncorrelated SAFE pools.

## 4. Infra sizing (AWS Lightsail)

Measured steady-state RSS ≈ **65–80 MB** (the eth-* live-signing stack is the bulk,
+34 MB; the app is ~30 MB). CPU near-idle between polls; disk trivial; bandwidth
trivial.

| Pick | Plan | RAM | vCPU | $/mo | Why |
|---|---|---|---|---|---|
| **Minimum** | $5 | 512 MB | 2 burst | $5 | ~6× RAM headroom; I/O-bound, burst credits never deplete |
| **Recommended** | $7 | 1 GB | 2 burst | $7 | no swap needed; logging/leak margin |

**The real constraint is latency/region, not the box.** Pick **us-east-1** (CLOB is
US/Cloudflare-fronted). **The proxy hop likely dominates** — a $5 box in us-east-1
with a nearby proxy beats a big box in the wrong region. Measure end-to-end RTT
*through the proxy*; co-locate box + proxy in US-East. If 512 MB, add a 1 GB
swapfile. Monitor: RSS, CPU-credit balance, and **end-to-end cancel latency** (the
real KPI).

---

## 5. Optimizations implemented (this pass)

All behavior-preserving except where noted; the 933-test suite stayed green.

| Change | File | Effect |
|---|---|---|
| **Concurrent per-pool prefetch** (`_prefetch_quote_reads`) in both accrue paths — config+book+mid fetched in a threadpool; decisions/orders/ledger writes stay serial on the main thread | `engine.py` | **9.5× measured** on the read phase; cycle work no longer grows linearly with the book |
| **Concurrent discovery scan** (per-pool book+history in a threadpool) | `rewards.py` | scoring ~30s → ~1.5s; scan now bounded by the (serial, cursor-chained) pagination |
| **Concurrent reeval** (held-pool re-scores in a threadpool; exits stay serial) | `runner.py` | reeval beat stops scaling with book size |
| **Deadline scheduling** (sleep only the unused remainder; warn+re-anchor on overrun) | `runner.py` | true `poll_seconds` cadence; work absorbed, not stacked |
| **Background discovery thread** (heavy scan off the hot loop; atomic report swap; Event-stopped, joined on shutdown) | `runner.py` | poll/cancel loop never blocks on the scan |
| **`PRAGMA synchronous=NORMAL`** (durable under WAL) | `db.py` | drops per-commit fsync on the per-pool hot writes |
| **`DryRunSubmitter.sent` → bounded deque (5000)** | `maker_live.py` | no memory growth over long dry-live soaks |
| **`equity_curve` rolling prune** (time-based, last `LM_RETENTION_DAYS`=30) | `db.py` | bounds the one unbounded series |
| **Event log** (`state/events/*.jsonl`, daily files, 30-day retention, thread-safe) | `events.py`, `runner.py` | granular per-poll/discovery/decision series for review |
| **Rotating runner log** (`state/runner.log`, daily, 30-day) | `runner.py` | durable human-readable log (stdout alone is ephemeral) |
| **Review + reward reconciliation tool** (`python -m pm_trader.review [days]`) | `review.py` | estimate-vs-ACTUAL reward, per-pool P&L, decisions |
| **Global request budget** (one shared `TokenBucket`, default `LM_MAX_REQ_PER_SEC`=149) | `ratelimit.py` | all CLOB reads+writes+discovery self-pace under the ~149/s order-book limit → fast polling without 429s |
| **Real-time WS market channel** (book/`price_change` → `engine.book_source`) | `ws.py` | mid pushed in ~ms → fast cancel; REST reads freed (REST fallback per-token) |
| **Real-time WS user channel** (live `trade` events → fills) | `ws.py` | real-time inventory; replaces REST `get_trades` polling |
| **WS reflex cancel** (price callback → instant `CANCEL_ALL` on a >band move) | `ws.py`, `runner.py` | ~ms stale-order pull, decoupled from accounting; next poll reposts via `force_recenter`. Submitter lock-wrapped for thread safety |
| **Order-conn keep-warm** (`ConnectionWarmer` pings `get_server_time` every 3s) | `maker_live.py` | sporadic cancels never pay a cold TLS handshake (cancel ~34ms → ~16-19ms) |
| **WS liveness gate** (engine trusts WS book only if a frame arrived <15s ago) | `ws.py`, `engine.py` | a stalled socket → REST fallback, never a stale book |
| **REST `/book` re-sync** (`LM_BOOK_RESYNC_HZ`, round-robin, allocated within 149/s) | `ws.py`, `runner.py` | authoritative anti-drift snapshot over the WS-maintained book; reserves budget for cancels |
| **Log hygiene at fast cadence** (httpx/websocket → WARNING; poll-event throttle `LM_EVENT_POLL_EVERY_S`) | `runner.py` | a 1s poll + 20/s re-sync no longer floods `runner.log` / `events/` |
| **Live exit crossing-cost booked** (drift-exit + reconcile-exit apply `maker_crossing_cost_c`) | `engine.py` | live ledger no longer optimistic on exits — consistent with the paper sim |
| **Read-conn keep-alive** (`keepalive_expiry=30s` on the REST clients) | `api.py`, `rewards.py` | reward_config / `/book` reads stay on a hot connection |
| **systemd unit** (auto-restart, graceful SIGTERM stop) | `deploy/` | survives reboot/crash; stop cancels all orders |

**Deliberately NOT done:** caching `get_reward_config` (it's a safety/exit signal —
fetch it fresh; concurrency already removed its latency cost). Capping `poll_fills`
pages (correctness > latency — dropping fills corrupts inventory).

**Cadence:** `LM_POLL_SECONDS` lowered 60 → **15** in `.env`. Faster cadence is the
profit lever and is now safe (non-blocking loop). Watch for Cloudflare `429`s; raise
back toward 30–60 if rate-limited.

## 6. Long-term stability (soak) verification

Compressed soak on the **real Engine + real SQLite**, network mocked, no money —
targeting the failure modes only a long run reveals (especially the new per-cycle
threadpools). 3-pool × 50,000 cycles and 15-pool × 20,000 cycles:

| Check | Result |
|---|---|
| Thread leak (per-cycle pools joined) | **none** — active_count flat at 1, even with 15 workers/cycle × 20k |
| FD leak | **none** — flat |
| Memory | **bounded** — RSS 46 → 50 MB, late-half drift ~1 MB |
| Ledger drift / corruption | **none** — cash/committed stable, `integrity_check: ok`, quotes intact |
| `sent` deque bound | **holds** at 5000 |
| Resilience | survived ~1,350 injected transient API errors, no crash |
| `equity_curve` growth | now **bounded** by the rolling prune (verified: 500 inserts @ cap 50 → 50 rows) |

> Note: an early run showed RSS climbing to 233 MB — traced to `MagicMock`
> recording every call (`call_args_list`), a test-harness artifact absent in
> production (real `PolymarketClient`). With plain fakes, memory is flat.

---

## 7. Operational notes

### 7.1 Reward timing — estimate vs settlement
- **Bot ledger reward = real-time continuous estimate.** Every poll credits
  `share × daily_rate × (elapsed_s / 86400)` (`orderbook.reward_accrual`) into
  `account.cash`; `get_maker_summary().reward_income` ticks up continuously.
- **Polymarket pays DAILY** (per UTC-day epoch, time-weighted). Real USDC lands in
  the wallet once/day, not continuously. The bot number leads the wallet and is an
  approximation (its `share` is a book-snapshot estimate vs Polymarket's full-day
  sampling). **Do not treat `reward_income` as settled cash.**

### 7.2 Deposit / withdraw — keep the ledger aligned
The bot ledger (`account.cash`, seeded once from `LM_CAPITAL`) and the real
Polymarket wallet are **separate, not auto-reconciled**. The bot has no
deposit/withdraw function — those happen on Polymarket. Changing the real balance
mid-run desyncs the ledger (rejected orders / meaningless P&L; deposits go unused;
withdrawing below `LM_MIN_WALLET_USDC` trips the kill-switch). Money behind resting
orders is locked collateral and can't be withdrawn until cancelled.

**Safe procedure (both directions):**
1. `touch KILL` (or SIGTERM) → bot cancels all orders + (live) flattens → frees all
   collateral, leaves the book flat.
2. Confirm on Polymarket: all cancelled, flat, USDC free.
3. Deposit / withdraw on the Polymarket wallet.
4. Re-seed the ledger: set `LM_CAPITAL` to the new free amount, then
   `rm state/paper.db` and restart → fresh `init_account(LM_CAPITAL)`, aligned.

### 7.3 Restart safety (already in code)
`_rehydrate` + `_reconcile_broker_orders` adopt surviving quotes and cancel orphaned
on-chain orders on restart. Keep `state/` on the persistent instance disk.

---

## 8. Review & iteration data (复盘)

Everything needed to review a run and iterate, on a rolling `LM_RETENTION_DAYS`
(default 30, ~1 month) window. All under `state/` (gitignored):

- **`state/events/events-YYYYMMDD.jsonl`** — append-only, one JSON object per line:
  `poll` (per-pool share/mid/reward/inventory each cycle), `discovery` (universe
  snapshot: safe count + top candidates), `place` / `exit` (decisions + reasons).
  Thread-safe (the background discovery thread and main loop both write). Analyze
  with pandas/duckdb/jq.
- **`state/paper.db`** — the ledger: `maker_quotes` (active + exited) carry the
  cumulative ESTIMATED reward, bleed, inventory P&L, fills per quote; `equity_curve`
  is the MTM series.
- **`state/runner.log`** — rotated daily, `LM_RETENTION_DAYS` kept.

**Reward reconciliation — `python -m pm_trader.review [days]`** joins three sources
per market: the ledger's ESTIMATED reward, the event-log decisions, and the ACTUAL
USDC reward PAID (public data-api `/activity?type=REWARD` for `POLYMARKET_FUNDER` —
the on-chain distributions). It prints estimate-vs-actual totals + ratio, per-pool
reward/bleed/net, and the decision/discovery summary. **This is the key 复盘 signal:**
the bot's reward is a book-snapshot estimate; Polymarket pays a time-weighted daily
settlement, so the ratio reveals how far the share estimate is off.

**Honest limits of a ~1-month window:** good for tactical review, tuning, and
catching some resolution-jump events — but a single month still may not contain
enough of the rare jump tail (one jump can wipe many days of reward) to fully judge
whether the edge survives; high confidence on tail risk needs multiple months.
Reconciliation also needs a LIVE run (dry-run has no real payouts).
