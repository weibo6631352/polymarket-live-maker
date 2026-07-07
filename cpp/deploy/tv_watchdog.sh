#!/usr/bin/env bash
# tv_watchdog.sh — 会话外的持久看门狗 (由 tv-watchdog.timer 每 5min 触发)。
#
# 补 systemd Restart=on-failure 与会话内 Monitor 之间的洞: 2026-07-06 一次网络抖动误触 halt ->
# 干净退出 (code 0, systemd 不拉起) -> 8h 无人管。分层策略, 既保活又不违背"真拒单保全现场":
#   · STOP flag 存在            -> 尊重人工急停, 不动
#   · 真业务拒单 halt (4xx)     -> 告警但不自动拉起 (保全现场等人查)
#   · 其它 down (崩溃/意外退出)  -> 限速自动拉起 (每小时 ≤4 次, 超则闩锁告警防崩溃环)
#   · 活着但卡死 (心跳 >15min)  -> 拉起
#   · 白名单 >90min 未刷新       -> 告警 (策展 timer 停了 = 该更新市场池了)
# 每轮把裁决写 state/tv_watchdog_status.json (可 pmpull 一眼看健康)。只读+systemctl, 不碰交易账本。
set -uo pipefail
REPO=/root/polymarket-live-maker
STATE=$REPO/state
UNIT=tail-vendor-trial
WL=$STATE/tail_vendor_whitelist.json
STATUS=$STATE/tv_watchdog_status.json
RCNT=$STATE/tv_restart_count          # "epoch_hour n" — 崩溃环限速
NOW=$(date -u +%s)

active=$(systemctl is-active "$UNIT" 2>/dev/null || true)
verdict=ok; action=none

# 心跳: 最近一次 scan 的 t_ms (epoch 窗口, 不用 -n 以免被一轮内的 place/cancel 洪水挤出)
last_scan_ms=$(journalctl -u "$UNIT" --since "@$((NOW-1200))" -o cat 2>/dev/null \
  | grep '"ev":"scan"' | tail -1 | grep -o '"t_ms":[0-9]*' | grep -o '[0-9]*' || true)
hb_age=99999
[ -n "${last_scan_ms:-}" ] && hb_age=$(( NOW - last_scan_ms/1000 ))

wl_age=99999
[ -f "$WL" ] && wl_age=$(( NOW - $(stat -c %Y "$WL") ))

do_restart() {
  local hr=$(( NOW / 3600 )) prev_hr=0 prev_n=0
  if [ -f "$RCNT" ]; then read -r prev_hr prev_n < "$RCNT" || { prev_hr=$hr; prev_n=0; }; fi
  [ "$prev_hr" != "$hr" ] && prev_n=0
  if [ "${prev_n:-0}" -ge 4 ]; then verdict="CRASHLOOP-latched"; action="no-restart"; return; fi
  echo "$hr $((prev_n+1))" > "$RCNT"
  systemctl restart "$UNIT" && action="restarted"
}

if [ -f "$REPO/STOP_TAIL_VENDOR" ]; then
  verdict=stopflag; action=respect-stop
elif [ "$active" != "active" ]; then
  last_halt=$(journalctl -u "$UNIT" --since "@$((NOW-1200))" -o cat 2>/dev/null | grep '"ev":"halt"' | tail -1 || true)
  if printf '%s' "$last_halt" | grep -q "business rejects"; then
    verdict="HALT-business"; action="alert-no-restart"     # 保全现场
  else
    verdict="down"; do_restart
  fi
elif [ "$hb_age" -gt 900 ]; then
  verdict="hung(hb=${hb_age}s)"; do_restart                # 活着但不扫描
elif [ "$wl_age" -gt 5400 ]; then
  verdict="stale-whitelist(${wl_age}s)"; action=alert      # 提醒更新市场池
fi

printf '{"t":%d,"active":"%s","hb_age_s":%d,"wl_age_s":%d,"verdict":"%s","action":"%s"}\n' \
  "$NOW" "${active:-unknown}" "$hb_age" "$wl_age" "$verdict" "$action" > "$STATUS"
# 非 ok 裁决也吐到 journal (tv-watchdog 单元), 便于 journalctl 追溯
[ "$verdict" != ok ] && echo "tv_watchdog: verdict=$verdict action=$action hb=${hb_age}s wl=${wl_age}s"
exit 0
