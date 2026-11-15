#!/usr/bin/env bash
# S6 阶段 A · 冻结 BIN 直烧（**S6 专用，硬锁定目标与写入范围**）
#
# 设计约束（评审 c99340c 意见）：
#  1) 端口/芯片/地址/分区上限**写死在脚本内**，不接受调用者指定：
#       芯片 esp32、端口 /dev/ttyUSB1、地址 0x10000（ota_0 起始）、
#       上限 0x1E0000（ota_0 容量 1920KiB）
#  2) 校验与写入之间不留"文件替换窗口"：先把输入**复制到私有临时冻结目录**，
#     对副本计算 SHA-256，再把**该副本**交给 esptool；结束后保留副本与校验记录。
#  3) 不执行 erase-flash；write_flash 只覆盖 ota_0 范围内的扇区。
#
# 用法：tools/flash_frozen.sh <bin> <expect_sha256>
set -euo pipefail

if [ "$#" -ne 2 ]; then
  echo "[FAIL] 参数个数必须恰为 2：<bin> <expect_sha256>（端口/地址已锁定，不可传参）"
  exit 2
fi
BIN_SRC="$1"; EXPECT="$2"

# ---- S6 锁定常量（勿改；改需重新审批）----
LOCK_PORT="/dev/ttyUSB1"
LOCK_CHIP="esp32"
LOCK_ADDR="0x10000"
LOCK_PART_SIZE=$((0x1E0000))          # ota_0 容量
FLASH_MODE="dio"; FLASH_FREQ="40m"; FLASH_SIZE="4MB"

[ -f "$BIN_SRC" ] || { echo "[FAIL] BIN 不存在: $BIN_SRC"; exit 2; }

# ---- 复制到私有冻结目录：校验对象 == 写入对象 ----
FREEZE_DIR=$(mktemp -d "/tmp/s6_freeze.XXXXXX") || exit 2
FROZEN="$FREEZE_DIR/frozen_$(basename "$BIN_SRC")"
cp -- "$BIN_SRC" "$FROZEN"

# 长度检查基于**冻结副本**（实际写入对象）
SIZE=$(stat -c%s "$FROZEN")
END=$((LOCK_ADDR + SIZE))
PART_END=$((LOCK_ADDR + LOCK_PART_SIZE))
if [ "$SIZE" -gt "$LOCK_PART_SIZE" ] || [ "$END" -gt "$PART_END" ]; then
  echo "[FAIL] 写入区间越界：[$LOCK_ADDR, $END) 超出分区 [$LOCK_ADDR, $PART_END)"
  exit 1
fi
printf '[ok] 写入区间 [0x%X, 0x%X) 落在 ota_0 [0x%X, 0x%X) 内\n' \
  "$LOCK_ADDR" "$END" "$LOCK_ADDR" "$PART_END"

ACTUAL=$(sha256sum "$FROZEN" | awk '{print $1}')
if [ "$ACTUAL" != "$EXPECT" ]; then
  echo "[FAIL] 冻结副本 SHA-256 不符：$ACTUAL != $EXPECT"
  echo "       副本保留于 $FROZEN（未写入 Flash）"
  exit 1
fi
echo "[ok] 冻结副本校验通过"
echo "     副本     = $FROZEN"
echo "     sha256   = $ACTUAL"
echo "     size     = $SIZE (上限 $LOCK_PART_SIZE, 终点 0x$(printf %X "$END"))"
echo "     目标     = chip=$LOCK_CHIP port=$LOCK_PORT addr=$LOCK_ADDR"

# ---- 写入：使用副本路径，且写入前再次校验同一副本 ----
ACTUAL2=$(sha256sum "$FROZEN" | awk '{print $1}')
if [ "$ACTUAL2" != "$EXPECT" ]; then
  echo "[FAIL] 写入前复查不符：$ACTUAL2 != $EXPECT"; exit 1
fi

IDF_PY="${IDF_PYTHON_ENV_PATH:-/home/tc/.espressif/python_env/idf6.0_py3.12_env}/bin/python"
echo "[run] esptool write_flash $LOCK_ADDR (from $FROZEN)"
"$IDF_PY" -m esptool --chip "$LOCK_CHIP" --port "$LOCK_PORT" -b 460800 \
  write_flash --flash_mode "$FLASH_MODE" --flash_freq "$FLASH_FREQ" \
  --flash_size "$FLASH_SIZE" "$LOCK_ADDR" "$FROZEN"

ACTUAL3=$(sha256sum "$FROZEN" | awk '{print $1}')
if [ "$ACTUAL3" != "$EXPECT" ]; then
  echo "[FAIL] 写入后冻结副本 SHA-256 不符：$ACTUAL3 != $EXPECT"
  exit 1
fi
echo "[done] 写入后冻结副本 SHA-256 仍与授权值一致（$ACTUAL3）"
echo "       证据副本保留：$FROZEN"
echo "       务必再用 device.info.build_sha 反证固件内身份"
