# Runbook — activate the research-driven maker stack

All fixes are built + committed (≤ `6b7710e`): fade-on-imbalance, semantic pool-selection
(whitelist override), autonomous LLM curation, CTF merge (dry-verified, OFF), telemetry.
Two things gate a real run — both are **yours** to do (an API key + real-money authorization).

## Step 1 — turn on autonomous pool-selection (unblocks `placed=0`), verify in DRY-LIVE (no money)
On the box, add YOUR Anthropic key to the dry unit (handle it like the PM key — never commit it):
```
mkdir -p /etc/systemd/system/pmm-live-maker-dry.service.d
printf '[Service]\nEnvironment=LM_ANTHROPIC_KEY=%s\nEnvironment=LM_CAPITAL=500\n' "sk-ant-YOURKEY" \
  > /etc/systemd/system/pmm-live-maker-dry.service.d/activate.conf
systemctl daemon-reload && systemctl restart pmm-live-maker-dry
```
Verify (expect: ENABLED, approved N>0, placed>0, and fade firing on imbalance):
```
journalctl -u pmm-live-maker-dry -o cat | grep -E "curation:|reselect:|fade_imbalance" | tail
```
✅ pass = `curation: LLM approved N of M`, `reselect: ... placed>0`, and over time `fade_imbalance`
events when a book imbalances. This proves the mechanism with zero real money.

## Step 2 — small LIVE run (real money — only after Step 1 is clean, and only you decide the size)
A separate unit with `PM_TRADER_LIVE=1` + a SMALL `LM_CAPITAL` (e.g. 200–300) + the key. Funder
0x78dE must hold USDC + the PM key set via `set_pmkey.sh`. Safety nets are on by default:
equity-drawdown kill (`LM_MAX_LOSS_PER_DAY`), churn breaker, chain-flat guard, fade, net-gate.
Start tiny; watch the first fills.

## Step 3 — measure (the goal: stable positive P&L)
Dashboard: `ssh -L 8787:localhost:8787 root@<box>` → open `cpp/telemetry/dashboard/index.html`.
Deep analysis from `state/telemetry.db`:
```
-- per-fill adverse selection (did fade help?)
SELECT ts_ms, question, side, bleed, mid_before, mid_after FROM fillcontext ORDER BY ts_ms DESC;
-- what the LLM approved vs what filled
SELECT * FROM logevent WHERE kind='curation' ORDER BY ts_ms DESC LIMIT 5;
-- equity trajectory
SELECT ts_ms, usdc, equity, day_pnl, realized_bleed, reward_accrued FROM equitysnapshot ORDER BY ts_ms;
```

## Honest expectation
Even fully fixed, $1k yields ~$/day (thin, execution-intensive; pros compete the edge down). The
test is whether the active stack (fade + semantic-selection + merge) flips the sign vs the old
static config's ~-$190 — not big profit at this scale.

---
## STATUS (2026-06-28): everything doable is DONE; the rest genuinely needs the live test

### ✅ VERIFIED in dry-live (no money, on the box)
- **Semantic selection works**: the numeric filters select TOXIC pools (verified: they picked the
  Tyler-Robinson trial + the McGonigle award — the exact pools we lost on). The whitelist OVERRIDE
  steered the bot to quote the LLM-curated broad-based pool (Dodgers) instead. So the bot quotes
  what *I* semantically approve, avoiding the toxic news pools.
- **fade-on-imbalance FIRES**: 7,962 `reflex_cancel reason=fade_imbalance` on real imbalances
  (obi_band~0.96, micro_lead~2.4¢); 9,128 signal events; books are fully liquid (347K L2 snapshots,
  both sides). The research's #1 fix is live-verified working.
- **placed=0 root cause**: a drained dry ledger ($45), NOT the filters. Fixed (low_reward bypass for
  whitelisted + fresh capital). The earlier scares (empty books / WS-bug) were MY measurement errors
  (the signal/fade events go to telemetry, not stderr — I was grepping the journal).
- **Full observability live**: dashboard reachable via `ssh -L 8787:localhost:8787` (or the tunnel
  helper); telemetry recording to state/telemetry.db.

### 🔒 LEFT for the SEPARATE live test (real money / on-chain — physically un-doable in dry-live)
1. **Live P&L** — does the active stack (semantic-selection + fade) flip the sign vs the static
   config's ~-$190? Needs `PM_TRADER_LIVE=1` + small `LM_CAPITAL`. Only measurable on REAL fills.
2. **Fade threshold tuning** (`LM_FADE_OBI` / `LM_FADE_MICRO_C`) — fade fires often on liquid/active
   pools; calibrate to "fire only on genuinely toxic flow" using live fade-vs-fill P&L data.
3. **Merge real-funds arming** — inner target/collateral must be pinned for THIS account (CTF+USDC.e
   as encoded vs the newer Ctf-Collateral-Adapter `0xada100db…`+pUSD `0xc011a7…` seen on-chain)
   before `LM_MERGE_ARM_REAL_FUNDS=1`. Proxy routing (sig_type=1) is solved + dry-verified.
4. **Autonomous LLM curation** — set `LM_ANTHROPIC_KEY` to enable (the static whitelist works without it).
