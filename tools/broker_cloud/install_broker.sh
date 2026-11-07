#!/usr/bin/env bash
# 在【云主机】上执行（Ubuntu 22.04/24.04、Debian 12 验证）
# 作用：安装 mosquitto 作为 IoTHub 测试 broker，监听 0.0.0.0:21884，
#       覆盖默认 /etc/mosquitto/mosquitto.conf（不保留 1883 监听），
#       配 Restart=always + 2 分钟健康检查（连续 3 次失败才重启，避免抖动），
#       可选写 ufw 白名单。
#
# 用法：
#   sudo BROKER_PORT=21884 bash install_broker.sh
#   sudo BROKER_PORT=21884 ALLOW_CIDR=203.0.113.7/32 bash install_broker.sh   # 只放行现场出口 IP
#
# 幂等：可重复执行；端口被占用会直接退出，不覆盖他人服务。

set -euo pipefail

PORT="${BROKER_PORT:-21884}"
ALLOW_CIDR="${ALLOW_CIDR:-}"
CONF_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/mosquitto_iothub.conf"

[ "$(id -u)" -eq 0 ] || { echo "请用 sudo 执行"; exit 1; }
[ -f "$CONF_SRC" ] || { echo "缺少配置模板：$CONF_SRC"; exit 1; }

if ss -ltn 2>/dev/null | awk '{print $4}' | grep -q ":${PORT}\$"; then
  echo "端口 ${PORT} 已被占用，请换 BROKER_PORT"; exit 1
fi

echo "== 1/6 安装软件包"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq mosquitto mosquitto-clients >/dev/null

echo "== 2/6 停默认实例并写配置"
systemctl stop mosquitto 2>/dev/null || true
install -d -o mosquitto -g mosquitto /var/log/mosquitto
sed "s/^listener 21884 0.0.0.0/listener ${PORT} 0.0.0.0/" "$CONF_SRC" > /etc/mosquitto/mosquitto.conf
chown root:mosquitto /etc/mosquitto/mosquitto.conf
chmod 644 /etc/mosquitto/mosquitto.conf

echo "== 3/6 systemd 加固（自动重启 + 打开文件数）"
install -d /etc/systemd/system/mosquitto.service.d
cat > /etc/systemd/system/mosquitto.service.d/override.conf <<'EOF'
[Service]
Restart=always
RestartSec=3
LimitNOFILE=4096
EOF
systemctl daemon-reload
systemctl enable --now mosquitto

echo "== 4/6 健康检查（systemd timer，2 分钟一次，连续 3 次失败才重启）"
cat > /usr/local/sbin/iothub-broker-health.sh <<EOF
#!/usr/bin/env bash
# 探测 broker 存活：能收到 \$SYS/broker/uptime 即健康；连续 3 次失败重启
STATE=/run/iothub-broker-health.fail
if mosquitto_sub -h 127.0.0.1 -p ${PORT} -t '\$SYS/broker/uptime' -C 1 -W 3 >/dev/null 2>&1; then
  rm -f "\$STATE"
  exit 0
fi
n=\$(cat "\$STATE" 2>/dev/null || echo 0); n=\$((n+1)); echo "\$n" > "\$STATE"
if [ "\$n" -ge 3 ]; then
  logger -t iothub-broker "health failed \$n times -> restart mosquitto"
  systemctl restart mosquitto; rm -f "\$STATE"
fi
EOF
chmod 755 /usr/local/sbin/iothub-broker-health.sh

cat > /etc/systemd/system/iothub-broker-health.service <<'EOF'
[Unit]
Description=IoTHub test broker health probe
[Service]
Type=oneshot
ExecStart=/usr/local/sbin/iothub-broker-health.sh
EOF

cat > /etc/systemd/system/iothub-broker-health.timer <<'EOF'
[Unit]
Description=Run IoTHub broker health probe every 2 minutes
[Timer]
OnBootSec=2min
OnUnitActiveSec=2min
AccuracySec=10s
[Install]
WantedBy=timers.target
EOF
systemctl daemon-reload
systemctl enable --now iothub-broker-health.timer

echo "== 5/6 防火墙（可选）"
if [ -n "$ALLOW_CIDR" ]; then
  if command -v ufw >/dev/null 2>&1; then
    ufw allow from "$ALLOW_CIDR" to any port "$PORT" proto tcp
    ufw status | grep -E ":${PORT}|Status" || true
  else
    echo "未安装 ufw；请改用 nftables，或仅在云安全组做源 IP 白名单"
  fi
else
  echo "未指定 ALLOW_CIDR，跳过 ufw；请在云控制台安全组只放行现场出口 IP"
fi

echo "== 6/6 自检"
sleep 1
ss -ltn | grep ":${PORT}" || { echo "监听失败，查看 journalctl -u mosquitto -n 50"; exit 1; }
bash "$(dirname "${BASH_SOURCE[0]}")/smoke_mqtt.sh" 127.0.0.1 "$PORT"

echo
echo "完成。监听 0.0.0.0:${PORT}"
echo "设备侧：BLE mqttcfg 页 host=<本机公网IP> port=${PORT} use_tls=off token 保持原值"
