#!/usr/bin/env bash
# set_pmkey.sh — 安全写入 POLYMARKET_PRIVATE_KEY 到本仓库的 .env。
# 私钥从隐藏输入读取：不进 shell 历史、不进命令行参数、永不打印。
# 在仓库所在目录操作（脚本自身所在目录），box / 开发机通用。
set -euo pipefail
cd "$(cd "$(dirname "$0")" && pwd)"
ENV=.env
[ -f "$ENV" ] || { echo "找不到 $ENV（请在已部署的仓库目录里跑）"; exit 1; }
cp -f "$ENV" "$ENV.bak.$(date +%s)"
echo "把你的 Polymarket 私钥粘进来（输入隐藏，看不见是正常的），然后回车："
read -rs PK
echo
PK_TRIM="$(printf '%s' "$PK" | tr -d '[:space:]')"
case "$PK_TRIM" in
  0x*) : ;;
  *)   PK_TRIM="0x$PK_TRIM" ;;
esac
n=${#PK_TRIM}
if [ "$n" -ne 66 ]; then
  echo "WARN: 私钥长度是 $n（应为 66 = 0x + 64 位十六进制）。已中止，.env 未改动。"
  unset PK PK_TRIM
  exit 1
fi
PK_TRIM="$PK_TRIM" python3 - "$ENV" <<'PY'
import os, sys
env = sys.argv[1]; pk = os.environ["PK_TRIM"]
lines = open(env).read().splitlines()
seen = False; out = []
for l in lines:
    if l.startswith("POLYMARKET_PRIVATE_KEY="):
        out.append("POLYMARKET_PRIVATE_KEY=" + pk); seen = True
    else:
        out.append(l)
if not seen:
    out.append("POLYMARKET_PRIVATE_KEY=" + pk)
open(env, "w").write("\n".join(out) + "\n")
print("OK: POLYMARKET_PRIVATE_KEY 已写入，长度", len(pk))
PY
unset PK PK_TRIM
echo "完成。已自动备份 .env，私钥只在 .env 里、全程没有打印。"
