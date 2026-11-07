# T1：DHCP 正常启动（65D8）

- 日期：2026-11-03
- 设备：65D8，MAC 10:97:bd:d3:65:d8，串口 /dev/ttyUSB0
- 构建：正式（APP_WIFI_FAULT_INJECT OFF）
  - BIN sha256 b0e6d6c7e1a5ba5271627d4e13aa0fc22b4b74c9517f7a11a7cab37e941ef5bf
  - ELF sha256 66e781a5dd2305a95b3ca0ffc49eda2332ed6f0072189fc10a692af872da8a6f
- 前置确认：烧写前日志显示 DHCP（STA_CONNECTED: dhcpc restarted；GOT_IP；MQTT connected），未改 NVS/未擦除 flash
- 烧写：idf.py -p /dev/ttyUSB0 flash，esptool "Hash of data verified"；NVS 未修改
- 采集：冷启动（POWERON_RESET）后 120s 串口日志

## 断言结果（PASS）

| 断言 | 期望 | 实测 |
| --- | --- | --- |
| 启动次数 | 1 | 1（rst:0x1 POWERON_RESET ×1，[boot-1] ×1） |
| apply_begin | 1（mode=dhcp source=boot） | 1 |
| apply_cfg: started | 1 | 1（gen=1） |
| net_config_state | dhcp | dhcp |
| GOT_IP | ≥1 | 1（192.168.1.45） |
| MQTT connected | 1 | 1 |
| apply_failed | 0 | 0 |
| offline_gate_set / pending / stop_fail | 0 | 0 |
| apply_start_timeout | 0 | 0 |
| abort / Guru Meditation | 0 | 0 |
| hook_* 日志 | 0（正式构建） | 0 |
| STA disconnected | 0 | 0 |
| MQTT 错误/重连 | 0 | 0 |

结论：T1 PASS（串口 + MQTT 证据）。BLE 未验证（本轮未提供口令，按约定非必过项）。
未执行 T2、未执行故障注入、未触碰 C8E4。
