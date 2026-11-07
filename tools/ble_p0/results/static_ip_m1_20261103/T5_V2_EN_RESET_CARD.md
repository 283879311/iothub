# T5 V2 执行卡草案：EN 硬复位下 RTC 票据保留（未批准、未执行）

> 状态：仅设计草案。烧写、写票据、复位均需单独批准。
> 目的：验证 `EN` 硬复位（RTS/EN 引脚复位，非 `esp_restart`）后 RTC_NOINIT 票据是否保留、是否一次性消费。

## 1. 与 V1 的区别

| 项 | V1（已 PASS） | V2（本卡） |
| --- | --- | --- |
| 复位方式 | `esp_restart()`，`rst:0xc (SW_CPU_RESET)` | RTS→EN 引脚硬复位（预期 `rst:0x1 POWERON_RESET` 或 EN 复位原因） |
| 目的 | 软件复位保留 | 硬复位保留边界 |
| 设备 | C8E4 | C8E4（65D8 不参与） |

## 2. 前置

- V1 已 PASS；C8E4 当前正式 M1 + 静态 `.50` + MQTT；
- hooks BIN `67913f19…`（与 V1 相同）；
- 正式恢复 BIN `b0e6d6c7…`；
- NVS 不擦除、不写配置。

## 3. 步骤（批准后）

1. 复核 MAC/正式固件/静态 `.50`/目标 BIN SHA；
2. 烧 hooks app（`0x10000`，esptool 校验）；
3. BLE 登录 → `rtc_probe`，确认 `reboot_required=true`、串口 `boot_ticket_armed stage=65535 count=0`、10s 无自动复位；
4. **硬复位**：通过 RTS/EN 引脚复位（pyserial `RTS` 脉冲或 esptool `--after hard_reset`），不是 `esp_restart`；
5. 采集 ≥60s：
   - 复位原因应为 EN/POWERON 类，不是 `rst:0xc`；
   - `ticket_seen=1 probe=1` 恰好 1 次；
   - 静态 `.50`、MQTT；
   - 无 abort/Guru/循环；
6. 再次 EN 硬复位，采集 ≥40s：
   - `ticket_seen=1 probe=1` 0 次；
   - 静态 `.50`、MQTT；
7. 恢复正式 M1（app `0x10000`），确认 `1.2-a35eef9-dirty`、`.50`、MQTT、`diag.m1hook` → `unknown_method`。

## 4. 判据

| 项 | 预期 |
| --- | --- |
| 首次 EN 复位 | 票据保留并消费一次（若丢失，记录为 RTC 保留边界） |
| 第二次 EN 复位 | 不重复触发 |
| 配置/网络 | 静态 `.50`、MQTT 正常 |
| 异常 | 无 abort/Guru/重启循环 |

若 EN 复位后票据丢失：记录为“RTC_NOINIT 在 EN 硬复位下不保证保留”，**不判定 M1 启动安全设计失败**；是否调整测试注入方案另行评审。

## 5. 边界

- 只验证 EN 硬复位，不验证掉电；
- 不进入 T5/T6 故障注入；
- 65D8 不参与；
- 所有设备操作需单独批准。
> 正式计划已归档：[docs/T5_V2_EN硬复位测试计划.md](../../../docs/T5_V2_EN硬复位测试计划.md)；本文件为执行卡工作稿。
