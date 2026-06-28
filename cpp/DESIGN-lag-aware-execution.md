# Design: lag-aware execution accounting (next session)

Status: **proposed, not implemented.** This is the last missing piece to take the live two-sided
maker from "bounded orphans, net-negative" to "zero orphans, stable net-positive." Written
2026-06-28 after the live experiment (see memory `execution-settlement-lag-root`).

## 0. Problem (recap)
PM's chain / data-api reflects a trade with a **~25 s settlement lag**. The instantaneous chain
query is therefore wrong for ~25 s after every fill. This single fact broke three subsystems,
each of which we patched separately with band-aids:

| Subsystem | Lag symptom | Current band-aid |
|---|---|---|
| Flatten (`engine::flatten_live`, `clob_submitter::flatten`) | sell just-bought shares → REJECTED "not enough balance" | retry loop, escalating sweep |
| Equity-drawdown kill (`runner::kill_check` ~L950) | `chain_position_value` undercounts just-bought positions → false kill | require 3 consecutive 15s drawdown samples (~45s) |
| Fill accounting (`engine::accrue_maker_rewards_live`, `clob_submitter::extract_new_fills`) | poll_fills vs chain desync → phantom inventory | per-leg column, status-gate, prime cursor, inv_pnl rebase |

Band-aids work but each is fragile. The **root fix is one shared component**: an in-flight ledger
that knows what the bot has done before the chain confirms it.

## 1. Goal
A single **lag-aware position estimate** that is correct *during* the lag window, used everywhere
a position value is needed. Replaces the band-aids with one source of truth.

```
believed_position(token) = chain_position(token)            // settled truth (lagging)
                         + in_flight_buys(token)            // submitted+acked, not yet on chain
                         - in_flight_sells(token)           // submitted+acked, not yet on chain
```

## 2. Core component: `InFlightLedger`
New class (suggest `cpp/include/pmm/inflight.hpp` + `cpp/src/pmm/inflight.cpp`).

State: a list of in-flight entries, one per acked order fill the bot caused:
```
struct InFlightFill {
    std::string token;
    double signed_size;     // +buy / -sell (in shares)
    double price;           // fill price (for $ cost)
    double acked_mono;      // monotonic time the ack/poll_fills saw it
};
```

API:
- `void on_fill(token, signed_size, price)` — called wherever the bot learns of its own fill
  (order ack in `clob_submitter`, and `extract_new_fills`). Append an entry.
- `void reconcile(const std::map<std::string,double>& chain_positions)` — called each 15s when the
  runner refreshes the chain. For each token, drop in-flight entries whose cumulative effect the
  chain now reflects (match by token + cumulative signed size, oldest first), and **age out** any
  entry older than `~40 s` (assume settled-or-dead, avoid leaks).
- `double net_size(token)` and `double net_cost(token)` — the in-flight delta + its $ cost.
- `std::map<std::string,double> believed_positions(chain)` — chain + in-flight, the lag-aware view.

Reconciliation detail (the subtle part): the chain catches up *cumulatively*, so when
`chain_position(token)` increases by X, retire the oldest in-flight buys totalling X. Keep a small
tolerance (≥1 share) for rounding. Age-out is the safety valve for fills that failed or that the
chain reflects in a way we can't match.

## 3. Integration points (replace the band-aids)
1. **Equity kill** (`runner::kill_check`): equity = USDC + Σ `believed_position(t)`×price. Just-bought
   positions are now counted via in-flight cost → **no false fire** → delete the 3-consecutive
   band-aid (`equity_dd_count_`). Cleaner and faster (no 45 s delay).
2. **Chain guard** (`engine::place_maker_quote_live` ~L444): block on `believed_position` instead of
   raw `chain_positions_` → blocks re-quote *immediately* on fill (no 15 s blind window).
3. **Flatten** (`engine::flatten_live`): flatten the *believed* position, and if the shares are
   in-flight (not settled), **wait** for the entry's settle time rather than retry-spamming a sell
   that will be rejected. Removes the "not enough balance" thrash.
4. **Accounting** (`accrue_maker_rewards_live`): use in-flight as the during-lag inventory truth;
   reconcile to chain as before. Reduces phantom-inventory reliance on the fragile ledger.

Wiring: the `InFlightLedger` lives in the runner (it already refreshes chain every 15 s in
`kill_check`), and is injected into the engine like `set_chain_positions` already is. The
submitter reports fills into it (it already parses acks + `extract_new_fills`).

## 4. Secondary fixes (cheap, do alongside)
- **Mid boundary too loose at 0.2.** Run C bought Israel @0.18 because selection-mid was ~0.21 (passed
  the 0.2 gate) but the BUY leg fills *below* mid. Fix: judge by the **actual order price**, not the
  selection mid — in `place_maker_quote_live`, after computing the two bid prices, skip a leg whose
  price < 0.25 or > 0.75 (config `LM_EXTREME_MID_MARGIN` already exists; add a price-level check on
  the computed quote, or just raise the margin to 0.25).
- **Size by $ not shares.** A high-price NO leg (e.g. 0.92) commits far more $ per share than the
  `size×(1-2s)` cap assumes (South Korea NO committed $170 vs the $100 `max_pool_frac` cap). Cap the
  **actual order notional** (size×price) at `max_pool_frac × capital` per leg.

## 5. Test plan
- Unit: `InFlightLedger` reconcile (chain catches up partially / fully / out-of-order; age-out;
  buy+sell netting). New `cpp/tests/inflight_smoke.cpp`.
- Parity: equity-kill with in-flight must not false-fire on a simulated just-bought position;
  must still fire on a real (settled) drawdown.
- Dry-live soak on the box (`LM_DRY_LIVE`, no real orders) to confirm no regressions, then a small
  real run watching for: zero orphans (chain == believed after each fill settles), no false kills.

## 6. Sequencing
1. `InFlightLedger` + unit tests (no behaviour change yet).
2. Wire into equity kill (delete `equity_dd_count_`), verify no false fire.
3. Wire into chain guard + flatten.
4. Secondary fixes (mid price-level, $ sizing).
5. Small real run, measure: orphans, false kills, net P&L vs the ~$12/day reward.

Success = a multi-hour real run with **0 orphaned positions** and **0 false kills**, net P&L
≥ reward minus minimal spread. That is the "stable profit" bar the experiment couldn't clear
without this layer.
