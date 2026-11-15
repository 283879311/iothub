#!/usr/bin/env bash
# capture_guard.sh — 包裹"停采集→烧录→恢复"的任意命令，**保证退出时采集在跑**
#
# 用途（S6 阶段 B · R2）：烧录失败/中断/esptool 报错时，`set -e` 会让脚本直接退出，
# 采集可能停在"已停"状态，设备却处于部分写入或无法启动。本脚本把恢复采集
# 从"正常路径的一步"变成"退出路径的保证"：
#   1) 记录进入时采集是否在跑（端口协作锁是否被持有）；
#   2) 执行被包裹命令（不因非零退出而提前中断）；
#   3) 退出时（含 TERM/INT）若采集未运行 → 用 capture_ctl 重启并复查；
#   4) 无法恢复 → **明确失败**（exit 9），绝不输出成功结论；
#   5) 可选写出 JSON 证据（GUARD_EVIDENCE=<path>）：gap 起止、命令 rc、重启结果。
#
# 用法：
#   tools/capture_guard.sh <port> <log> <capture_ctl> [--evidence <json>] -- <cmd...>
#
# 例：
#   tools/capture_guard.sh /dev/ttyUSB1 $LOG tools/serial_capture_ctl.sh -- \
#       sh -c 'tools/serial_capture_ctl.sh stop /dev/ttyUSB1 '"$LOG"'; tools/flash_frozen.sh ...'
set -uo pipefail

PORT="${1:-}"; LOG="${2:-}"; CTL="${3:-}"
[ -n "$PORT" ] && [ -n "$LOG" ] && [ -n "$CTL" ] || {
  echo "用法: $0 <port> <log> <capture_ctl> [--evidence <json>] -- <cmd...>" >&2
  exit 2
}
shift 3
EVID=""
if [ "${1:-}" = "--evidence" ]; then EVID="${2:-}"; shift 2; fi
[ "${1:-}" = "--" ] && shift
[ "$#" -gt 0 ] || { echo "[FAIL] 缺少要执行的命令" >&2; exit 2; }
CMD=("$@")

# ---- 端口锁路径（与 capture_serial.py / 门禁 / flash 同一规则）----
real_port=$(readlink -f -- "$PORT" 2>/dev/null || true)
[ -n "$real_port" ] || real_port="$PORT"
lock_id=$(basename "$real_port")
if [ -w /run/lock ]; then LOCK_DIR=/run/lock; else LOCK_DIR=/tmp; fi
LOCK_FILE="$LOCK_DIR/iothub_serial_${lock_id}.lock"

capture_running() { ! flock -n "$LOCK_FILE" -c true 2>/dev/null; }

RUNNING_BEFORE=0
capture_running && RUNNING_BEFORE=1
T0=$(date +%s)
RESTART_RC=""
RESTORED=0

guard_exit() {
  local rc=$?
  set +e
  if capture_running; then
    RESTORED=1
  else
    echo "[guard] 命令退出($rc)后采集未运行——尝试重启" >&2
    "$CTL" start "$PORT" "$LOG"; RESTART_RC=$?
    sleep 1
    capture_running && RESTORED=1
  fi
  T1=$(date +%s)
  if [ -n "$EVID" ]; then
    python3 - "$EVID" "$PORT" "$LOG" "$RUNNING_BEFORE" "$rc" "$RESTORED" \
      "$RESTART_RC" "$T0" "$T1" <<'PY' || echo "[guard] 证据写入失败" >&2
import json, sys, time
out, port, log, rb, rc, restored, rrc, t0, t1 = sys.argv[1:10]
doc = {
    "version": 1,
    "port": port, "log": log,
    "capture_running_before": rb == "1",
    "command_rc": int(rc),
    "gap_start_unix": int(t0), "gap_end_unix": int(t1),
    "gap_s": int(t1) - int(t0),
    "restart_rc": (int(rrc) if rrc not in ("", None) else None),
    "capture_restored": restored == "1",
    "result": "restored" if restored == "1" else "recovery_failed",
    "at_unix": int(time.time()),
}
with open(out, "w", encoding="utf-8") as f:
    f.write(json.dumps(doc, ensure_ascii=False, indent=1))
PY
  fi
  if [ "$RESTORED" != "1" ]; then
    echo "[guard][RECOVERY FAILED] 采集未能恢复——不得宣称恢复成功" >&2
    [ "$rc" = "0" ] && rc=9
  else
    echo "[guard] 采集已在运行（gap ${T0}->${T1}）"
  fi
  exit "$rc"
}

trap guard_exit EXIT
trap 'exit 143' TERM INT

echo "[guard] 进入：采集运行=$RUNNING_BEFORE 锁=$LOCK_FILE"
"${CMD[@]}"
rc=$?
echo "[guard] 命令退出 rc=$rc"
exit "$rc"
