# polymarket-live-maker

Autonomous **live liquidity-rewards market maker** for Polymarket, implemented in
**C++20** (`cpp/`). Self-contained native binary — custom CLOB order signing
(secp256k1 / EIP-712), persistent HTTPS + WebSocket, SQLite ledger, polling-driven
autonomous quoting loop. Dry-live by default; places real orders only behind an
explicit operator switch (`PM_TRADER_LIVE=1`). No LLM in the loop.

> Strategy + economics: [`docs/research/04-lp-rewards-edge.md`](docs/research/04-lp-rewards-edge.md).
> Polymarket pays a daily USDC pool to two-sided limit orders resting within
> `max_spread` of mid. The bot discovers safe, low-jump mid-tail reward pools,
> quotes a small decorrelated book, sizes by quality (capped for diversification),
> manages inventory with skewed quotes, and exits + benches a pool on a catalyst
> jump. Read it before risking money.

## Build

Requires a C++20 toolchain (**GCC 14+** for `<format>`), **cmake ≥ 3.27**, **ninja**,
and `-devel` packages: openssl, sqlite, libcurl, zlib. secp256k1 is fetched via
CMake FetchContent (needs git + network).

```bash
cd cpp
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
```

Produces `cpp/build/live-maker` plus the test binaries (`pmm_*`). Run a test binary
directly to check it (they are not wired to ctest).

## Run

Reads `.env` (see `.env.example`) and writes `state/` (ledger + event log) in the
working directory; single-instance locked per `state/`.

```bash
cd <working dir with .env>
/path/to/cpp/build/live-maker
```

- **Default: dry-live** — exercises the live code path, logs the orders it *would*
  place, sends nothing. P&L = gross reward only.
- **`PM_TRADER_LIVE=1` → real orders (real money).** Start tiny (`LM_CAPITAL=20`);
  capital is the exposure throttle.
- **Kill switch:** `touch KILL` in the working dir → cancels all + flattens + stops.
- Config knobs: see `.env.example`.

Deployment: `docs/DEPLOYMENT.md`. systemd unit: `deploy/polymarket-live-maker-cpp.service`.

## License

See `LICENSE`.
