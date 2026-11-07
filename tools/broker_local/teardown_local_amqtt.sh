#!/usr/bin/env bash
# 关闭并删除本机 amqtt 测试 broker（默认 dry-run，--apply 才执行）
#
# 用法：
#   sudo bash teardown_local_amqtt.sh              # 只列清单，不动任何东西
#   sudo bash teardown_local_amqtt.sh --apply      # 停止进程 + 备份后删除安装与配置
#
# 设计约束：
#   - 删除一律"先移到备份目录"，不做 rm -rf（备份目录会打印出来）
#   - 只处理 amqtt 相关：进程 / pip 包 / 配置 / 日志，不碰其它服务
#   - 结束时校验端口是否真的释放
set -uo pipefail

APPLY=0
[ "${1:-}" = "--apply" ] && APPLY=1

PORT="${PORT:-1883}"
TS="$(date +%Y%m%d_%H%M%S)"
BACKUP="/tmp/amqtt_teardown_$TS"
MOVED=()

say() { printf '%s\n' "$*"; }
hr()  { say "------------------------------------------------------------"; }

say "amqtt 清理计划（PORT=$PORT，模式=$([ $APPLY -eq 1 ] && echo APPLY || echo DRY-RUN)）"
hr

# ---------- 1. 定位进程 ----------
# 只认两类：①真正监听 PORT 的 pid（ss，最准）②cmdline 里是 amqtt/python 且不含本脚本名。
# 必须排除自身：本脚本/其包装 shell 的 cmdline 里也含 "amqtt" 字样。
say "[1/5] 定位监听 $PORT 的进程"
SELF="$$"
PIDS=""
if command -v ss >/dev/null 2>&1; then
  PIDS="$(ss -ltnpH "sport = :$PORT" 2>/dev/null \
          | grep -oP 'pid=\K[0-9]+' | sort -u | tr '\n' ' ')"
fi
if [ -z "${PIDS// /}" ] && command -v pgrep >/dev/null 2>&1; then
  for p in $(pgrep -f 'amqtt' 2>/dev/null); do
    [ "$p" = "$SELF" ] && continue
    cl="$({ tr '\0' ' ' < "/proc/$p/cmdline"; } 2>/dev/null || true)"
    case "$cl" in
      *teardown_local_amqtt*) continue ;;   # 本脚本及其包装 shell
    esac
    # 首词只允许 python*/amqtt，排除 bash -c 包装
    first="$(printf '%s' "$cl" | awk '{print $1}' | xargs -r basename 2>/dev/null)"
    case "$first" in
      python*|amqtt*|*/amqtt) PIDS="$PIDS $p" ;;
    esac
  done
fi
PIDS="$(printf '%s\n' $PIDS | awk 'NF && !seen[$0]++' | tr '\n' ' ')"
if [ -z "${PIDS// /}" ]; then
  say "  未找到 amqtt 进程（端口 $PORT 也没有监听者）"
else
  for p in $PIDS; do
    [ -d "/proc/$p" ] || continue
    say "  PID $p"
    say "    user   : $(stat -c %U /proc/$p 2>/dev/null || echo '?')"
    say "    cmdline: $(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null | cut -c1-160)"
    say "    exe    : $(readlink -f /proc/$p/exe 2>/dev/null || echo '?')"
    say "    cwd    : $(readlink -f /proc/$p/cwd 2>/dev/null || echo '?')"
    say "    started: $(ps -o lstart= -p "$p" 2>/dev/null | xargs)"
  done
fi
hr

# ---------- 2. 定位安装 ----------
say "[2/5] 定位 amqtt 安装位置"
INSTALLS=()
# 2a. 进程 exe/cwd 推出来的 venv
for p in ${PIDS:-}; do
  [ -d "/proc/$p" ] || continue
  exe="$(readlink -f /proc/$p/exe 2>/dev/null || true)"
  cwd="$(readlink -f /proc/$p/cwd 2>/dev/null || true)"
  for base in "$exe" "$cwd"; do
    case "$base" in
      */bin/python*|*/bin/amqtt) INSTALLS+=("$(dirname "$(dirname "$base")")") ;;
    esac
  done
done
# 2b. 常见 venv 与系统 site-packages
for venv in /home/*/.venv* /home/*/*/venv /home/*/*/.venv /opt/*/venv /root/.venv*; do
  [ -d "$venv" ] || continue
  if compgen -G "$venv/lib/python*/site-packages/amqtt*" >/dev/null 2>&1; then
    INSTALLS+=("$venv")
  fi
done
# 2c. 系统级
for sp in /usr/lib/python3*/site-packages /usr/local/lib/python3*/dist-packages /usr/lib/python3/dist-packages; do
  compgen -G "$sp/amqtt*" >/dev/null 2>&1 && INSTALLS+=("SYSTEM:$sp")
done

