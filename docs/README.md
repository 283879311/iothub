# 固件 1.3 文档导航

产品能力与构建说明见[根 README](../README.md)。当前产品事实以源码和 `contract/v1/` 为准；本导航按文档职责分类，不把历史过程稿列为当前维护入口。

## 当前维护入口

- [固件 1.3 文档维护基线](baseline/固件1.3文档基线_2026-11-18.md)
- [BLE 当前架构与验收边界](architecture/BLE_架构与验收总览.md)
- [CMD 共享命令架构与权限边界](architecture/CMD共享命令架构与权限边界.md)
- [ThingsBoard MQTT 架构与验收总览](architecture/MQTT_架构与验收总览.md)
- [Wi-Fi / Operation owner fencing](architecture/WIFI-OP-RESOURCE-RACE_主文档.md)
- [OTA 与启动回滚边界](architecture/OTA_架构与验收总览.md)
- [BLE v1 命令协议](protocol/BLE命令协议规范.md)
- [BLE 微信小程序交互参考](protocol/BLE微信小程序交互协议_T4_2026-10-24.md)

## 状态台账

- [CMD 唯一任务台账](status/CMD任务台账.md)
- [S6 现场验收状态与操作边界](status/S6现场验收总览.md)

## 设计与审查依据

- [CMD-04 快照方案总纲](design/CMD/CMD-04_status快照方案_2026-11-14.md)
- [CMD-04a operation_status 设计](design/CMD/CMD-04a_operation_status快照清单与判定表_2026-11-14.md)
- [CMD-04b network_status 设计](design/CMD/CMD-04b_network_status快照清单与判定表_2026-11-14.md)
- [CMD-02/03 并发静态审查（历史基线）](reviews/CMD/CMD-02_03_并发静态审查_2026-11-15.md)
- [CMD-04c 已实施部分审查证据](reviews/CMD/CMD-04c_execute_method内联快照清单与判定表_2026-11-14.md)
- [CMD-05 config/profile 锁序审查](reviews/CMD/CMD-05_config_profile锁序审查_2026-11-15.md)
- [P0/P1 阶段性基线与客户端审查输入（2026-11-24）](reviews/CMD/P0-P1阶段性基线_2026-11-24.md)：P0 审核依据（正式处置见 CMD 台账 §2.1）、H1-P1 设备覆盖分层与 cleanup 时序输入。
- [CMD-03 WAIT200 与 NVS 验收规范（2026-11-25）](reviews/CMD/CMD-03_WAIT200与NVS验收规范_2026-11-25.md)：WAIT200 全调用 250 ms 上限、cleanup 1 s 观察窗口与 NVS 系统管理数据例外口径。
- [MQTT 运行态与 config 同步实施证据](reviews/MQTT/MQTT运行态与config同步复核_2026-11-15.md)

设计文件说明设计约束，审查文件保存限定时点的证据；任务状态只在对应台账维护，专项材料不另行维护状态。

## 操作授权

- [S6 阶段 B 一次性操作授权申请](authorization/S6阶段B操作授权申请_2026-11-16.md)：当前为待批准申请，不是操作授权。未取得明确批准前不得进行设备操作。

## 设备报告与原始证据

- [S6 阶段 A owner 回归结果](reports/S6阶段A回归结果_R1R2R3R5_2026-11-15.md)
- [S6 阶段 A 烧录事故记录](reports/S6烧录事故记录_2026-11-15.md)
- [CMD-02/15 联合验证执行记录](reports/CMD-02_15_执行记录_2026-11-16.md)：阶段 1/2 执行事实；阶段 3 未授权。
- [S6 阶段 A 分区证据](evidence/S6_stageA/README.md)
- [S6 阶段 B 镜像 manifest](evidence/S6_stageB/images_manifest.md)
- [S6 阶段 B 身份采集](evidence/S6_stageB/identity_2026-11-15/README.md)
- [CMD-03 Phase A clean-cycle 设备与 NVS 核验（2026-11-25）](evidence/CMD-03_phase_a_clean_2026-11-25/README.md)：clean-tree T13/T19 PASS、WAIT200 pilot、NVS 前后一致、生产 main 恢复。
- [CMD-03 Phase B WAIT200 重复测量（2026-11-25）](evidence/CMD-03_phase_b_wait200_2026-11-25/README.md)：空闲与 BLE 负载两臂各 10/10 PASS；NVS 仅 `phy/cal_data` 变化（非配置类），阶段 B 未关闭。
- [H1-P1 main 固件设备验收（2026-11-24）](evidence/H1-P1_device_main_2026-11-24/README.md)：设备 `IotHub-65D8`；构建/设备证据执行于 `0e4d49c`，证据文档合入 `956afcd`；设备侧 200 ms 与设备侧完整并发矩阵为 NOT_COVERED。

## 历史材料

- [R-NET 设备验收报告](archive/2026-11/R-NET设备验收报告_2026-11-07.md)：原设备、原镜像和原测试范围的历史证据。
- [T5 首轮 50 轮 soak](archive/2026-10/T5_2026-10-24_50轮_mixed_serial.md)：仅供追溯，不代表当前固件验收。
- [BLE T5 常态化韧性计划](archive/2026-10/BLE常态化韧性验收_T5_2026-10-24.md)：历史方案，不是当前执行计划或设备授权。
- `archive/` 保存已退出当前维护集但仍有追溯价值的材料；不因归档而改变原始证据。

## 仓库级测试与工具

- [主机测试入口](../tests/README.md)
- [BLE P0 harness 与结果索引](../tools/ble_p0/README.md)
- [BLE P0 历史过程材料索引](../tools/ble_p0/results/README.md)
- 协议契约：`../contract/v1/`

## 文档状态约定

文档状态字段统一使用以下值：

- `CURRENT`：当前维护的权威说明或状态台账。
- `PENDING`：尚待实施、评审或授权的设计/申请材料。
- `HISTORICAL`：历史方案、审查、验收或事故证据；不代表当前功能状态。
- `RETIRED`：已被替代或明确不再使用的材料。

`ACTIVE` 统一改为 `CURRENT`；`ARCHIVED` 是存储位置，不是文档状态；`FROZEN` 的历史快照以 `HISTORICAL` 标识并保留不可变性说明；`CODE_DONE` 属于任务实施状态，不作为文档生命周期状态。任务台账中的 TODO、IN_PROGRESS、TEST_PENDING、PASS、DEFERRED 等仍按各自任务语义解释，不与文档状态混用。


## 仓库级协作规则

- [AGENTS.md](../AGENTS.md)：仓库级 Agent/协作者规则，涵盖授权边界、产品与架构约定、协议、设备安全、测试证据和文档维护。
