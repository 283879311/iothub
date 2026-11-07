# V1 关口二执行卡：rtc_probe 写入 + 软件复位一次性验证（未执行）

> 前提：关口一 PASS；C8E4 当前 hooks 固件 `67913f19…`，静态 192.168.1.50，MQTT 在线。
> 设备：C8E4 `/dev/ttyUSB1`，MAC `b0:a7:32:28:c8:e4`，BLE `B0:A7:32:28:C8:E6`；65D8 不参与。

## 1. 阶段一：写入 probe 票据（不重启）

1. BLE `auth.login`（密码经环境变量，不落盘）；
2. v0 `diag.m1hook`，params `{"action":"rtc_probe"}`；
3. 必须收到 `ok=true` 且 `reboot_required=true`；
4. 等待 ≥10s，确认**未自动重启**：
   - 串口无 `rst:0x`；
   - BLE `network.status` 仍可查询，uptime 未归零；
5. 串口日志应出现（无故障注入）：
   - `boot_ticket_armed stage=65535 count=0`。

失败：无响应 / `reboot_required` 缺失 / 自动重启 → 停止，进入恢复流程。

## 2. 阶段二：软件复位（esp_restart）

1. 先启动串口采集（≥60s，115200）；
2. BLE 发送 v0 `diag.m1hook`，params `{"action":"reboot"}`；
3. **只接受** `ok=true` + `reboot_scheduled=true`；
   - 收到 `already_pending=true`、`reboot_failed`、超时或响应缺失 → 停止，不假定已复位；
4. 期望 1.5s 内 `esp_restart`：串口出现 `rst:0xc (SW_CPU_RESET)`（不是 POWERON）；
5. 启动日志断言：
   - `ticket_seen=1 probe=1` **恰好 1 次**；
   - `apply_begin mode=static`、`static ip configured: 192.168.1.50`、GOT_IP `.50`、MQTT connected；
   - 无 abort/Guru/重启循环；
6. BLE 回读 `config.get`/`network.status`：静态 `.50`、MQTT 在线。

## 3. 阶段三：第二次复位（一次性验证）

1. 再次发送 `reboot`，只接受 `reboot_scheduled=true`；
2. 采集 ≥40s；
3. 断言：
   - `ticket_seen=1 probe=1` **0 次**；
   - 启动仍为静态 `.50`、MQTT connected；
   - 不得把其他日志误判为 probe（仅匹配完整字符串 `ticket_seen=1 probe=1`）。

## 4. 阶段四：恢复正式 M1 固件

1. 读取当前 app 描述符确认 `1.2-afac9cb-dirty`；
2. 用 esptool 写回已验收的正式 M1 app（仅 `0x10000`）：

```bash
esptool.py -p /dev/ttyUSB1 write_flash 0x10000 \
  /home/tc/esp/workspace/iothub/build/iothub.bin
```

3. 期望 BIN `b0e6d6c7…`，esptool `Hash of data verified`；
4. 启动采集 ≥30s：
   - `App version: 1.2-a35eef9-dirty`；
   - 静态 `.50`、MQTT connected；
   - 无 abort/Guru；
5. BLE 登录后 v0 `diag.m1hook` 应返回 `unknown_method`（正式构建无测试入口）；
6. 记录 NVS 逻辑配置（`config.get`），不做跨启动原始 NVS 比对（启动期 Wi-Fi 会写 NVS）。

## 5. 失败/中止与恢复

| 中止点 | 现场状态 | 恢复动作 |
| --- | --- | --- |
| probe 写入失败/无响应 | RTC 无票据或未知 | 停止，BLE 查状态；必要时重试一次 probe |
| probe 写入成功但未发送 reboot | RTC 有票据 | 先执行一次受控 `reboot` 消费票据，再恢复正式固件；或直接刷正式固件（正式构建不消费票据，但残留票据需记录） |
| reboot 响应失败 | 票据可能已消费/未消费 | 停止，采集日志确认是否出现 `ticket_seen`；未出现则按上一条处理 |
| 首次启动无 `ticket_seen=1 probe=1` | 票据未保留或已消费 | V1 FAIL；停止，恢复正式固件，不进入 V2 |
| 首次启动重复 probe | 一次性失效 | 立即停止，保留日志，恢复正式固件 |
| 出现 abort/重启循环 | 系统异常 | 立即恢复正式固件；若无法恢复，保留现场并报告 |

恢复统一以“刷回正式 M1 app + 启动确认 `.50`/MQTT”为完成条件。

## 6. 边界

- 只操作 C8E4；65D8 不参与；
- 不写 NVS 配置、不擦 flash、不改静态参数；
- V1 只验证软件复位；V2 EN 硬复位未批准；
- V1 通过不代表 T5/T6 自动批准。
