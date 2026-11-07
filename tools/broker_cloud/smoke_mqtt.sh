#!/usr/bin/env bash
# broker 连通性冒烟：在云主机本地或任意有 mosquitto-clients 的机器上执行
# 用法：bash smoke_mqtt.sh <host> <port> [token]
#   token 用于模拟设备：固件只发 username（= mqtt_token），不发密码
#
# 五项检查：
#   1) TCP 端口可达
#   2) 匿名 CONNECT（mosquitto_sub 无凭据）
#   3) 设备形态 CONNECT（-u token，无密码）—— 若此项失败，设备必然连不上
#   4) 自定义主题回环 iothub/smoke/<ts>
#   5) ThingsBoard 主题回环 v1/devices/me/telemetry（设备 backend=tb_selfhost 时用）

set -uo pipefail

HOST="${1:-127.0.0.1}"
PORT="${2:-21884}"
TOKEN="${3:-smoke-token-0001}"
TS="$(date +%s)"
TOPIC_SMOKE="iothub/smoke/${TS}"
TOPIC_TB="v1/devices/me/telemetry"
FAIL=0
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass() { echo "  [PASS] $*"; }
fail() { echo "  [FAIL] $*"; FAIL=$((FAIL+1)); }

echo "== smoke: ${HOST}:${PORT}"

# 1) TCP 可达
if timeout 3 bash -c "</dev/tcp/${HOST}/${PORT}" 2>/dev/null; then
  pass "TCP ${HOST}:${PORT} 可连接"
else
  fail "TCP ${HOST}:${PORT} 不可连接（安全组/防火墙/监听地址）"; echo "汇总：FAIL=${FAIL}"; exit 1
fi

# 2) 匿名 CONNECT
if mosquitto_pub -h "$HOST" -p "$PORT" -t "$TOPIC_SMOKE" -m "anon-${TS}" -q 0 -W 3 2>"$TMP/e2"; then
  pass "匿名 CONNECT（无凭据）"
else
  fail "匿名 CONNECT 被拒：$(tr -d '\n' <"$TMP/e2")"
fi

# 3) 设备形态：仅有 username，无密码
if mosquitto_pub -h "$HOST" -p "$PORT" -u "$TOKEN" -t "$TOPIC_SMOKE" -m "token-${TS}" -q 0 -W 3 2>"$TMP/e3"; then
  pass "设备形态 CONNECT（-u token 无密码）"
else
  fail "设备形态 CONNECT 被拒（设备将无法连接）：$(tr -d '\n' <"$TMP/e3")"
fi

# 4) 自定义主题回环
mosquitto_sub -h "$HOST" -p "$PORT" -u "$TOKEN" -t "$TOPIC_SMOKE" -C 1 -W 6 >"$TMP/s4" 2>"$TMP/e4" &
SUB4=$!
sleep 0.6
mosquitto_pub -h "$HOST" -p "$PORT" -u "$TOKEN" -t "$TOPIC_SMOKE" -m "roundtrip-${TS}" -q 1 -W 4 2>>"$TMP/e4"
wait $SUB4
if [ "$(cat "$TMP/s4")" = "roundtrip-${TS}" ]; then
  pass "主题回环 ${TOPIC_SMOKE}"
else
  fail "主题回环失败：recv='$(cat "$TMP/s4")' err='$(tr -d '\n' <"$TMP/e4")'"
fi

# 5) TB 主题回环（模拟设备 telemetry 上行）
mosquitto_sub -h "$HOST" -p "$PORT" -u "$TOKEN" -t "$TOPIC_TB" -C 1 -W 6 >"$TMP/s5" 2>"$TMP/e5" &
SUB5=$!
sleep 0.6
mosquitto_pub -h "$HOST" -p "$PORT" -u "$TOKEN" -t "$TOPIC_TB" -m "{\"smoke\":${TS}}" -q 1 -W 4 2>>"$TMP/e5"
wait $SUB5
if grep -q "smoke" "$TMP/s5"; then
  pass "TB 主题回环 ${TOPIC_TB}"
else
  fail "TB 主题回环失败：recv='$(cat "$TMP/s5")' err='$(tr -d '\n' <"$TMP/e5")'"
fi

echo "汇总：FAIL=${FAIL}"
[ "$FAIL" -eq 0 ] || exit 1
