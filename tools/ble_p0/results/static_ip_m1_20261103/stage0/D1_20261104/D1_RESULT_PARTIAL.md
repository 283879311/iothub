# 阶段 0 D1 设备测试结果（部分完成，MQTT 项阻塞）

> 日期：2026-11-04
> 设备：C8E4（BLE `B0:A7:32:28:C8:E6`，ESP MAC `b0:a7:32:28:c8:e4`）；65D8 未连接、未烧写。
> 固件：生产 BIN `4ddc7d9b5403b8f0c1fcb2590c238b62aa14b9cbf9fae3099992c69b7e28a352`
> 状态：**BLE 子集 PASS；MQTT v1 子集 BLOCKED（缺 broker 凭据）；D1 未完成，不判定通过。**

## 1. 烧写与基线

- 烧写：仅 app `0x10000`，`Hash of data verified`，见 `flash.log`。
- NVS 预烧快照：16,384 bytes，SHA-256 `f9ee17252bf7ea8c7d6752e0aa7b38121232acddec3a82a0a12e490d12b520f3`；原文未归档、已删除。
- 启动后：静态 `192.168.1.50`；CMCC-502；MQTT 在线；`reset_reason=poweron`。
- 串口早期启动头未捕获，`boot_serial.log` 从 WiFi 阶段开始；不作为复位原因/启动完整性判据。

## 2. 测试结果

| ID | 结果 | 证据 |
| --- | --- | --- |
| D1-00 BLE 登录 | PASS | `d1_ble_run.log` token 已脱敏 |
| D1-01 `device.info.build_variant` | PASS：`mqtt_net_txn=off` | `d1_ble_results.json` / `d1_final.json` |
| D1-02 BLE v1 既有路径 | PASS：device.info/status/config.get/network.status 正常 | `d1_ble_results.json` |
| D1-03 外部 `.async` 直调 | PASS：tryconnect/apply/cancel 三个均 `guard_rejected` | `d1_async_all.json` |
| D1-04 MQTT v1 生产关闭 | **BLOCKED**：无 broker 凭据 | — |
| D1-05 MQTT v1 network.status 原始 JSON | **BLOCKED**：无 broker 凭据；BLE 原始 status 已存 | `d1_ble_results.json` |
| D1-06 事务建立/取消 | PASS（门控用途）：accepted → connecting；cancel → cancelled；未 apply | `d1_ble_results.json` |
| D1-07 MQTT config.set active | **BLOCKED**：无 broker 凭据；BLE v0 等价门控 PASS：`busy` | `d1_ble_results.json` |
| D1-08 auth.admin.set active | PASS：`busy`；未改密，后续 token 仍有效 | `d1_ble_results.json` |
| D1-09 profile.delete active（`__gate_probe`） | PASS：`busy`；不存在的 SSID 未被删除 | `d1_ble_results.json` |
| D1-10 cancel 后 config.set | PASS：成功，未知字段无配置变化（`changed=[]`） | `d1_ble_results.json` |
| D1-11 last_profile | **SKIP**：profile 数=2（CMCC-502/TQ2018），不制造删除条件 | `d1_ble_results.json` |

补充：D1-09 按批准意见使用不存在 SSID `__gate_probe`，门控失效时最多 `not_found`，不会真删 profile。

### D1-06 成功路径说明

- D1-06 的门控用途已满足：事务 accepted → connecting → cancel → cancelled；未 apply、未改配置。
- 另用 `/home/tc/.iothub_test.env` 中的口令对当前 SSID 做了一次 tryconnect，结果为 `failed/wrong_password`（约 4.5s）；该口令很可能属于另一测试 SSID，不是 CMCC-502。此失败不是固件结论，但说明“正确口令 → got_ip”未在本次 D1 验证。
- 失败后设备仍为 CMCC-502、静态 `.50`；MQTT 短暂 `waiting_sta_ip`，12s 后恢复在线；未写配置。
- 证据：`d1_tryconnect_success.json`、`d1_after_failed_try.json`。

## 3. 关键原始响应

- 事务前状态：`connecting`
- 事务中：`auth.admin.set` → `busy`；`profile.delete` → `busy`；BLE v0 `config.set` → `busy`
- cancel 后：`cancelled`；`config.set {"__gate_probe":1}` → `success:true, changed:[], effect:none`
- `device.info.build_variant`：`mqtt_net_txn=off`
- 最终网络：静态 `.50`、CMCC-502、MQTT 在线、trial idle

## 4. 证据文件

