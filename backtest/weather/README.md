# Polymarket daily-temperature pipeline (read-only research)

Polymarket lists **49 cities x 11 negRisk buckets/day** — "Highest temperature in
&lt;CITY&gt; on &lt;DATE&gt;?", event slug `highest-temperature-in-<city>-on-<month>-<day>-<year>`,
endDate `<date>T12:00:00Z`. This directory holds the full free-data pipeline: cohort
discovery → resolution-grade outcomes → open-meteo forecasts → per-station bias
correction → bucket probabilities → comparison/backtest vs PM mids.

**Zero orders, zero keys, stdlib-only Python.** Everything caches under `cache/`
(gitignored).

## VERDICT — CLOSED 2026-07-05 (strategy line decommissioned)

Final chain of evidence: (1) model line — PM beats the free model (below); (2) maker line — live
trial 2026-07-04 lost $11.24 (all fills in the Shanghai ramp, -14c/sh settle-P&L); (3) 3-day
resolved-cohort tape re-measurement (jul-1/2/3, 260k prints, 11.7M shares): pooled maker capture
+0.07/-0.06/-0.46 c/sh ~= ZERO, no local-hour cell stable across days; (4) crypto-tail transplant
(sell 2-7c temp tails): -0.02/-1.36 c/sh — temp is FORECASTABLE, tails are priced by forecasts;
(5) every "positive pocket" (buy-side 15-40c etc.) flipped sign across days = noise. Rewards for
the trial day: $0.49 vs -$11.24 trading. Tooling: maker_hour_slice.py (--cohort). Ops units
removed 2026-07-05 (box archive /root/temp-trial-archive.tar.gz). REVIVAL = real forecasting
skill (MOS path below) only.

## Original model verdict (2026-07, ~unchanged on the 2026-07-03 rebuild)

**The market beats the free model.** Original full run: PM T−1 mids Brier **0.0616** vs
free-model **0.0674** (paired t ≈ 2.6); PM won in *every* subset (US/intl, hot/cold,
calm/volatile), and blending the model into PM only degraded PM. Rebuild spot-check
(16 city-days, 2026-07-01/02): PM 0.0532 vs model 0.0629, blend 0.0559, t ≈ 1.5 — same
direction. **Do not trade this model against the market.**

What would change the verdict (the reason this plumbing is kept): *real forecasting
skill* — station-level MOS trained on months of METAR history, full ensembles, and
same-day nowcasting off incoming METAR — would reuse every piece here (cohort fetch,
station parsing, METAR ground truth, bucket semantics, scoring harness) with only
`forecast.py`/`model.py` swapped for the skilled distribution.

## Hard-won domain gotchas (encoded in the code — do not relearn these)

1. **Resolution is station-specific and often NOT the obvious airport.** 45 cities →
   Wunderground daily history at a named ICAO (NYC=**KLGA**, Denver=**KBKF** Buckley SFB,
   Paris=**LFPB** Le Bourget, London=**EGLC** City Airport, Panama City=**MPMG**);
   3 cities → NOAA `weather.gov/wrh/timeseries` (**LTFM** Istanbul, **UUWW**
   Moscow-Vnukovo, **LLBG** Tel Aviv); Hong Kong → **HK Observatory HQ** "Absolute Daily
   Max". (Counts re-measured 2026-07-03; the original run recorded 44/4.) Always parse
   the station **per market** from the gamma `description` field
   (`common.parse_resolution_source`) — never hardcode a guess. Watch percent-encoded
   Wunderground paths (Ankara: `/tr/%C3%A7ubuk/LTAC`).
2. **Ground truth = max over raw METAR rows from IEM** (`metar_max.py`). The IEM *daily
   summary* (DSM) endpoint reads **1–2 °F HIGH** vs what PM/Wunderground resolve at US
   stations — never use it. Day boundaries are the **station's local timezone**.
