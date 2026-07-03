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

## 5b. Multi-expert review + fixes (2026-06-30)

Three independent expert reviews (C++ correctness, financial-risk, backtest-methodology) audited the code + data.

**Critical crisis/exit holes found in the C++ → FIXED:**
- A frozen/one-sided book defeated BOTH the stop-loss and the time-stop (both gated on a fresh bid) → silent
  hold-to-resolution. Fixed: a hard `MAX_HOLD_MS` force-exit that ignores book freshness (floor price if no bid).
- LIVE sell-reject was treated as flat → silent hold. Fixed: check the SELL status; on reject KEEP the position
  and retry (never strand). NOTE the settlement-lag risk: a sell 1.5 s after the buy may reject if shares aren't
  settled — must verify the buy→sell round-trip in the tiny-live test.
- Window-swap-while-holding read the wrong/stale token bid → fixed (no swap while holding; reset bids on swap).
- Shutdown orphaned an open position → fixed (flatten on exit). Entry fill basis made conservative in LIVE
  (buy_px, never overstates). Post-exit cooldown (anti-churn). MAX_USD cumulative-deployed is the real hard cap.
- DEFERRED (noted, add before live): Binance sign-confirm, persistent daily kill-switch, actual-fill reconciliation,
  the model gates (|z|>2.5 veto, maker-exit).

**Statistics corrected for honesty:**
- The per-trigger t=5.10 OVERSTATED significance (triggers within a window are autocorrelated). **Window-CLUSTERED
  (22 independent windows): t=4.06 — still strongly significant (p<0.001).**
- The cheap filter + 1.5 s hold were selected in-sample → +6.84c is optimistic. **Honest edge band: +3.9c
  (unfiltered) … +6.8c (cheap).** Both tokens (Up & Down) profit (not one-sided).
- **OUT-OF-SAMPLE: it GENERALIZES.** Held-out 2nd-half windows (never used to pick cheap/hold) = +7.28c/78%,
  matching in-sample +6.41c/79% → the cheap-scalp is NOT a cherry-picked artifact.
- fair_value σ had lookahead (whole-window vol) → fixed to CAUSAL σ([lo,t]); on 50 windows the model is UNBIASED
  (meanP_fair 0.436 ≈ actual 0.433) and modestly skillful (Brier 0.206 < base 0.245).
- Backtest fills assume displayed best ask/bid at full 5-share size (no depth/fees) — only a live order proves fill.

## 5c. LIVE TEST (2 real trades) + FEES (2026-06-30)

Two real 5-share scalps were run (user-authorized; key already on the box). Reconciled from on-chain activity:
- **Competition: PASSED** — the BUY filled at the displayed cheap ask or better (0.46→0.46; 0.24→0.22). The cheap
  ask is NOT a phantom at our latency; we get it.
- **Settlement: the SELL takes ~3.5 s, not 1.5 s** (not the feared ~25 s). The bot's sell-at-1.5s rejects, retries,
  and fills ~3.5 s. CRUCIAL: the cheap-scalp edge does NOT decay at the realizable hold — @3000 ms it's +6.95c/73%
  (≈ the 1.5 s value), because the most-lagged cheap side keeps catching up past 1.5 s. So the settlement delay is
  survivable.
- **🔴 FEES ARE REAL.** Reverse-engineered + verified on real fills: **taker fee = 0.07·p·(1-p) per share, per
  leg** (feeType crypto_fees_v2; takerOnly; rebateRate 0.2 for makers). Round-trip for a cheap entry (p≈0.25) ≈
  2.6c/share. **NET edge after fees on both taker legs: +4.45c/share, t=3.37 (per-trigger), still +EV and
  significant.** Net+clustered t ≈ 2.7. The MAKER-exit would avoid the sell-leg fee AND earn the rebate → higher net.
- Two live trades netted ≈ −$0.54 (incl. fees) — noise (one reverted). Statistical confirmation needs ~30-50 live
  trades. Position is FLAT after both (verified).

## 6. Reproduce

Recorder + offline analysis (all in `backtest/`, run on the box against `/tmp/raw_ticks.csv`):
- `raw_recorder.py` — append-mode tick recorder (OKX+Binance BTC bid/ask, PM both-sides bid/ask, window meta;
  records past the window end to capture the Chainlink-resolved outcome). `REC_SECS=28800` for an 8 h run.