INSTALLS=($(printf '%s\n' "${INSTALLS[@]:-}" | awk 'NF && !seen[$0]++'))
if [ ${#INSTALLS[@]} -eq 0 ]; then
  say "  未在常见位置找到 amqtt 包（可能装在沙箱/容器/其它用户目录）"
else
  for i in "${INSTALLS[@]}"; do say "  $i"; done
fi
hr

# ---------- 3. 定位配置与日志 ----------
say "[3/5] 定位配置与日志"
CONFS=()
for c in /home/*/amqtt.yaml /home/*/amqtt.yml /home/*/.amqtt /home/*/.config/amqtt \
         /etc/amqtt /etc/amqtt.yaml /etc/amqtt.yml /root/amqtt.yaml; do
  [ -e "$c" ] && CONFS+=("$c")
done
# 进程 cwd 下的 yaml
for p in ${PIDS:-}; do
  cwd="$(readlink -f /proc/$p/cwd 2>/dev/null || true)"
  [ -n "$cwd" ] || continue
  for f in "$cwd"/amqtt.yaml "$cwd"/amqtt.yml; do
    [ -f "$f" ] && CONFS+=("$f")
  done
done
CONFS=($(printf '%s\n' "${CONFS[@]:-}" | awk 'NF && !seen[$0]++'))
if [ ${#CONFS[@]} -eq 0 ]; then
  say "  未找到配置文件"
else
  for c in "${CONFS[@]}"; do say "  $c"; done
fi
LOGS=()
for l in /home/*/amqtt*.log /home/*/*/amqtt*.log /tmp/amqtt*.log; do
  [ -f "$l" ] && LOGS+=("$l")
done
[ ${#LOGS[@]} -gt 0 ] && { say "  日志:"; for l in "${LOGS[@]}"; do say "    $l"; done; }
hr

# ---------- 4. 执行 ----------
if [ $APPLY -eq 0 ]; then
  say "[4/5] DRY-RUN：未做任何改动。确认清单无误后执行："
  say "      sudo bash $0 --apply"
  hr
  say "[5/5] 结束"
  exit 0
fi

mkdir -p "$BACKUP" || { say "无法创建备份目录 $BACKUP，中止"; exit 1; }
say "[4/5] 执行：备份 → 停止 → 删除"

# 4a. 先备份配置
for c in ${CONFS[@]:-}; do
  [ -e "$c" ] || continue
  cp -a "$c" "$BACKUP"/ 2>/dev/null && say "  备份 $(basename "$c") → $BACKUP/"
done

# 4b. 停止进程
if [ -n "${PIDS// /}" ]; then
  for p in $PIDS; do
    [ -d "/proc/$p" ] || continue
    say "  SIGTERM → $p"
    kill -TERM "$p" 2>/dev/null
  done
  for _ in $(seq 1 8); do
    sleep 1
    still=""
    for p in $PIDS; do [ -d "/proc/$p" ] && still="$still $p"; done
    [ -z "$still" ] && break
  done
  for p in ${still:-}; do
    say "  仍存活，SIGKILL → $p"
    kill -KILL "$p" 2>/dev/null
  done
  sleep 1
  say "  amqtt 进程已停止"
else
  say "  无进程可停"
fi

# 4c. 卸载安装（优先 pip uninstall，保证 dist-info 一起清）
for i in ${INSTALLS[@]:-}; do
  [ -n "$i" ] || continue
  case "$i" in
    SYSTEM:*)
      say "  系统包，跳过自动卸载：$i（需手工 pip uninstall amqtt）";;
    *)
      if [ -x "$i/bin/pip" ]; then
        say "  $i/bin/pip uninstall -y amqtt"
        "$i/bin/pip" uninstall -y amqtt 2>&1 | sed 's/^/    /'
      elif [ -x "$i/bin/python" ]; then
        say "  $i/bin/python -m pip uninstall -y amqtt"
        "$i/bin/python" -m pip uninstall -y amqtt 2>&1 | sed 's/^/    /'
      else
        # 兜底：移走包目录（先备份）
        for d in "$i"/lib/python*/site-packages/amqtt*; do
          [ -e "$d" ] || continue
          say "  移走 $d → $BACKUP/"
          mv "$d" "$BACKUP"/ && MOVED+=("$d")
        done
      fi;;
  esac
done

# 4d. 移走配置与日志
for c in ${CONFS[@]:-} ${LOGS[@]:-}; do
  [ -e "$c" ] || continue
  say "  移走 $c → $BACKUP/"
  mv "$c" "$BACKUP"/ 2>/dev/null && MOVED+=("$c")
done

hr
# ---------- 5. 校验 ----------
say "[5/5] 校验"
if ss -ltn 2>/dev/null | grep -q ":$PORT "; then
  say "  ⚠ 端口 $PORT 仍在监听："
  ss -ltnp 2>/dev/null | grep ":$PORT "
  say "  可能还有其他 broker 实例，或进程被守护重启"
  rc=1
else
  say "  ✓ 端口 $PORT 已释放"
  rc=0
fi
say "  备份/移走的文件都在：$BACKUP"
[ ${#MOVED[@]} -gt 0 ] && { say "  已移走:"; for m in "${MOVED[@]}"; do say "    $m"; done; }
say ""
say "回滚：把 $BACKUP 里的文件移回原处即可（脚本输出里记录了原路径）。"
exit $rc