3. **Bucket semantics.** US: 2-°F buckets ("98-99°F"), tails "or below"/"or higher",
   source shows whole °F → bucket \[lo,hi\] = interval \[lo−0.5, hi+0.5). International:
   1-°C buckets, whole °C → same round semantics. **Hong Kong: HKO publishes 0.1 °C →
   FLOOR semantics**, bucket \[lo,hi\] = \[lo, hi+1).
4. **HK proxy caveat:** HKO HQ is not a METAR station; we use VHHH METAR as the outcome
   proxy and it *disagrees with the official resolution* (e.g. 2026-07-01: VHHH max
   34.0 °C, HKO resolved the 33 °C bucket). `backtest_mini.py` flags `[METAR!=PM]` and
   scores with PM's actual resolution.
5. **Forecasts (open-meteo, keyless):** ensemble API (icon_seamless + gfs_seamless +
   ecmwf_ifs025 ≈ **122 members**) for live distributions; historical-forecast API
   `temperature_2m_previous_day1..7` for archived deterministic previous-runs.
   **Ensemble previous-runs are NOT archived** (all-null) — deterministic previous-runs
   are the only honest backtest forecast. `api.open-meteo.com` itself was unreachable
   from some networks; the `ensemble-api.` / `historical-forecast-api.` hosts work.
6. **Per-station grid bias correction is mandatory** (`bias_calib.py`): the NWP grid ran
   cold vs station at 34/48 stations (mean +0.42 °C, up to ±1.9 °C at Chengdu, Guangzhou,
   Buenos Aires, Incheon, Munich; −1.8 °C at Lucknow); residual sd ≈ 0.95 °C after a
   trailing-METAR correction. Rebuild examples (10 days to 2026-07-02, fc−obs):
   KLGA +1.34, EGLC +0.10, LTFM −0.22, VHHH −2.12.
7. **gamma discovery:** filter tag_id **104596** ("highest-temperature") + an
   **end-date window** on the target day (offset caps make raw paging unreliable).
   As of 2026-07-03 markets exist up to ~2 days ahead (creation timestamps seen at
   01:02 and 04:03 UTC), not strictly T−1 01:03.

## Scripts (run from this directory; `python3 x.py --help` for full flags)

| script | does | example |
|---|---|---|
| `fetch_cohort.py` | gamma cohort → `cache/cohort_<date>.json` (stations, buckets, tokens, optional CLOB books) | `python3 fetch_cohort.py 2026-07-03 --cities nyc,london --books` |
| `metar_max.py` | resolution-grade daily max from raw IEM METAR (never DSM) | `python3 metar_max.py KLGA 2026-07-02` |
| `forecast.py` | open-meteo ensemble (live) / previous-run (backtest) daily-max distribution | `python3 forecast.py KLGA 2026-07-01 --mode previous --lead 1` |
| `bias_calib.py` | trailing-N-day per-station bias → `cache/station_bias.json` | `python3 bias_calib.py KLGA --end 2026-07-02 --days 10` |
| `model.py` | bias-corrected bucket probabilities → `cache/model_<date>.json` | `python3 model.py 2026-07-03 --mode ensemble` |
| `compare.py` | model vs live PM mids, discrepancy table + mid-sum sanity | `python3 compare.py 2026-07-03 --cities nyc` |
| `backtest_mini.py` | **decisive metric**: PM T−1 mid vs model previous_day1 vs outcomes (Brier/log-loss, paired t) | `python3 backtest_mini.py 2026-07-01 2026-07-02 --cities nyc,london,paris` |

Typical live-day flow: `fetch_cohort.py D` → `bias_calib.py <ICAO> --end D-1` per station →
`model.py D --mode ensemble` → `compare.py D`. Backtest flow: just `backtest_mini.py D`
(it fetches its own cohort, trains bias ending D−1 — no target-day leakage — and pulls
PM mids from CLOB `/prices-history` at the `--snap-utc` snapshot, default 00:00 UTC).
