#!/usr/bin/env bash
# serial_capture_ctl.sh — 串口抓包的启停控制器（PID 文件机制）
#
# 背景（2026-11-07）：pkill -f 自匹配误杀已发生三次——包装 shell 的命令行里
# 含与模式相同的字面量时，pkill 会把执行中的包装 shell 自己杀掉。本脚本用
# **PID 文件**管理抓包进程，全流程零 pkill。
#
# 用法：
#   tools/serial_capture_ctl.sh start /dev/ttyUSB0 out/serial.log
#   tools/serial_capture_ctl.sh stop  out/serial.log
#   tools/serial_capture_ctl.sh status out/serial.log
#
# 行为：
#   start  : 拒绝重复启动（同日志文件的 PID 文件存在且进程存活时报错退出）；
#            setsid 独立进程组启动 capture_serial.py，PID 写 <logfile>.pid；
#   stop   : 按 PID 文件 kill -TERM，等待退出（≤5s）后强杀；删除 PID 文件；
#            **不碰其它进程**——即使模式写错也不会误伤；
#   status : 报告 PID 存活与日志字节数。
set -uo pipefail

CMD="${1:-}"
PORT="${2:-}"
# start 需要 <port> <logfile>；status/stop 只需要 <logfile>
LOG="${3:-${2:-}}"

pidfile() { echo "$LOG.pid"; }

is_alive() {
    [ -n "${1:-}" ] && [ -d "/proc/$1" ] && grep -q capture_serial "/proc/$1/cmdline" 2>/dev/null
}

case "$CMD" in
    start)
        [ -n "$PORT" ] && [ -n "${3:-}" ] || { echo "用法: $0 start <port> <logfile>" >&2; exit 2; }
        PF="$(pidfile)"
        if [ -f "$PF" ]; then
            OLD="$(cat "$PF" 2>/dev/null || true)"
            if is_alive "$OLD"; then
                echo "!! 抓包已在运行 pid=$OLD 日志=$LOG——先 stop 再 start" >&2
                exit 1
            fi
            rm -f "$PF"   # 陈旧 PID 文件：进程已死，清理
        fi
        mkdir -p "$(dirname "$LOG")"
        setsid nohup python3 "$(dirname "$0")/capture_serial.py" "$PORT" "$LOG" \
            > "$LOG.stdout" 2>&1 < /dev/null &
        NEW=$!
        echo "$NEW" > "$PF"
        sleep 1
        if is_alive "$NEW"; then
            echo "capture started pid=$NEW log=$LOG"
        else
            echo "!! capture 启动即退出（端口占用/权限），见 $LOG.stdout" >&2
            rm -f "$PF"
            exit 1
        fi
        ;;
    stop)
        [ -n "$LOG" ] || { echo "用法: $0 stop <logfile>" >&2; exit 2; }
        PF="$(pidfile)"
        if [ ! -f "$PF" ]; then
            echo "无 PID 文件（可能已停止）: $PF"
            exit 0
        fi
        PID="$(cat "$PF" 2>/dev/null || true)"
        if is_alive "$PID"; then
            kill -TERM "$PID"
            for _ in 1 2 3 4 5; do
                sleep 1
                is_alive "$PID" || break
            done
            is_alive "$PID" && kill -KILL "$PID"
            echo "capture stopped pid=$PID"
        else
            echo "pid=$PID 已不存活（清理陈旧 PID 文件）"
        fi
        rm -f "$PF"
        ;;
    status)
        [ -n "$LOG" ] || { echo "用法: $0 status <logfile>" >&2; exit 2; }
        PF="$(pidfile)"
        if [ -f "$PF" ] && is_alive "$(cat "$PF" 2>/dev/null || true)"; then
            echo "running pid=$(cat "$PF") log=$LOG bytes=$(wc -c < "$LOG" 2>/dev/null || echo 0)"
        else
            echo "not running (log=$LOG bytes=$(wc -c < "$LOG" 2>/dev/null || echo 0))"
        fi
        ;;
    *)
        echo "用法: $0 {start|stop|status} <port> <logfile>" >&2
        exit 2
        ;;
esac
