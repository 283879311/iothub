#!/usr/bin/env bash
# 冻结 BIN 直烧（S6 阶段A 修定流程）：绕开 idf.py flash 的重编，写入前紧邻复查 sha256。
# 用法：
#   tools/flash_frozen.sh <bin> <expect_sha256> <port> <addr>
set -euo pipefail

BIN="${1:?bin}"; EXPECT="${2:?expect sha256}"; PORT="${3:?port}"; ADDR="${4:?addr}"
IDF_PY="${IDF_PYTHON_ENV_PATH:-/home/tc/.espressif/python_env/idf6.0_py3.12_env}/bin/python"

[ -f "$BIN" ] || { echo "[FAIL] BIN 不存在: $BIN"; exit 2; }
ACTUAL=$(sha256sum "$BIN" | awk '{print $1}')
if [ "$ACTUAL" != "$EXPECT" ]; then
  echo "[FAIL] 写入前复查不符：$ACTUAL != $EXPECT"; exit 1
fi
echo "[ok] 写入前复查通过 sha256=$ACTUAL"
echo "[run] esptool write_flash $ADDR $BIN (port=$PORT)"
"$IDF_PY" -m esptool --chip esp32 --port "$PORT" -b 460800 \
  write_flash --flash_mode dio --flash_freq 40m --flash_size 4MB \
  "$ADDR" "$BIN"
echo "[done] 烧录完成（务必再用 device.info.build_sha 反证）"