- `flash.log`：烧写/校验输出
- `nvs_before.sha256`、`nvs_before.len`：NVS 快照哈希/长度（无原文）
- `boot_serial.log`：启动后串口 70s
- `d1_ble_results.json`：BLE D1-01/02/06/08/09/10/11 + 门控原始响应
- `d1_async_all.json`：三个外部 `.async` 拒绝响应
- `d1_final.json`：测试后只读状态
- `d1_ble_run.log` / `d1_ble_run.err` / `d1_async_all.log`：BLE 会话日志（token/口令脱敏）
- `d1_tryconnect_success.json`：当前 SSID 的失败尝试（wrong_password；未写配置）
- `d1_after_failed_try.json`：失败后恢复状态（静态 .50、MQTT 在线）
- `d1_final_after_mqtt.json`：第一次 MQTT 认证尝试后的只读状态（仍 online）
- `d1_final_after_mqtt2.json`：第二次 token 尝试后的只读状态（仍 online）
- `d1_mqtt_off.json`：设备 token 连接成功但 RPC 发布全部 timeout
- `serial_mqtt_probe.log`：发布 RPC 期间串口 25s；无对应 MQTT RX
- `d1_after_mqtt_rpc.json`：RPC 尝试后的只读状态（仍 online）

## 4.1 新 TB 服务器切换（D1 执行中例外，已获授权）

- 已将 C8E4 `mqtt_host` 从 `117.131.99.2` 改为 `139.9.93.245`，端口 21883；token 未变，静态 `.50` 保持。
- `config.set` 返回 `success/changed:[mqtt_host]/effect:subsys`；串口显示 `uri=mqtt://139.9.93.245:21883`、`MQTT connected`。
- 设备 token 对新服务器认证 CONNACK 0。
- 但用该设备 token 向新服务器 `v1/devices/me/rpc/request/<id>` 发布 v1 `network.status`，设备串口仍无 `MQTT RX`；设备 token 不能替代 TB 服务端 RPC 权限。
- 证据：`d1_switch_mqtt_host.json`、`serial_mqtt_switch.log`、`d1_mqtt_off_newtb.json`。
- 这是 D1 卡范围外的配置变更；旧 host 记录在证据中，回退需另行批准。

## 4.2 方案 1 决策与 D1/D2 范围修正

- 已选择 **TB server-side MQTT 用户**；设备 token 不再用于注入 RPC。
- D1 生产 BIN 只验证：`network.status` 成功、`network.scan/tryconnect/apply/cancel` 均 forbidden、active BLE 事务期间 MQTT `config.set` busy、cancel 后释放。
- `network.scan` 不在阶段 0 MQTT v1 白名单；D1 期望 `forbidden`，不是长响应成功。
- MQTT tryconnect/apply/cancel 的**成功路径与 owner/cancel 矩阵属于 D2 测试 BIN**，不能放进 D1 生产 BIN。
- 服务端 MQTT 身份就绪前，不再尝试设备 token 注入 RPC，不发送控制命令。

## 4.3 TB 服务端 RPC/JWT 探测结果

- README 提供的 tenant 账号可登录 TB REST；JWT 有效。
- C8E4 在 TB 上匹配为 **Test Device B1**（NVS token 与 TB credentials 一致）。
- 对 B1 调 `POST /api/rpc/twoway/{deviceId}`：
  - v0 body `{"method":"network.status","params":{}}` → HTTP 200，返回 v0 status；
  - 带 v1 envelope 字段的 body 也 → HTTP 200，但返回仍是 v0 status 形状，证明 TB RPC API 改写了 payload，未原样下发 `version/cmd/args`。
- tenant 账号直接登录 MQTT：CONNACK 5；设备 token 自发布：设备无 `MQTT RX`。
- 结论：README 只提供 tenant REST/JWT，没有 **broker 级 server-side MQTT 发布身份**；方案 1 尚缺这一项，D1-04/05/07 仍不能执行。
- 证据：`tb_rpc_probe.json`。

## 5. 阻塞项

MQTT v1 测试需要“服务端注入 RPC”的能力，不是只有设备 token 就够：

- 匿名连接：CONNACK rc=2。
- 两次外部提供的 token：CONNACK 5（not authorized）。
- 按测试授权从设备 NVS 临时读取的测试 token：CONNACK 0，认证成功；token 未打印、未写报告、未归档。
- 但用该 token 向 `v1/devices/me/rpc/request/<id>` 和 `iothub/rpc` 发布 v1 `network.status` 后，串口 25s 内没有对应 `MQTT RX`；设备只收到 attributes 下行。说明 TB broker/ACL 不允许设备 token 自注入服务端 RPC。
- 因此 D1-04/05/07 仍不能判定；未观察到设备执行任何 MQTT 控制命令，未改配置。

需要以下之一才能继续 D1：

1. 需要 **broker 级 server-side MQTT 发布身份**（不是设备 token、不是 tenant REST 账号）：可向 `v1/devices/me/rpc/request/+` 发布原始 v1 envelope，并订阅 response；或由 TB 规则链/网关提供等价原始下发能力。
2. TB REST/JWT 已证明会改写为 v0 `method/params`，不能用于 v1 envelope 验证。
3. 若无法提供 broker 级身份，D1 只能保留 BLE 子集；MQTT v1 转 D2/硬件侧，D1 不判通过。

## 6. 当前设备状态

- 仍运行 D1 生产 BIN；静态 `.50`、MQTT 在线。
- 未恢复正式 M1；未申请 D2；未操作 65D8。
