# PM 5-min BTC Up/Down — Lag-Scalp Strategy (statistics, model, strategy)

Status: **offline edge statistically solid; live fill/competition test pending (needs a user-authorized tiny real order).**
Date: 2026-06-30. Data: 51 resolved 5-min windows recorded on the box (`/tmp/raw_ticks.csv`, ~282 MB).

---

## 0. The market

Polymarket "Bitcoin Up or Down — HH:MM-HH:MM ET": a 5-minute window. **"Up" resolves true iff the BTC price at the
END of the window is ≥ the price at the OPEN.** Resolution source = **Chainlink BTC/USD data stream** (NOT Binance/
spot — verified from the market description; correcting this was a turning point, all earlier Binance-resolved
numbers were artifacts). PM price 0-1 = market's probability; pay X, get $1 if right, $0 if wrong.

## 1. The mechanism (what we actually trade)

We do **NOT** bet on the window outcome (hold-to-resolution is NEGATIVE — the underdog bounce reverts). We arbitrage
the **transient lag**: when BTC moves, the fair probability jumps **instantly**, but the PM quote lags ~200 ms and
then catches up over ~1-1.5 s. We buy the favored side at the stale PM ask and sell into the catch-up.
The unifying variable is the **gap g = P_fair − P_mid**. "Cheap entry" (ask < 0.55) is just *where PM has lagged the
most* = largest g = the best scalp. You exit in ~1.5 s, BEFORE the bounce can revert.

## 2. Statistical summary (51 windows)

Headline edge — **cheap-scalp** (BTC move → buy favored-side ask, hold 1.5 s, sell the bid; spread already netted):

| metric | value |
|---|---|
| n (cheap triggers) | 57 |
| mean | **+6.84¢ / share (gross)** |
| SE | 1.34¢ |
| **t-stat** | **5.10  →  p ≪ 0.01, highly significant** |
| win rate | 79%, Wilson 95% CI **[67%, 88%]** (excludes 50%) |
| per 5-share trade | +$0.342 gross (≈28% of the ~0.24 stake) |

**Stability across the growing sample (anti-overfit check):**

| windows | 10 | 16 | 22 | 30 | 41 | 51 |
|---|---|---|---|---|---|---|
| cheap-scalp | +4.9¢/81% | +6.4/84% | +6.4/79% | +6.6/81% | +6.5/82% | +6.8/79% |

Stable across a 5× sample increase → a real signal, not noise (noise decays with n).

**Competition proxy (ask persistence after the trigger, cheap entries):** the favored-side ask is unchanged at
+0 ms and +17 ms (e.g. 0.2125 → 0.2125), only starts rising ~50-100 ms later (= the PM catch-up we scalp). So in
this data the cheap ask **survives our 17 ms latency** and the gap is still open when we arrive.

**Calibration (fair_value.py):** meanP_fair ≈ actual up-rate (unbiased, e.g. 0.496 vs 0.492). Brier ≈ base rate
(the model does NOT predict the winner — EXPECTED, the resolution is genuinely ~random, so P_fair ≈ PM ≈ true prob).
This does not hurt the scalp, which captures the transient post-move lag, not the resolution.

## 3. Mathematical model

**Fair value** (P that Up resolves true, given BTC now):

```
P_fair(Up) = F_ν( z ),    z = ( ln(S/K) − ½σ²τ ) / sqrt( σ²τ + σ_b² )
```
- S = BTC now, K = window open, τ = seconds left, σ = vol per √second.
- **F_ν = Student-t CDF**, ν = ν(τ) = 4 + 6·(τ/Δ)/(κ−3), clamped [3,100] — BTC has fat tails; they bite most at
  small τ (few increments left), → Gaussian at large τ. (Gaussian over-prices the favored/expensive side.)
- **σ via HAR multi-scale** (blend 1-min/5-min/30-min realized vol; sample at ≥1 s to avoid bid-ask bounce) +
  **floor σ / clamp |z|** — σ is the sensitive knob; too-small σ manufactures fake gaps (a past failure).
  NOTE: in our data the 1-second realized vol underestimates the multi-minute dispersion (BTC is super-diffusive),
  so a coarser/HAR σ is required — single-1s σ is biased.
