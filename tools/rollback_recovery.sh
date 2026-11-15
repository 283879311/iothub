#!/usr/bin/env bash
# rollback_recovery.sh — S6 阶段 B · R2 恢复序列（带采集恢复保证）
#
# 序列（缺一不可，失败即退）：
#   1) 回滚门禁 `rollback_gate.py --recovery-mode`（必须 rc=0，否则**不烧录**）
#   2) 停采集（记计划内 gap）→ 烧录冻结回滚镜像（flash_frozen.sh 自带端口锁）
#   3) 无论烧录成功/失败/中断，都由 capture_guard.sh **保证采集恢复**
#
# 为什么需要第 3 步：`set -e` 在烧录失败时会直接退出，采集会停在"已停"；
# 设备此时可能已部分写入，没有采集更没法取证。capture_guard 把"恢复采集"
# 从"正常路径的一步"变成"退出路径的保证"，恢复不了就明确失败(rc=9)。
#
# 用法：
#   tools/rollback_recovery.sh --log-dir <本轮目录> --anchor-file <锚点> \
#       --evidence-dir <本轮全新证据目录> --attest-no-external-holder "<说明>"
#
# 产物：<evidence-dir>/rollback_recovery.json（gap 起止、烧录 rc、重启结果）
#       以及门禁自己的 mac_read_flow*.json / rollback_gate_last.json
#
# 本脚本只做"门禁→停采集→烧录→恢复采集"。烧录后的 BLE 反证与状态核对
# 由申请人按 §3.1 R2 第 3–5 步另行执行。
set -uo pipefail

PORT="/dev/ttyUSB1"
RB_BIN="docs/evidence/S6_stageB/bin/iothub_a7e04b6a9.bin"
RB_SHA="047e4726455db505a5433bd3cb90a94022144add78361d6d8a52548e79f0ea41"

LOG_DIR=""; ANCHOR=""; EVID=""; ATTEST=""; CTL="tools/serial_capture_ctl.sh"

usage() {
  echo "用法: $0 --log-dir <dir> --anchor-file <json> --evidence-dir <new dir> \\" >&2
  echo "          --attest-no-external-holder <说明> [--capture-ctl <脚本>]" >&2
  exit 2
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --log-dir) LOG_DIR="${2:-}"; shift 2 ;;
    --anchor-file) ANCHOR="${2:-}"; shift 2 ;;
    --evidence-dir) EVID="${2:-}"; shift 2 ;;
    --attest-no-external-holder) ATTEST="${2:-}"; shift 2 ;;
    --capture-ctl) CTL="${2:-}"; shift 2 ;;
    *) echo "[FAIL] 未知参数: $1" >&2; usage ;;
  esac
done
[ -n "$LOG_DIR" ] && [ -n "$ANCHOR" ] && [ -n "$EVID" ] && [ -n "$ATTEST" ] || usage

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || { echo "[FAIL] 无法进入仓库根目录" >&2; exit 2; }

LOG="$LOG_DIR/serial_ttyUSB1.log"
REC_EVID="$EVID/rollback_recovery.json"

echo "[R2] 1/3 回滚门禁（恢复模式）"
python3 tools/rollback_gate.py --port "$PORT" --log-dir "$LOG_DIR" \
  --anchor-file "$ANCHOR" --bin "$RB_BIN" --expect-bin-sha256 "$RB_SHA" \
  --evidence-dir "$EVID" --attest-no-external-holder "$ATTEST" --recovery-mode
GATE_RC=$?
if [ "$GATE_RC" -ne 0 ]; then
  echo "[STOP] 回滚门禁未通过（rc=$GATE_RC）——禁止烧录，采集未触碰" >&2
  exit 1
fi

echo "[R2] 2/3 停采集 → 烧录（guard 包裹，退出必恢复采集）"
tools/capture_guard.sh "$PORT" "$LOG" "$CTL" --evidence "$REC_EVID" -- \
  bash -c 'ct="$1"; port="$2"; log="$3"; bin="$4"; sha="$5"
           "$ct" stop "$port" "$log"      # 不看 rc：由 guard 按实际锁状态复查
           tools/flash_frozen.sh "$bin" "$sha"' \
  _ "$CTL" "$PORT" "$LOG" "$RB_BIN" "$RB_SHA"
FLASH_RC=$?

echo "[R2] 3/3 完成检查"
if [ "$FLASH_RC" -eq 9 ]; then
  echo "[RECOVERY FAILED] 烧录未完成或采集未能恢复——设备标注未恢复，人工介入" >&2
  exit 9
fi
if [ "$FLASH_RC" -ne 0 ]; then
  echo "[STOP] 烧录失败（rc=$FLASH_RC）；采集已由 guard 恢复，禁止重试烧录，人工介入" >&2
  exit "$FLASH_RC"
fi
echo "[OK] 烧录返回 0 且采集已恢复。下一步（人工）：等新 boot → BLE 反证"
echo "     device.info.build_sha == a7e04b6a9 → trial=idle / 无活动 op / mqtt_connected=true"
