# 固件 1.3 文档导航

产品能力与构建说明见[根 README](../README.md)。当前行为以源码和 `contract/v1/` 为准；本索引只列日常开发、维护和验收仍会用到的文档。历史材料不作为当前实现依据。

## 当前基线

- [固件 1.3 文档维护基线](baseline/固件1.3文档基线_2026-11-18.md)

## 架构与维护边界

- [BLE 当前状态与维护边界](architecture/BLE_架构与验收总览.md)
- [CMD 共享命令架构与权限边界](architecture/CMD共享命令架构与权限边界.md)
- [ThingsBoard MQTT 架构与验收总览](architecture/MQTT_架构与验收总览.md)
- [Wi-Fi 网络与 owner fencing](architecture/WIFI-OP-RESOURCE-RACE_主文档.md)
- [OTA 与启动回滚边界](architecture/OTA_架构与验收总览.md)

## 协议

- [BLE v1 命令协议规范](protocol/BLE命令协议规范.md)：命令字段、请求/响应和协议边界；语义以 `contract/v1/` 为准。
- [BLE 微信小程序交互参考](protocol/BLE微信小程序交互协议_T4_2026-10-24.md)：客户端连接、订阅、分片与交互；不是固件 wire contract。

## 验证、待办与操作门禁

- [CMD 当前任务台账](validation/cmd_refactor_plan.md)
- [CMD-02/03 并发静态审查](validation/CMD-02_03_并发静态审查_2026-11-15.md)
- [CMD-04 status 快照方案](validation/CMD-04_status快照方案_2026-11-14.md)，以及 [04a](validation/CMD-04a_operation_status快照清单与判定表_2026-11-14.md)、[04b](validation/CMD-04b_network_status快照清单与判定表_2026-11-14.md)、[04c](validation/CMD-04c_execute_method内联快照清单与判定表_2026-11-14.md) 实施清单
- [CMD-05 锁序审查](validation/CMD-05_config_profile锁序审查_2026-11-15.md)
- [MQTT 运行态与 config 同步实施证据](validation/MQTT运行态与config同步复核_2026-11-15.md)
- [S6 现场验收状态与操作边界](validation/S6_现场验收总览.md)
- [S6 阶段 B 一次性操作授权申请](validation/S6阶段B操作授权申请_2026-11-16.md)：申请材料不是操作授权，实际操作仍需明确批准。

## 设备报告

- [R-NET 设备验收报告](reports/R-NET设备验收报告_2026-11-07.md)
- [S6 阶段 A owner 回归结果](reports/S6阶段A回归结果_R1R2R3R5_2026-11-15.md)
- [S6 阶段 A 烧录事故记录](reports/S6烧录事故记录_2026-11-15.md)

## 原始证据与历史材料

- [S6 阶段 A 分区证据](evidence/S6_stageA/README.md)
- [S6 阶段 B 镜像 manifest](evidence/S6_stageB/images_manifest.md)
- [S6 阶段 B 身份采集](evidence/S6_stageB/identity_2026-11-15/README.md)
- `evidence/` 保存身份、镜像、分区和哈希等原始证据；`archive/` 保存仍有追溯价值的历史材料，不属于当前维护入口。

## 仓库其他入口

- 协议契约：`../contract/v1/`
- 测试说明：[`tests/README.md`](../tests/README.md)
- BLE 测试工具：[`tools/ble_p0/README.md`](../tools/ble_p0/README.md)

专题文档中的历史测试结论必须限定原设备、原镜像和原测试范围，不得外推为当前固件 1.3 整体验收 PASS。
