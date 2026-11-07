# V1 关口二 阶段三：第二次软件复位与一次性验证（PASS）

- 设备：C8E4，hooks 固件，静态 192.168.1.50
- 操作：串口采集 → BLE 登录 → 发送一次 `reboot`；采集 45s
- 前提：阶段二已消费 probe 票据

## BLE 响应

```json
{"ok": true, "action": "reboot", "reboot_scheduled": true, "id": 2}
```

## 断言

| 检查项 | 实测 |
| --- | --- |
| reboot 响应 | `ok=true`、`reboot_scheduled=true` |
| 复位原因 | `rst:0xc (SW_CPU_RESET)` ×1 |
| `ticket_seen=1 probe=1` | **0 次**（宽泛 `ticket_seen` 也 0 次） |
| 静态配置 | `apply_begin mode=static` ×1、`.50` 配置日志 ×1 |
| GOT_IP | 192.168.1.50 |
| MQTT | connected |
| 异常 | 无 abort/Guru，45s 内仅 1 次复位 |
| BLE 回读 | 静态 `.50`、connected、has_ip、mqtt_connected、rssi_valid |

结论：probe 票据已被阶段二消费，第二次软件复位未再次触发，符合一次性语义。

## 当前状态

- 未恢复正式固件（阶段四未批准）；65D8 未触碰。