- `scalp_analyze.py` — the headline: scalp by hold / trend / price, ask-persistence, statistical significance.
- `fair_value.py` — fair value + σ-horizon calibration (Brier + reliability) on real PM-resolved outcomes.
- `temporal_v2.py` — same-unit (BTC-implied P vs PM P) lag measurement (~200 ms).
- `analyze_raw.py` — hold-to-resolution control (NEGATIVE — confirms scalp, not bet).
- `replay_cpp.py` — **the faithful 1:1 replay of the live C++ execution** (one-pos-at-a-time, edge-trigger+cooldown,
  cheap entry, settlement-delayed exit ~3500ms, RESOLUTION-aware, net of taker fee, depth HAIRCUT, adverse-selection
  split). This is THE tool — use it for any honest net-EV check. Run on `/tmp/overnight.csv`.

## 7. FINAL VERDICT (2026-07-01) — real but marginal/fragile → PARK IT

After: a real LIVE test (~13 trades, lost ~$5-7), a CRITICAL bug found+fixed, an overnight data collection
(81 windows / 135 trades), a strictly-execution-aligned replay, two independent quant/financial-math expert
reviews, and every iteration tried — the honest, data-backed verdict:

**The edge is REAL and statistically significant, but too thin / fragile / capacity-capped to be worth real money.**

Evidence (replay_cpp on 81 overnight windows, real-money-aligned, net of fees):
- Overall @SETTLE=3500: **+2.8c/share net, t=3.51 (significant), 54% win, maxDD ~$1.8/5sh.** Stable across the
  growing sample (9→45→81 windows): +3.4→+2.8→+2.8c, t 1.45→3.29→3.51.
- 🔴 **Decisive adverse-selection split**: the edge is driven ENTIRELY by the CONTINUED bucket (BTC keeps moving
  during the forced ~3.5s hold: +6.0c/72% win); the **REVERTED bucket LOSES (-1.6c/30% win)**. So it is PARTLY a
  "BTC-will-continue" bet — un-filterable at a momentum trigger. +EV only because ~59% of triggers continue; a
  high-reversion regime flips it negative.
- 🔴 **Depth**: the recorder has only best bid/ask; the cheap book is thin, 5 shares walks the exit down. At a
  realistic 2-3 tick haircut the edge shrinks to +1-2c and the reverted bucket goes MORE negative.
- **Tail**: cheap = the lagging/losing side; if the ~3.5s settlement-lag stops you selling near the window end the
  position RESOLVES to 0 (one wipeout ≈ 10 good trades). nResolved>0 in the data — it happens.

Live-vs-backtest gap, explained: the live loss was MOSTLY (a) a now-FIXED bug — the marketable BUY partial-fills
(<5 shares), the bot sold the intended 5 → infinite "balance not enough" retry → window resolved → token dead →
position stranded to 0 (the user spotted "stuck/not trading"); plus (b) concentrated 2-window variance; plus (c) a
stop-loss that cut trades the no-stop replay keeps. NOT proof the edge is dead. Bug fix: place() returns actual
filled shares; sell exactly that; give up the retry on "invalid token id".

Iterations tried — none rescue it: PRICE-BAND (0.15-0.40) made it WORSE (-, t 3.3→1.6); MAKER-exit is dead
(unsettled shares reject any sell, maker or taker — verified "balance:0"); σ/fat-tail gating not worth the
fragile calibration. Frictions confirmed real: fee 0.07·p·(1-p)/share/leg; settlement ~3.5s; depth walk-down.

