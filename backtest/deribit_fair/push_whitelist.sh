#!/usr/bin/env bash
# 一键刷新 tail-vendor 策展名单: 管线 → 排名 → 推箱子 (bot 每轮热读, 无需重启)。
# 用法: ./push_whitelist.sh [N=12]   (需一次性: python3 -m venv .venv && .venv/bin/pip install requests)
set -euo pipefail
cd "$(dirname "$0")"
N="${1:-12}"
PY="${WL_PY:-}"
if [ -z "$PY" ]; then
  for cand in ./.venv/bin/python3 /private/tmp/claude-501/*/*/scratchpad/venv/bin/python3; do
    [ -x "$cand" ] && PY="$cand" && break
  done
fi
[ -z "$PY" ] && { echo "no venv python with requests; set WL_PY=/path/to/python3" >&2; exit 2; }

echo "== pipeline (census -> deribit -> map) ==" >&2
"$PY" census.py >&2
"$PY" deribit_pull.py >&2
"$PY" map_markets.py >&2
echo "== ranking ==" >&2
"$PY" make_whitelist.py "$N" > /tmp/tail_whitelist.json
B64=$(base64 < /tmp/tail_whitelist.json | tr -d '\n')
echo "== push to box ==" >&2
pmbox "echo $B64 | base64 -d > /root/polymarket-live-maker/state/tail_vendor_whitelist.json && python3 -c 'import json; d=json.load(open(\"/root/polymarket-live-maker/state/tail_vendor_whitelist.json\")); print(len(d[\"slugs\"]), \"slugs installed, generated_at\", d[\"generated_at\"])'"
