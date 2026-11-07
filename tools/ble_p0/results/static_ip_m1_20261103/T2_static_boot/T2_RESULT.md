# T2：合法静态 IP 正常启动（C8E4）

- 日期：2026-11-03
- 设备：C8E4，基础 MAC b0:a7:32:28:c8:e4（BLE 广播 B0:A7:32:28:C8:E6），/dev/ttyUSB1
- 构建：M1 正式（APP_WIFI_FAULT_INJECT OFF）；BIN b0e6d6c7… / ELF 66e781a5…
- 烧写前：设备为 pre-M1 `1.2-a35eef9`、DHCP、静态字段为旧 192.168.43.x 且 `sta_use_static=false`
- 执行：烧写 M1 正式（esptool 校验通过，NVS 未擦）→ DHCP 基线确认 → BLE v0 config.set → 自动重启进入 S1

## 写入参数与响应

```json
{"sta_use_static":true,"sta_static_ip":"192.168.1.50","sta_static_gateway":"192.168.1.1",
 "sta_static_netmask":"255.255.255.0","sta_static_dns1":"223.5.5.5","sta_static_dns2":"8.8.8.8"}
```
响应：`ok=true`，`changed=[sta_use_static, sta_static_ip, sta_static_gateway, sta_static_dns1]`（netmask/dns2 原值已相同），`saved=true`、`applied=false`、`restart_required=true`、`effect=reboot`；随后 `rst:0xc (SW_CPU_RESET)` 自动重启。

## S1 断言（PASS）

| 断言 | 期望 | 实测 |
| --- | --- | --- |
| 静态应用 | `apply_begin mode=static source=boot` | 1 次 |
| 静态参数 | 与新值一致 | `static ip configured: ip=192.168.1.50 gw=192.168.1.1 mask=255.255.255.0 dns1=223.5.5.5 dns2=8.8.8.8` |
| apply 成功 | `apply_cfg: started` + `net_config_state=static` | 各 1 次 |
| 关联 | `STA_CONNECTED: static ip mode` | 1 次 |
| GOT_IP | `192.168.1.50` | 1 次（连接后 12ms） |
| MQTT | connected | 1 次 |
| apply_failed / 救援 / 离线 / 启动超时 | 0 | 0 |
| abort / Guru / hook_ | 0 | 0 |
| 异常重启 | 无（仅 config.set 后 1 次 SW_CPU_RESET） | 符合预期 |

## 回读与地址归属

- BLE 回读：`sta_use_static=true`、`sta_static_ip=192.168.1.50`、gateway/mask/dns 与写入一致、`sta_ip=192.168.1.50`、`sta_connected=true`。
- `network.status`：`sta_ip=192.168.1.50`、`sta_has_ip=true`、`mqtt_connected=true`、`rssi_valid=true`。
- 主机 `ping 192.168.1.50` 通；ARP `lladdr b0:a7:32:28:c8:e4`，与设备基础 MAC 一致。

## 结论

T2 PASS。C8E4 当前运行 M1 正式固件并处于静态 IP（192.168.1.50）。未执行 T3、未做故障注入。
DNS 可解析性未单独验证（DNS1=223.5.5.5，网关 53 拒绝）；DNS 空值清理属 M2。

## 断连事件说明

- 日志中仅 1 次真实 `STA disconnected`（reason=8 ASSOC_LEAVE，t=7553ms），发生在 config.set 触发重启前的主动离开；随后 `MQTT stopping` 并 `rst:0xc`，属预期。
- S1 静态启动之后至采集结束（134s）无断连，MQTT 持续 ACK。
