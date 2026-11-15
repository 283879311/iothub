#!/usr/bin/env bash
# capture_guard.sh — 包裹"停采集→烧录→恢复"的任意命令，**保证退出时采集在跑**
#
# 目的（S6 阶段 B · R2）：烧录失败/中断/esptool 报错时 `set -e` 会让脚本直接退出，
# 采集可能停在"已停"，设备却处于部分写入或无法启动。把恢复采集从"正常路径的一步"
# 变成"退出路径的保证"。
#
# 采集状态：**调用 port_identity.py state**（与门禁/采集同一实现：PID+端口锁双验，
# 锁文件不存在时尝试创建并取得，无法判定即 UNKNOWN）。
#
# **执行前置条件**：进入时状态必须为 RUNNING（R2 前提：门禁已恢复采集）。
#   STOPPED / UNKNOWN → rc=92，**不执行被包裹命令、不触碰采集**。
#
# 返回码（恢复状态与命令返回码分离，恢复类失败优先）：
#   0..89  被包裹命令真实返回码（命令成功即 0）
#   90     采集未能恢复（不论命令返回什么）
#   91     恢复证据写入失败（不得视为成功）
#   92     进入前置条件不满足（未执行命令、未触碰采集）
#
# 用法：
#   tools/capture_guard.sh <port> <log> <capture_ctl> [--evidence <json>] -- <cmd...>
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

ROOT=$(cd "$(dirname "$0")/.." && pwd)

capture_state() {          # RUNNING | COOPERATING_CAPTURE_STOPPED | UNKNOWN
  # 严格存活判定：PID(含 cmdline=capture_serial) + 端口锁双证，不一致即 UNKNOWN。
  # 与门禁的 capture_state（锁被持有即保守 RUNNING）区分：这里回答"本仓库采集是否在跑"。
  python3 "$ROOT/tools/port_identity.py" liveness \
      --port "$PORT" --log-path "$LOG" 2>/dev/null || true
}

write_evidence() {         # $1=result  $2=restored(0/1)  $3=command_rc  $4=restart_rc
  [ -n "$EVID" ] || return 0
  G_OUT="$EVID" G_PORT="$PORT" G_LOG="$LOG" \
  G_STATE_BEFORE="$STATE_BEFORE" G_STATE_AFTER="${STATE_AFTER:-}" \
  G_RESULT="$1" G_RESTORED="$2" G_CMD_RC="$3" G_RESTART_RC="$4" \
  G_W0="$W0" G_W1="$(date +%s)" \
  python3 - <<'PY'
import json, os, time
e = os.environ
rrc = e.get("G_RESTART_RC") or ""
doc = {
    "version": 3,
    "port": e["G_PORT"], "log": e["G_LOG"],
    "state_before": e["G_STATE_BEFORE"],
    "state_after": e.get("G_STATE_AFTER") or None,
    "capture_restored": e["G_RESTORED"] == "1",
    "command_rc": int(e["G_CMD_RC"]),
    "restart_rc": (int(rrc) if rrc else None),
    "guard_window_start_unix": int(e["G_W0"]),
    "guard_window_end_unix": int(e["G_W1"]),
    "guard_window_s": int(e["G_W1"]) - int(e["G_W0"]),
    "result": e["G_RESULT"],
    "at_unix": int(time.time()),
}
with open(e["G_OUT"], "w", encoding="utf-8") as f:
    f.write(json.dumps(doc, ensure_ascii=False, indent=1))
PY
}

STATE_BEFORE=$(capture_state)
W0=$(date +%s)
PRECOND_FAIL=0

guard_exit() {
  local rc=$?
  set +e
  if [ "$PRECOND_FAIL" = "1" ]; then
    write_evidence "precondition_failed" 0 "$rc" ""
    exit 92
  fi
  STATE_AFTER=$(capture_state)
  RESTORED=0
  RESTART_RC=""
  if [ "$STATE_AFTER" = "RUNNING" ]; then
    RESTORED=1
  else
    echo "[guard] 命令退出($rc)后采集状态=$STATE_AFTER——尝试重启" >&2
    "$CTL" start "$PORT" "$LOG"; RESTART_RC=$?
    sleep 1
    STATE_AFTER=$(capture_state)
    [ "$STATE_AFTER" = "RUNNING" ] && RESTORED=1
  fi
  write_evidence "$([ "$RESTORED" = 1 ] && echo restored || echo recovery_failed)" \
                 "$RESTORED" "$rc" "$RESTART_RC"
  local evid_rc=$?
  if [ "$RESTORED" != "1" ]; then
    echo "[guard][RECOVERY FAILED] 采集未能恢复（state=$STATE_AFTER）——不得宣称恢复成功" >&2
    exit 90
  fi
  if [ "$evid_rc" -ne 0 ]; then
    echo "[guard][EVIDENCE FAILED] 采集已恢复但恢复证据写入失败——按失败处理" >&2
    exit 91
  fi
  echo "[guard] 采集运行中（guard 窗口 ${W0}s 起）"
  exit "$rc"
}

trap guard_exit EXIT
trap 'exit 143' TERM INT

# ---- 执行前置条件：必须是 RUNNING（否则不执行命令、不触碰采集）----
if [ "$STATE_BEFORE" != "RUNNING" ]; then
  PRECOND_FAIL=1
  echo "[guard][PRECONDITION FAILED] 进入时采集状态=${STATE_BEFORE:-空}（要求 RUNNING）" >&2
  echo "        未执行被包裹命令，未触碰采集；rc=92" >&2
  exit 92
fi

echo "[guard] 进入：采集状态=$STATE_BEFORE"
"${CMD[@]}"
rc=$?
echo "[guard] 命令退出 rc=$rc"
exit "$rc"
