# T5 V2：EN 硬复位下 RTC_NOINIT 票据保留测试计划

> 状态：**设计已批准；执行卡待审查；设备操作未批准**。
> 日期：2026-11-03。设备：仅 C8E4（MAC `b0:a7:32:28:c8:e4`）；65D8 不参与。
> 前置：T5 V1 已 PASS 并关闭（[验收报告](T5_V1_RTC保留验收报告_2026-11-03.md)）。

## 1. 验证问题

V1 已证明：`esp_restart()` 软件复位后 RTC_NOINIT 票据保留、消费一次、不重复触发。
V2 回答一个独立问题：

> **RTS/EN 硬复位后，RTC_NOINIT 票据是否仍保留，并能否被启动流程正确消费？**

V1 结论不得外推到 EN 硬复位；V2 也不验证掉电。

| 项目 | V1 | V2 |
| --- | --- | --- |
| 复位方式 | `esp_restart()`（`rst:0xc SW_CPU_RESET`） | RTS/EN 引脚硬复位（预期 `rst:0x1 POWERON_RESET`） |
| RTC 保留 | 已验证 | 待验证 |
| 首次票据消费 | 恰好一次 | 待验证 |
| 第二次复位不重复 | 已验证 | 待验证 |
| 正式固件恢复 | 已通过 | 待执行 |

## 2. 三个独立关口

| 关口 | 内容 | 当前状态 |
| --- | --- | --- |
| G1 设计批准 | 本测试计划（目标/边界/判据） | **已批准** |
| G2 执行卡审查 | 具体命令、复位真实性、证据采集、恢复路径 | **待审查** |
| G3 设备操作批准 | 烧 hooks、写 probe、EN 复位、恢复 | **未批准** |

G2 通过不等于 G3；V2 通过也不等于 T5/T6 批准。

## 3. 测试环境与前置

| 项 | 值 |
| --- | --- |
| 设备 | C8E4 `/dev/ttyUSB1`，MAC `b0:a7:32:28:c8:e4`，BLE `B0:A7:32:28:C8:E6` |
| 当前固件 | 正式 M1 `b0e6d6c7…`，静态 `192.168.1.50`，MQTT 在线 |
| hooks 固件 | `67913f19…`（与 V1 相同，含 `rtc_probe`/`boot_fail`/`reboot`） |
| 正式恢复固件 | `b0e6d6c7…`（完整 SHA 核对） |
| NVS | 不擦除、不写配置 |
| 65D8 | 不连接、不烧写、不修改，保持 DHCP |

## 4. 复位真实性（V2 核心）

- **必须**通过 RTS→EN 引脚复位：保持 DTR 使 GPIO0 为高，拉低 RTS 150ms 后释放；
- **禁止**用 `diag.m1hook action=reboot`（那是 `esp_restart`，属 V1 路径）；
- **禁止**以 `rst:0xc (SW_CPU_RESET)` 作为 V2 复位成功；V2 预期 `rst:0x1 (POWERON_RESET)`；
- 若使用 esptool `--after hard_reset`，需确认其确为 EN 复位，并在日志中核对复位原因；优先使用显式 RTS 脉冲，便于复现；
- 若复位原因无法确认，不得判 PASS。

## 5. 执行步骤（G3 批准后）

1. **烧 hooks**：`esptool write_flash 0x10000 67913f19…`，校验 `Hash of data verified`；
2. **写票据**：BLE 登录 → v0 `diag.m1hook {"action":"rtc_probe"}`；
   - 必须 `ok=true` 且 `reboot_required=true`；
   - 串口出现 `boot_ticket_armed stage=65535 count=0`；
   - 等待 ≥10s，无自动复位（`rst:0x`=0）；
3. **第一次 EN 硬复位**：RTS 脉冲；采集 ≥60s；
4. **判据（三选一）**：
   - **A 保留并消费**：复位原因 EN/POWERON、`ticket_seen=1 probe=1` 恰好 1 次、静态 `.50`、MQTT → 进入第 5 步；
   - **B 票据丢失**：无 `ticket_seen=1 probe=1`，但设备正常启动 → 记录为“RTC_NOINIT 在 EN 硬复位下不保证保留”，V2 记录边界，停止后续，进入恢复；
   - **C 不明确**：复位原因不符、日志缺失、设备失联 → 不得判 PASS，停止并恢复；
5. **第二次 EN 硬复位**（仅 A 成功后）：再 RTS 脉冲，采集 ≥40s；
   - `ticket_seen=1 probe=1` **0 次**（完整字符串匹配）；
   - 静态 `.50`、MQTT；无 abort/Guru/循环；
6. **恢复正式 M1**（无论 A/B/C）：
   - `esptool write_flash 0x10000 b0e6d6c7…`，完整 SHA 核对、`Hash of data verified`；
   - 启动确认 `1.2-a35eef9-dirty`、静态 `.50`、MQTT；
   - `diag.m1hook` → `unknown_method`。

## 6. 判据表

| 检查项 | 通过条件 |
| --- | --- |
| 复位真实性 | 第一次/第二次均为 EN 类复位（POWERON），非 `rst:0xc` |
| 票据写入 | `reboot_required=true` + `boot_ticket_armed` 日志 |
| 首次 EN 复位 | A：消费恰好一次 + 配置/MQTT 正常 |
| 第二次 EN 复位 | 完整 `ticket_seen=1 probe=1` 出现 0 次 |
| 恢复 | 正式版本/`.50`/MQTT/`unknown_method` |
| 异常 | 无 abort/Guru/重启循环 |

## 7. 失败与中止

- 复位原因不明确、日志采集不完整、设备无法连接：停止，不判 PASS；
- 票据丢失（B）：记录 RTC 保留边界，**不判定 M1 启动安全设计失败**；恢复正式固件后结束；
- 出现 abort/Guru/重启循环：立即停止，恢复正式固件，保留现场；
- 恢复失败：保留证据，不继续任何测试。

## 8. 证据要求

- hooks/正式 BIN 完整 SHA、烧写输出；
- BLE `rtc_probe` 原始响应；
- 两次 EN 复位前后完整串口日志（含复位原因行、`ticket_seen` 行）；
- 静态 IP/MQTT/BLE 状态回读；
- 恢复后的正式版本与 `diag.m1hook unknown_method`；
- 所有文件 SHA-256。

## 9. 边界

- 只验证 EN 硬复位；不验证掉电；
- 不进入 T5/T6 故障注入；
- 65D8 不参与；
- tryconnect 事件归属状态机冻结；
- 任何烧写/票据/复位操作需 G3 单独批准。
