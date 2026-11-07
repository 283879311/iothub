# V1 关口二 阶段四：恢复正式 M1（PASS）

- 设备：C8E4，MAC b0:a7:32:28:c8:e4
- 烧写：`esptool.py -p /dev/ttyUSB1 write_flash 0x10000 build/iothub.bin`
- 目标 SHA-256：`b0e6d6c7e1a5ba5271627d4e13aa0fc22b4b74c9517f7a11a7cab37e941ef5bf`（完整值核对一致）
- esptool：Wrote 1156464 bytes，`Hash of data verified`
- 范围：仅 app 0x10000；未写 bootloader/partition/otadata；未擦 NVS

## 启动（65s）

| 检查项 | 实测 |
| --- | --- |
| App version | `1.2-a35eef9-dirty` |
| `apply_begin mode=static` | 1 |
| 静态 IP | 192.168.1.50（配置日志 + GOT_IP） |
| MQTT | connected |
| rst | 1 次（采集用 DTR/RTS 冷复位） |
| abort/Guru | 0 |

说明：烧写后首次自动复位采集起点偏晚（仅 2072B，从 MQTT 开始），因此另做一次冷复位重新采集完整启动；两次均为正式固件启动，无异常。

## BLE 回读

```json
{"hook": {"ok": false, "error": "unknown_method", "id": 2},
 "config": {"sta_use_static": true, "sta_static_ip": "192.168.1.50", "sta_ip": "192.168.1.50"},
 "status": {"sta_ip": "192.168.1.50", "sta_connected": true, "sta_has_ip": true,
            "mqtt_connected": true, "rssi_valid": true}}
```

`diag.m1hook` 返回 `unknown_method`，确认测试入口未进入正式构建。

## 结论

阶段四 PASS。C8E4 已恢复正式 M1 固件 + 静态 192.168.1.50 + MQTT。
