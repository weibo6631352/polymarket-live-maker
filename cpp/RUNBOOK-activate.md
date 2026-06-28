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

## Still TODO (optional, not on the critical path to run)
- CTF merge sig_type=1 proxy-factory `exec` wrapper (positions held by proxy 0x78dE, not the EOA) —
  the only piece left for a REAL merge; merge is double-gated OFF until then.
- Tune `LM_FADE_MICRO_C` / `LM_FADE_OBI` from live fade-vs-fill telemetry.
