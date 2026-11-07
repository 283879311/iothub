# V1 关口二 阶段一：写入 rtc_probe（PASS）

- 设备：C8E4，hooks 固件 `67913f19…`，静态 192.168.1.50
- 操作：BLE 登录 → v0 `diag.m1hook {"action":"rtc_probe"}`；未发送 reboot
- 串口并行采集约 14s

## 结果

| 断言 | 实测 |
| --- | --- |
| BLE 登录 | ok |
| rtc_probe 响应 | `{"ok":true,"action":"rtc_probe","reboot_required":true}` |
| 串口票据写入日志 | `boot_ticket_armed stage=65535 count=0` ×1 |
| 等待 ≥10s 自动重启 | 无（`rst:0x` = 0） |
| 10s 后 BLE 状态 | `sta_ip=192.168.1.50`、`sta_connected=true`、`mqtt_connected=true` |

## 当前状态

- RTC 中**存在未消费的 probe 票据**；
- 设备保持运行，未重启、未刷写、未改 NVS；
- 在阶段二批准前，不应复位或断电（否则票据会被消费，阶段一证据随之变化）。

## 证据

- `stage1_rtc_probe_response.json`
- `stage1_after_10s.json`
- `stage1_serial.log`
- `sha256_stage1.txt`
