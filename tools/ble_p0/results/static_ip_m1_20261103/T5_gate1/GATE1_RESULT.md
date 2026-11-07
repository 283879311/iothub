# V1 关口一：hooks 固件烧写与核验（PASS）

- 日期：2026-11-03
- 设备：C8E4，MAC b0:a7:32:28:c8:e4，/dev/ttyUSB1
- 目标：hooks 构建，BIN `67913f19…`，仅写 app 0x10000
- 烧写前：正式 M1 `1.2-a35eef9-dirty`，静态 192.168.1.50

## NVS A/B（同一 bootloader 会话）

| 项 | SHA-256 |
| --- | --- |
| A（写前） | `bb3628c5e9bbd6e3f80a4d063a7ab7a53df89344f655546956e40f6420914807` |
| B（写后） | `bb3628c5e9bbd6e3f80a4d063a7ab7a53df89344f655546956e40f6420914807` |
| 比较 | 逐字节一致（cmp 通过） |

说明：NVS 在每次应用启动时会被 Wi-Fi 栈写入（page 状态/内容变化已验证），因此跨启动对比不成立；A/B 在同一 bootloader 会话内完成，只隔离“写 app”动作。

## 烧写

- `write_flash 0x10000`：Wrote 1160496 bytes，esptool `Hash of data verified`；
- 未擦 NVS、未写 otadata/bootloader/partition。

## 启动（40s）

| 断言 | 实测 |
| --- | --- |
| App version | `1.2-afac9cb-dirty`（hooks 构建） |
| `apply_begin mode=static source=boot` | 1 |
| `static ip configured: ip=192.168.1.50 …` | 1 |
| `net_config_state=static` | 1 |
| GOT_IP | 192.168.1.50 |
| MQTT connected | 1 |
| rst | 仅 1 次 POWERON_RESET |
| abort/Guru | 0 |

## BLE 核验

- `auth.login` 成功；
- v0 `diag.m1hook action=status` → `{"ok": true, "action": "status"}`；
- `config.get`：`sta_use_static=true`、`sta_static_ip=192.168.1.50`、`sta_ip=192.168.1.50`；
- `network.status`：`sta_connected=true`、`mqtt_connected=true`。

## 结论

关口一 PASS。C8E4 当前运行 hooks 固件（BIN `67913f19…`），静态 `.50`、MQTT 在线。
未写 `rtc_probe`、未执行 `reboot`、未进入关口二；65D8 未触碰。
