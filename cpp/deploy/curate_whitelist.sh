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
# 完整性闸门 (FAIL-CLOSED): 逐级校验 census/deribit/map 两币齐全+量在带内 + 新名单不异常坍缩;
# 任一级"信息不完整"就非零退出 → set -e 中止 → 不 mv → 保留上一版好名单 (watchdog 会在 >90min 告警)。
python3 validate_curation.py /tmp/tv_wl.json "$REPO/state/tail_vendor_whitelist.json"
mv /tmp/tv_wl.json "$REPO/state/tail_vendor_whitelist.json"
echo "whitelist updated: $(python3 -c 'import json;print(len(json.load(open("/root/polymarket-live-maker/state/tail_vendor_whitelist.json"))["slugs"]))') slugs"
