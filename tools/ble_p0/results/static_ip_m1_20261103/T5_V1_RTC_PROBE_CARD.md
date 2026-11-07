# V1 软件复位 RTC 保留验证执行卡（C8E4，未执行）

> 目的：验证 `RTC_NOINIT` 票据在**软件复位（esp_restart）**后仍保留。
> 不注入真实故障、不执行 T5/T6。V2 EN 硬复位另行批准。

## 0. 设备选择

- 使用 **C8E4**（`/dev/ttyUSB1`，MAC `b0:a7:32:28:c8:e4`，BLE `B0:A7:32:28:C8:E6`）；
- 65D8 不参与，保持 DHCP；
- C8E4 当前：M1 正式 `b0e6d6c7…`，静态 `192.168.1.50`。

## 1. 构建与镜像

| 用途 | 文件/值 |
| --- | --- |
| hooks 固件（V1 用） | `build-hookstest/iothub.bin`，BIN `67913f191122ef85a180cee2f678b3631f64ba125c546d2c2978811ccc9c7f35`，ELF `ee280e32…` |
| 正式恢复固件 | `build/iothub.bin`，BIN `b0e6d6c7…`，ELF `66e781a5…` |
| 原始 NVS 备份 | `nvs_backup_1.bin`，SHA `62cc5747…682389`；page0 `5ab2b53c…`、page1 `ebdb5913…` |

新增测试入口（`APP_WIFI_FAULT_INJECT`，本地提交 `090b827`）：
- `diag.m1hook action=reboot` → `app_reboot_schedule()`，延迟 1.5s `esp_restart`，先回包；
- 响应语义：首次成功 `reboot_scheduled=true`；已有复位在途 `already_pending=true`；创建失败 `reboot_failed`。V1 只接受 `reboot_scheduled=true` 并继续，其他情况停止。
- 仅 BLE + token；正式构建不包含。

## 2. 前置复核（任一不符即停）

| 项 | 期望 |
| --- | --- |
| MAC | `b0:a7:32:28:c8:e4` |
| 当前固件 | M1 正式 `1.2-a35eef9-dirty`（BIN `b0e6d6c7…`） |
| 当前网络 | 静态 `192.168.1.50`，MQTT connected |
| NVS page0/page1 | `5ab2b53c…` / `ebdb5913…`（与备份一致） |
| 完整 NVS 16 KiB | `62cc5747…` |

## 3. 执行步骤

1. 再次读取 NVS 16 KiB，与备份逐字节比对；另外存一份 V1 前快照。
2. 刷 hooks 固件（仅 C8E4），确认启动；用 `diag.m1hook action=status` 确认测试入口可用。
3. BLE 发送 `rtc_probe`，期望 `reboot_required=true`；**确认设备未自动重启**（3s 后仍可查询/uptime 未归零）。
4. 记录当前启动日志起点，发送 `reboot`，期望 `reboot_scheduled=true`。
5. 采集 ≥30s 启动日志，断言：
   - `ticket_seen=1 probe=1` **恰好一次**；
   - Wi-Fi 仍为静态 `192.168.1.50`，MQTT connected；
   - 无 abort/Guru/重启循环。
6. 再次发送 `reboot`，采集 ≥30s，断言 **无** `ticket_seen=1 probe=1`（一次性已清除）。
7. 读回 NVS 16 KiB，与 V1 前快照逐字节比对（probe 不应写 NVS）。
8. 刷回正式 M1 `b0e6d6c7…`，重启确认静态 `.50` + MQTT；再读 NVS 比对。

## 4. 通过标准

- 第一次软件复位后 `ticket_seen=1 probe=1` 出现一次；
- 第二次软件复位后不再出现；
- NVS 全程未变；
- 设备最终恢复正式固件与静态 `.50`。

## 5. 失败处理

- 第一次复位后未出现 `ticket_seen=1 probe=1` → RTC 在软件复位下未保留，V1 FAIL；停止，刷回正式固件，不做 V2。
- 出现重复 probe、abort、重启循环、NVS 变化 → 立即停止并恢复备份。
- 无论结果，恢复正式固件 `b0e6d6c7…` 并确认 `.50`+MQTT。

## 6. 边界

- V1 只证明软件复位；V2（EN 硬复位）未批准。
- V1/V2 通过不代表 T5/T6 自动批准。
- 65D8 不参与；tryconnect 状态机冻结。