Both experts independently concluded "PARK IT": net ~+1-2.8c/share = ~$0.10-0.25/trade, hard-capped at 5 shares
(can't scale — walks the thin book), ~$10-25/day gross BEFORE the resolution tail, plus constant attention. The
risk-adjusted return is poor. Only revisit if a maker-exit both (a) fills reliably AND (b) survives the settlement
reject — which the data says it won't. **CONCLUSION: a genuine but un-economic edge. No real money. Locked off.**
The full evidence chain (recorder → faithful replay → adverse-selection split → expert panel → iterations) is the
reusable method; the answer for THIS strategy is park it.

## 8. LIVE RE-TEST + expert post-mortem (2026-07-01) — the backtest-vs-live gap, precisely attributed

After a user-authorized controlled live re-test (safe caps: fill-count + deployed-$ + net-loss), with a
correctness bug fixed (see below) and two independent financial-math expert reviews.

**Execution bug found + fixed live (orphan class, now closed):** a marketable GTC buy that does not immediately
cross RESTS as a live order, returns `filled=0` in the immediate response → the bot recorded no position → the
resting order filled async on-chain → an untracked ORPHAN rode to resolution (1 occurred in the first 3 orders;
recovered manually, it happened to win — luck). Root: fill detection trusted the immediate response. FIX: the
entry buy now uses **FAK (fill-and-kill)** — it takes whatever is immediately available and cancels the rest, so
it can NEVER rest; `filled` is then the definitive fill (0..N). Verified live: FAK either fills cleanly
(`status=matched`, takingAmount) or is killed cleanly (`"no orders found to match with FAK order"`) → **0 orphans
confirmed on-chain across the whole re-test.** Also added: `SNIPE_MAX_FILLS` (count ACTUAL fills, not FAK-killed
attempts, toward the N-order target), taker-fee baked into pnl, per-side book-freshness, pre-warm of tick/neg-risk
at window discovery. Commits: FAK d127ae2, fills-accounting bb922cc, pre-warm 053089d, hardening 9060d98.

**Live result (18 attempts, real money):** only **3 filled (17% fill rate)**, 8 FAK-killed ("ask gone in our
~20ms"), rest other rejects. The 3 fills: Up 0.56→0.55 (flat), Down 0.41→0.42 (+1¢), Down ~0.22→0.19 (reverted)
— **all 3 lost, ~−$0.70** (real, after correcting a bookkeeping pessimism where the bot logged entry=buy_px not
the actual FAK fill). Killed asks spanned the WHOLE range 0.24–0.52 (NOT price-selective).

**Why backtest(+2.8¢/sh) → live(negative), attributed by the expert panel:**
- 🥇 **Fill rate ≈ 17%, not the backtest's implicit 100%** (Wilson 95% CI [5.8%, 39%]). This is the robust,
  statistically-solid divergence — the backtest overstated opportunity 3–17×. Needs no P&L significance to stand.
- 🥇 **Winner's-curse / information adverse selection (the connecting mechanism):** we only fill when the ask
  SURVIVED ~20ms = when no faster informed sniper wanted it. The +6¢ CONTINUED bucket (BTC keeps moving, PM
  catches up) is exactly what the fast (likely London-co-located) players race for → they take it, we're left with
  the no-continuation / REVERTED bucket (−1.6¢). So our fills are NOT an unbiased sample of the backtest's signals
  — they're systematically biased to the losing bucket. This PREDICTS the failure. (Caveat, per the quant expert:
  the mechanism is theoretically necessary + directionally consistent, but NOT statistically PROVEN at n=3 fills —
  it doesn't need to be; the two facts above suffice.)
- 🥉 **Fee floor > catch-up:** round-trip taker at p≈0.4–0.5 = 3.4–3.5¢/sh; observed catch-ups ±1¢. Profit needs
  a >3.4¢ catch-up; the winner's-curse subset delivers ≈0.

**Honest statistical caveat (quant expert corrected an over-read):** the "3 fills all lost / −$0.70" is NOT
statistically distinguishable from the backtest +2.8¢/sh (z≈−1.4, p≈0.08–0.11; 3 losses ≈ 0.46³ ≈ 10% under the
backtest's own 46% loss rate). Do NOT claim "the per-fill edge collapsed" or extrapolate "−$2.8/10 fills" as a
forecast. The gap is explained by fill-rate + fee arithmetic, NOT by a demonstrated edge collapse. Distinguishing
live per-fill edge from the backtest would need ~25–40 fills (~5h, ~150 attempts) — not practically reachable.

**Solutions evaluated — all hit hard walls:** London co-lo (the only real fix) = geo-blocked, needs KYC/KYB
exemption (infra+compliance, parked); maker orders = rest→orphan/miss on entry + settlement "balance:0" reject on
exit (dead); cheap-only = revert risk + thinner book → more kills; bigger size = walks the thin book; software
latency = pre-warm/warm-connection/event-driven already done, the ~10ms Ireland→London leg is physical.

**FINAL (reconfirmed): structurally uncapturable at our latency/location → PARK.** The backtest edge is
paper-real but assumes a fill quality we cannot achieve from eu-west-1 against London-co-located competition.
**Reusable lesson:** any taker-sniping strategy whose backtest assumes ~100% fill MUST be re-evaluated assuming
you only fill the *winner's-curse subset* (the signals faster informed players declined) — otherwise the backtest
systematically lies. Real money locked off again (PM_TRADER_LIVE=0).
