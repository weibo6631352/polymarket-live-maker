#!/usr/bin/env bash
# run_bridge.sh — telemetry consumer pipeline for the pmm dashboard.
#
# telemetry_tap (Fast-DDS subscriber -> stdout JSON lines) is fanned out to BOTH:
#   - recorder.py  -> SQLite (state/telemetry.db), one table per topic (SQL analysis)
#   - ws_server.py -> WebSocket 0.0.0.0:8787 /telemetry (the live dashboard)
# `tee` + bash process substitution duplicates the single tap stream to both sinks
# without either blocking the other (ws_server reads stdin on its own thread, drops
# for slow clients; recorder batches commits). Requires bash (process substitution).
#
# Decoupled from the bot over DDS domain 0 — the bot (publisher) and this bridge
# (subscriber) can each restart independently; systemd Restart=always recovers crashes.
#
# Env overrides (all optional): PMM_REPO, PMM_DDS, PMM_TELEMETRY_DB, PMM_TAP.
set -uo pipefail

REPO="${PMM_REPO:-$HOME/polymarket-live-maker}"
DDS="${PMM_DDS:-$HOME/dds}"
DB="${PMM_TELEMETRY_DB:-$REPO/state/telemetry.db}"
TAP="${PMM_TAP:-$REPO/cpp/build-tel/telemetry_tap}"
BRIDGE="$REPO/cpp/telemetry/bridge"

export LD_LIBRARY_PATH="$DDS/lib:$DDS/lib64:${LD_LIBRARY_PATH:-}"
cd "$REPO"
mkdir -p "$(dirname "$DB")"

echo "run_bridge: tap=$TAP db=$DB ws=ws://0.0.0.0:8787/telemetry" >&2

# tap stdout -> tee -> { recorder (SQLite), ws_server (WebSocket) }.
# When tap exits the pipe closes -> ws_server/recorder finish -> bash exits ->
# systemd restarts the unit.
"$TAP" \
  | tee >(python3 "$BRIDGE/recorder.py" "$DB") \
  | python3 "$BRIDGE/ws_server.py"
