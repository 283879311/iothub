# 阶段 0 代码审查索引

> 日期：2026-11-04
> 方式：只读整理；未改代码、未构建、未连接设备、未烧写。
> 说明：仓库工作区在阶段 0 之前已有 M1/静态 IP 未提交改动；`stage0_cmd_mixed.diff` 含这些前置改动，不能当作纯阶段 0 delta。

## 文档

- 设计过程稿：未保留在当前仓库；本目录仅保留补丁与执行证据，不把缺失的设计文件作为维护入口。
  - SHA-256：`ef29919c3721b70b7d1a717ec92e3dfd1f4e8a9b173cafd9bbb3d39d5948075c`
- 当前实现与任务状态：以 [MQTT 架构与验收总览](../../../../../docs/architecture/MQTT_架构与验收总览.md) 和 [CMD 任务台账](../../../../../docs/status/CMD任务台账.md) 为准。
  - SHA-256：`e13833ba51f268856ee3050f7a909e5bf6dddf25a3da56e90a3f45e930d42fd9`

## Diff

- `stage0_new_and_clean.diff`
  - 新增模块/测试全量 diff；
  - `ble_cmd.c`、`mqtt.c`、`config.c`、`tests/README.md` 的工作区 diff（这些文件在阶段 0 前未改动）。
- `stage0_cmd_mixed.diff`
  - `cmd.c`、`operation.c`、`operation.h`、`CMakeLists.txt`；
  - **包含阶段 0 之前已有的 M1 工作区改动**，审查时请按 hunk 区分。

## 关键源码入口

- 策略：`main/cmd_policy.c` / `main/cmd_policy.h`
- 配置门控：`main/cmd_cfg_txn.c` / `main/cmd_cfg_txn.h`
- 适配层声明：`main/cmd_net_adapter.h`
- 统一分派/策略/门控：`main/cmd.c`
- BLE v1 adapter：`main/ble_cmd.c`
- MQTT v1 adapter：`main/mqtt.c`
- profile 不变量/工厂清空：`main/config.c` / `main/config.h`
- owner/错误枚举：`main/operation.h` / `main/operation.c`
- 共享 v1 错误映射：`main/cmd_error.c` / `main/cmd_error.h`

## 构建/主机验证（无设备）

- 生产 BIN：`/tmp/iothub_stage0_build/iothub.bin`
  - SHA-256：`4ddc7d9b5403b8f0c1fcb2590c238b62aa14b9cbf9fae3099992c69b7e28a352`
  - `mqtt_net_txn=off`
- 测试 BIN：`/tmp/iothub_stage0_testbuild/iothub.bin`
  - SHA-256：`2891326680832f469b50ee341c048c8f886dfab50ac033704c04eac44deca4c0`
  - `mqtt_net_txn=test`
- 主机测试：`cmd_policy` 51 + test-build PASS；`cmd_cfg_txn` 22；`cmd_error` PASS。
- 设备侧：**未执行**。