- **σ_b = Chainlink-basis stdev** (we observe Binance, settle on Chainlink) — widens the dist, pulls toward 0.5,
  forbids endgame trades.

**Lag dynamics** (how PM tracks fair value):

```
dPM = k · (P_fair − PM) dt + noise        k ≈ 1-2 / s   (half-life 0.4-0.7 s)
```
First-order exponential lag ⇒ the gap g is **AR(1)**, decaying at rate k. This explains the scalp curve (negative at
200 ms, positive at ~1.5 s): a pure 200 ms delay would fully catch up at 200 ms; the exponential lag is gradual.
Estimate k from AR(1) of the gap series, or fit `mean_scalp(h) = G₀(1−e^{−kh}) − δ/2 − ρh`.

**Edge:** on a move, P_fair jumps; PM lags; we capture `(1−e^{−k·h})·g` over hold h, net of spread + costs.

## 4. Strategy

**Entry:** OKX bbo-tbt trigger (`|ln(S_t/S_{t−3s})| > θ`, θ≈3e-4) + Binance sign-confirm. Buy the favored side at a
**marketable** ask (`min(ask+2 ticks, 0.97)`), especially when **cheap** (ask<0.55 = max g). Gate on the MODEL gap:
`(1−e^{−k h})·g > spread + slippage + fees + Z·SE_fair` (Z≈1.5-2); `0.15<P_mid<0.60`; `60s<τ<240s`; ask fresh
(<300 ms). **Hard veto |z| > 2.5** (σ-robustness — large |z| is the fake-gap zone where ∂P/∂σ is largest).

**Exit:** hold ~1-1.5 s; exit on **gap-closure** (`P_mid ≥ P_fair − g_out`). **Maker-preferred** sell (post a limit at
`P_fair − 1 tick`); taker fallback (sell the bid) after ~800 ms. Hard time-stop 2.5 s. **NEVER hold to resolution.**
(Maker-exit is essential: taker-in + taker-out pays the full spread twice.)

**Risk:** stop-loss `P_bid ≤ entry_ask − 0.03`; one position at a time; **fixed 5 shares until ≥150 live trades**,
then ¼-Kelly at most (Kelly on 57 samples prints ~17× = ruin); **hard caps on ORDER COUNT + deployed $** incremented
*before* each placement (not on realized P&L — the prior $94 over-run lesson); daily kill-switch (N consecutive
losers / −$X = regime change or competition arrival).

## 5. The make-or-break (why this is NOT yet "certain profit")

- ✅ Offline **GROSS** edge is statistically solid (t=5.10) and stable.
- 🟡 It is GROSS (clean bid exit). NET needs: real exit slippage + the maker-exit fill rate + competition.
- 🔴 **Decisive unknown the backtest CANNOT settle:** would our live order actually FILL at the cheap ask, or is the
  ask a **phantom** (already lifted by a faster, possibly co-located sniper whose fill hasn't reached our feed)?
  The recorded ask-persistence is encouraging but cannot model our own live order. **Only ONE tiny real order
  (5 shares, a few dollars) settles it** — user-authorized only, never self-armed, only after the bigger-sample
  (≥100 window) offline confirmation. Decisive shadow/live measurement: log the ask actually available at our 17 ms
  vs the backtest +50 ms ask and the gap remaining; if the gap survives at 17 ms → real; if gone → mirage (co-lo).

## 6. Reproduce

Recorder + offline analysis (all in `backtest/`, run on the box against `/tmp/raw_ticks.csv`):
- `raw_recorder.py` — append-mode tick recorder (OKX+Binance BTC bid/ask, PM both-sides bid/ask, window meta;
  records past the window end to capture the Chainlink-resolved outcome). `REC_SECS=28800` for an 8 h run.
- `scalp_analyze.py` — the headline: scalp by hold / trend / price, ask-persistence, statistical significance.
- `fair_value.py` — fair value + σ-horizon calibration (Brier + reliability) on real PM-resolved outcomes.
- `temporal_v2.py` — same-unit (BTC-implied P vs PM P) lag measurement (~200 ms).
- `analyze_raw.py` — hold-to-resolution control (NEGATIVE — confirms scalp, not bet).
