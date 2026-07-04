#!/usr/bin/env bash
# 箱上自动策展一跑 (由 tail-vendor-curate.timer 每小时触发)。
# census→deribit→map→rank → 原子写 state/tail_vendor_whitelist.json (bot 每轮热读)。
set -euo pipefail
export HOME=/root
REPO=/root/polymarket-live-maker
cd "$REPO/backtest/deribit_fair"
restore() { cd "$REPO" && git checkout -- backtest/deribit_fair/discrepancies.json 2>/dev/null || true; }
trap restore EXIT

python3 census.py
python3 deribit_pull.py
python3 map_markets.py
python3 make_whitelist.py 24 > /tmp/tv_wl.json
python3 - << 'PY'
import json
d = json.load(open("/tmp/tv_wl.json"))
assert len(d.get("slugs", [])) >= 5, "too few slugs — refusing to overwrite whitelist"
PY
mv /tmp/tv_wl.json "$REPO/state/tail_vendor_whitelist.json"
echo "whitelist updated: $(python3 -c 'import json;print(len(json.load(open("/root/polymarket-live-maker/state/tail_vendor_whitelist.json"))["slugs"]))') slugs"
