# V1 关口二 阶段二：软件复位与首次票据消费（PASS）

- 设备：C8E4，hooks 固件，静态 192.168.1.50
- 操作：串口采集已开启 → BLE 登录 → 发送一次 `diag.m1hook {"action":"reboot"}`
- 采集：70s（覆盖复位与启动）

## BLE 响应

```json
{"ok": true, "action": "reboot", "reboot_scheduled": true, "id": 2}
```

## 首次启动断言

| 检查项 | 实测 |
| --- | --- |
| 复位原因 | `rst:0xc (SW_CPU_RESET)` ×1（另有 1 条复位前 `tcp_read error`，为连接被复位中断，非复位事件） |
| RTC 票据消费 | `ticket_seen=1 probe=1` **恰好 1 次**（t=1324ms） |
| 静态配置 | `static ip configured: ip=192.168.1.50 …` ×1；`net_config_state=static` ×1 |
| GOT_IP | 192.168.1.50（连接后 11ms） |
| MQTT | connected |
| 异常 | 无 abort、无 Guru、70s 内仅 1 次复位 |
| BLE 回读 | `sta_ip=192.168.1.50`、`sta_connected=true`、`sta_has_ip=true`、`mqtt_connected=true`、`rssi_valid=true` |
| 配置回读 | `sta_use_static=true`、`sta_static_ip=192.168.1.50` |

## 当前状态

- RTC probe 票据已被消费；设备正常运行，静态 `.50` + MQTT；
- 未执行第二次复位（阶段三未批准）；未恢复正式固件（阶段四未批准）；
- 65D8 未触碰。

## 证据

- `stage2_reboot_response.json`
- `stage2_serial.log`
- `stage2_after_boot.json`
- `sha256_stage2.txt`
