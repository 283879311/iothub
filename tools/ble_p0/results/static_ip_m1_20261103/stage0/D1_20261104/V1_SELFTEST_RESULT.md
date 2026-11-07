# v1-only BLE 命令自测结果

> 日期：2026-11-04
> 设备：C8E4（BLE `B0:A7:32:28:C8:E6`）；65D8 未操作。
> 固件：v1-only BIN `a792158d93b98c3e86e3ffdd2e292c00b1f201be0ba9296bf7916c69ce9f245e`
> 状态：**BLE v1 自测 PASS；MQTT v1 实机注入未执行；D1/D2 旧基线失效。**

## 1. 固件/烧写

- 仅写 app `0x10000`，`Hash of data verified`。
- Image size：1,159,344 bytes；App version `1.2-a35eef9-dirty`；ELF SHA `39940aa2e5b289c0a5ff3c2f5307fda6991c85cce6987962652edee0d022bfa9`。
- `device.info`：`protocol_versions=["1.0"]`、`build_variant="mqtt_net_txn=on"`。
- 启动后静态 `.50`、CMCC-502、TB `139.9.93.245:21883`、MQTT 在线。
- NVS 预烧快照 SHA-256：`3f8796f571edb11cc2105fdac677c2809539d4ae206d7b677135a713ca90e88e`（原文未归档）。

## 2. BLE v1 自测

| 项 | 结果 | 证据 |
| --- | --- | --- |
| `auth.login` + token | PASS | `v1_selftest3.log` |
| `device.info` | PASS：仅 1.0，`mqtt_net_txn=on` | `v1_selftest3.json` |
| `device.status` | PASS | 同上 |
| `gpio.status` | PASS | 同上 |
| `uart.runtime` | PASS | 同上 |
| `config.get` | PASS：静态 `.50`、TB host 新地址 | 同上 |
| `io.get` | PASS | 同上 |
| `network.status` | PASS：CMCC-502/.50/MQTT | 同上 |
| `network.profile.list` | PASS：2 profiles | 同上 |
| `config.set` 未知 patch | PASS：success/changed=[]/effect=none | 同上 |
| `config.set` network no-op（`net_mode=sta`） | PASS：BLE v1 已允许 network 域，未重启 | 同上 |
| `network.apply` fake id | PASS：not_found | 同上 |
| `network.cancel` fake id | PASS：not_found | 同上 |
| `ip_mode=static` | PASS：invalid_argument | 同上 |
| `allow_leave_static` | PASS：invalid_argument | 同上 |
| 静态模式 + 不同 SSID | PASS：`static_ip_pinned`，未创建 operation | 同上 |
| `network.tryconnect` 当前 SSID（错误口令） | PASS：accepted → connecting；cancel → cancelled；未 apply | 同上 |
| `network.scan` | PASS：3 APs | 同上 |
| BLE v0 `device.status` | PASS：`unsupported_version`，id=701 | 同上 |
| BLE v0 `config.set` | PASS：`unsupported_version`，id=701 | 同上 |

### BLE v1 静态 IP 写回/恢复

- `.50 → .51`：`config.set` 返回 `success/changed:[sta_static_ip]/effect:reboot`；重启后 `network.status.sta_ip=192.168.1.51`、MQTT 在线。
- 恢复 `.50`：同样 `effect:reboot`；重启后 `sta_ip=192.168.1.50`、MQTT 在线。
- SSID `CMCC-502`、口令未改；未改 `net_mode`。
- 证据：`v1_static_write_test.json`。

### BLE v1 apply 成功路径

- 当前 SSID 正确口令：`tryconnect` accepted → connecting → got_ip（约 3s）→ `apply` 返回 `success/saved:true/applied:true`。
- `operation.get` 终态 `success`；静态 `.50` 保留、CMCC-502、MQTT 在线；未切换网络。
- 证据：`v1_apply_success.json`。

未执行：

- MQTT v1 实机注入（无原始 v1 发布端）。
- `auth.admin.set`（避免改密/失效 token）。
- `network.reconnect`（会重启 Wi-Fi）。
- MQTT v1：TB REST/tenant 会改写为 v0，且设备 token 自发布不路由；需要原始 v1 发布端。

## 3. 当前状态

- 设备运行 v1-only BIN；静态 `.50`、TB 新 host、MQTT 在线。
- v0 外部入口已关闭；旧 stage0/D1/D2 结果不再适用于该 BIN。
- 未恢复正式 M1；未操作 65D8；未提交/推送代码。

## 4. 证据

- `v1_flash2.log`：最终 v1 BIN 烧写/校验
- `v1_boot2.log`：启动串口
- `v1_selftest3.json`：BLE v1 原始响应
- `v1_selftest3.log` / `.err`：BLE 会话日志（token 脱敏）
- `v1_nvs_before.sha256`：NVS 快照哈希
- `v1_static_write_test.json`：v1 静态 IP 写入/恢复原始响应
- `v1_apply_success.json`：v1 tryconnect/got_ip/apply 成功路径原始响应
