# Deployment record

Production host for the live maker. Validated 2026-06-25.

> ⚠️ **This file contains host access credentials. Keep the GitHub repo PRIVATE.**
> The Polymarket private key is NOT here (only in the host's gitignored `.env`).
> Rotate the SSH password if this repo is ever exposed.

## Host

| | |
|---|---|
| Provider / region | AWS EC2, **eu-west-1 (Ireland / Dublin)** |
| Instance type | **t3.medium** — 2 vCPU (burstable), 3.7 GiB RAM |
| CPU | Intel Xeon Platinum 8259CL @ 2.50 GHz (turbo ~3.1) |
| Disk | 80 GB (≈2 GB used) |
| OS | Amazon Linux 2023 (kernel 6.1) |
| Python | 3.11.15 (project requires ≥3.10; AL2023 default 3.9 not used) |
| Order ID | #1790240704463104 |
| Expiry | 2026-07-25 16:13 |
| Panel | https://aws.121788.xyz/lb/etfowz7n6o9wx328 |

### SSH access
| | |
|---|---|
| Host | `3.255.213.234` (port 22) |
| User | `root` |
| Password | `66313527a` (changed from the provider default 2026-06-25) |

Burstable is fine: the bot is I/O-bound and near-idle (~67 MB RSS), so CPU credits
accrue rather than deplete. RAM/disk are hugely over-provisioned for it.

## Polymarket connectivity — DIRECT, no proxy

Ireland is **not** geo-blocked, so the host reaches Polymarket directly. **No
`ALL_PROXY` / `HTTPS_PROXY` / `LM_WS_PROXY` is set or needed** (and `socksio` /
`python-socks` are unnecessary on this host).

Measured from the host (2026-06-25):

| Path | Result |
|---|---|
| ICMP RTT to clob | **min 1.23 / avg 1.32 / max 1.51 ms** |
| CLOB `/time` | HTTP 200 · TLS 15 ms · TTFB **34 ms** |
| CLOB `/sampling-markets` | HTTP 200 · TTFB 30 ms · 2.4 MB |
| WS host TLS connect | 16 ms |

Colocation-grade — ~15× faster than the prior proxied path (~500 ms/GET). The
cancel-latency profit lever is genuinely viable here.

## Layout

- Code: `/root/polymarket-live-maker` (full repo incl. `.git`)
- venv: `/root/polymarket-live-maker/.venv` (`pip install -e ".[live,dev]"`)
- Config: `/root/polymarket-live-maker/.env` (real creds; `PM_TRADER_LIVE=0`,
  `LM_WS=1`, `LM_POLL_SECONDS=1.0`, `LM_MAX_REQ_PER_SEC=149`, signature_type=1)
- Run: `cd /root/polymarket-live-maker && .venv/bin/python -m pm_trader.runner`
  (or the `live-maker` entrypoint)

## Validation on host (2026-06-25)

- **983 tests pass** (`pytest -q -m "not live"`) on Python 3.11.
- Dry-run smoke (`PM_TRADER_LIVE=0`, direct, no proxy): WS market channel up,
  discovery 10 safe/60 in ~2 s, 3-pool book placed, DRY orders logged, **WS reflex
  CANCEL fired on real mid moves**, real-time event log written, **0 errors / 0
  reconnects**, graceful shutdown.

## Go-live checklist (when the operator decides)

1. Fund the Polymarket proxy wallet (funder `0x78dE…`) with USDC.
2. Confirm `.env` capital/risk knobs (`LM_CAPITAL`, `LM_MAX_POOLS`,
   `LM_MAX_LOSS_PER_DAY`, optional `LM_MIN_WALLET_USDC`).
3. Flip `PM_TRADER_LIVE=1` in `.env`.
4. Run under a process manager (e.g. systemd) for auto-restart; `state/` holds the
   ledger + event log + rotating `runner.log` and must persist across restarts.
5. Stop / withdraw: `touch /root/polymarket-live-maker/KILL` → cancels all +
   flattens; then withdraw on Polymarket (see PERFORMANCE-AND-OPS §7.2).
