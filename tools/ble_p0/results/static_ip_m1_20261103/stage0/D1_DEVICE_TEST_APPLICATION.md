# 阶段 0 D1 设备测试申请卡（待批准）

> 状态：**申请审查中；未连接、未烧写、未操作设备。**
> 设备：仅 C8E4（MAC `b0:a7:32:28:c8:e4`）；65D8 不参与。
> D1 目标：验证生产 BIN 的“关闭路径确实关闭 + 既有 BLE 路径无回归 + 配置门控可观测”。
> D2（MQTT owner/cancel 开闸矩阵）另行申请，本卡不包含。

## 1. 固件与基线

| 项 | 值 |
| --- | --- |
| 待烧 D1 BIN | `/tmp/iothub_stage0_build/iothub.bin` |
| 生产 BIN SHA-256 | `4ddc7d9b5403b8f0c1fcb2590c238b62aa14b9cbf9fae3099992c69b7e28a352` |
| 构建变体 | `mqtt_net_txn=off`（device.info 可读） |
| 烧写地址 | app `0x10000`（仅此区域） |
| NVS | 不擦除、不写网络配置；烧写前记录 16 KiB 快照（仅取证，不比较跨启动字节） |
| 当前基线 | C8E4 运行正式 M1 `b0e6d6c7…`，静态 `192.168.1.50`，MQTT 在线 |
| 65D8 | 不连接、不烧写、不修改 |

D1 不恢复正式 M1；D2 结束后另行按 G3c/设备恢复流程处理。

## 2. D1 测试项

| ID | 测试 | 期望 |
| --- | --- | --- |
| D1-00 | 预检：BLE v1 `auth.login`（当前管理员口令，环境读取，不落日志） | 登录成功；失败则停止，不执行 D1-08 |
| D1-01 | `device.info`（BLE v1） | `build_variant="mqtt_net_txn=off"`；版本/静态 IP 正常 |
| D1-02 | BLE v1 既有路径：`device.info`/`device.status`/`config.get`/`operation.get` | 正常返回；无回归 |
| D1-03 | BLE v0 原始 `.async` 直调：`network.tryconnect.async`/`apply.async`/`cancel.async` | `guard_rejected`（策略层拒绝），不创建 operation/trial |
| D1-04 | MQTT v1 生产关闭：`network.tryconnect`/`apply`/`cancel` | `forbidden`（或映射后的 `guard_rejected`），不创建 operation/trial |
| D1-05 | MQTT v1 `network.status` 原始响应 | 正常返回；保存原始 JSON，供 D2 与 BLE v1 对照 |
| D1-06 | BLE v1 `network.tryconnect` 到当前正式 SSID（口令仅从环境/现场读取，不落日志），成功后 `network.cancel` | accepted → 可取消；不 apply，不改配置 |
| D1-07 | 事务 active 时：MQTT `config.set {"__gate_probe":1}`（未知字段，无副作用） | `busy` |
| D1-08 | 事务 active 时：BLE v1 `auth.admin.set`（old=new，口令来自环境，不落日志） | `busy`；未改密、未作废 token |
| D1-09 | 事务 active 时：`network.profile.delete` 任意 SSID | `busy`；不删除 |
| D1-10 | 取消/结束事务后：MQTT `config.set {"__gate_probe":1}` | 不再是 `busy`；未知字段被忽略，无配置变化 |
| D1-11 | `network.profile.list` 计数；若恰好 1 个有效 profile，普通删除该 profile | `last_profile`，profile 仍在 |
| D1-12 | 若 profile 数 >1：不执行最后 profile 删除；记录“条件不满足，未测” | 不删除任何 profile |

说明：

- D1-07/08/09 共用同一 active 事务窗口；事务来自 D1-06 的 `network.tryconnect`，在 cancel 前执行。
- `auth.admin.set` 的 old/new 使用环境中的当前管理员口令；**不记录明文**，不写报告。若返回 `busy`，不得改密；若意外成功，立即停止并按现场口令恢复流程处理。
- D1-11 只在 profile 数恰好为 1 时执行；否则跳过，不能通过删除其他 profile 来制造条件。
- MQTT v1 生产关闭路径只验证 `forbidden`，不尝试打开开关。

## 3. 执行顺序

1. 预检：确认 C8E4 当前状态、端口 by-id、NVS 快照、生产 BIN SHA；
2. 仅 `write_flash 0x10000` 写 D1 BIN，`Hash of data verified`；
3. 启动并采集串口 ≥60s；确认 MQTT/静态 `.50`；
4. 执行 D1-01～D1-05；
5. 执行 D1-06 建立事务；
6. 在事务 active 窗口执行 D1-07～D1-09；
7. cancel，执行 D1-10；
8. 执行 D1-11（条件满足时）/D1-12；
9. 保存证据，保持 D1 BIN 运行，等待 D2 申请；不恢复正式 M1。

## 4. 证据

- 生产 BIN SHA、烧写输出 `Hash of data verified`；
- `device.info` 原始 JSON（含 `build_variant`）；
- `.async` 拒绝响应、MQTT v1 关闭响应、MQTT v1 `network.status` 原始 JSON；
- 事务 active 窗口的 `config.set`/`auth.admin.set`/`profile.delete` 响应；
- cancel 后 gate 释放响应；
- profile 列表与 `last_profile` 响应（若适用）；
- 串口日志、NVS 快照 SHA/长度（不归档 NVS 原文）；
- 所有证据文件 SHA-256。

## 5. 中止条件

- BIN SHA 不符、MAC 不是 C8E4、烧写校验失败；
- 事务无法建立或无法 cancel；
- `auth.admin.set` 意外成功（立即停止，恢复现场口令流程）；
- 任何测试返回非预期状态、设备失联、abort/重启循环；
- 需要修改 NVS/网络配置才能继续。

## 6. 授权边界

本卡只申请 D1：烧写生产 BIN、执行上述 BLE/MQTT 只读/门控测试、cancel 事务。不包含：

- D2 MQTT 事务开闸矩阵；
- 生产开关置 1；
- 修改 NVS/网络配置；
- 恢复正式 M1（另行批准）；
- 65D8 任何操作。
