# 项目文档导航

> 更新：2026-11-17。这里是文档入口，不复制长篇进度。当前行为看当前规范/主文档；历史报告只证明其原始设备、镜像和测试范围。

## 从这里开始

| 目标 | 首选文档 |
|---|---|
| 当前固件能力、架构、构建和运行边界 | [项目 README](../README.md) |
| CMD 重构任务、状态、授权与下一步 | [CMD 重构主台账](cmd_refactor_plan.md) |
| 当前共享命令架构与来源权限 | [CMD 共享命令架构与权限边界](CMD共享命令架构与权限边界.md) |
| BLE 架构、T3/T4/T5 关系与限制 | [BLE 架构与验收总览](BLE_架构与验收总览.md) |
| BLE v1 wire 命令 | [BLE 命令协议规范](BLE命令协议规范.md) + `contract/v1/` |
| MQTT 职责、当前实现与验收 | [MQTT 架构与验收总览](MQTT_架构与验收总览.md) |
| Wi-Fi owner fencing 并发专项 | [WIFI-OP-RESOURCE-RACE 主文档](WIFI-OP-RESOURCE-RACE_主文档.md) |
| OTA 传输与启动回滚边界 | [OTA 与启动回滚总览](OTA_架构与验收总览.md) |
| S6 现场轮次、设备与授权状态 | [S6 现场验收总览](S6_现场验收总览.md) |
| 网络联网验收 | [R-NET 设备验收报告](R-NET设备验收报告_2026-11-07.md) |
| 命令层函数拆分审查 | [cmd.c 第一轮逐函数审查](cmd.c_review_2026-11-14.md) |
| 协议契约演进计划 | [协议契约整改方案](协议契约整改方案_2026-11-10.md) |
| Wi-Fi 分层重构未来方案 | [Wi-Fi 分层重构草案](wifi分层重构_设计草案_2026-11-13.md) |
| 固件 1.3 文档体系重构方案（设计签收，未执行） | [文档体系重构方案](iothub-doc-restructure-Firmware-1.3-design-approved.md) |

## 当前专项

### CMD 命令层

- [CMD 共享命令架构与权限边界](CMD共享命令架构与权限边界.md)：当前 v1 注册表、BLE/MQTT 来源策略、operation owner 与维护边界。
- [cmd_refactor_plan.md](cmd_refactor_plan.md)：唯一任务台账，含 CMD-01～CMD-15 状态、授权边界和当前执行窗口。
- [CMD-02/03 并发静态审查](CMD-02_03_并发静态审查_2026-11-15.md)：`s_net_async` 所有权与锁序。
- [CMD-03 窄窗口竞态测试设计](CMD-03_窄窗口竞态_测试设计_2026-11-15.md)：Phase A/B/C 分界与可证伪测试。
- [CMD-05 config/profile 锁序审查](CMD-05_config_profile锁序审查_2026-11-15.md)：J/C/P 锁序及 CMD-06 前置约束。
- [CMD-04 status 快照设计](CMD-04_status快照方案_2026-11-14.md)：总方案；a/b/c 清单是执行细节。
- [CMD-15 MQTT 运行态与 config 同步复核](MQTT运行态与config同步复核_2026-11-15.md)：R1/R2/R4 CODE_DONE，R3/R5 未授权。

### BLE / MQTT / Wi-Fi

- [BLE 架构与验收总览](BLE_架构与验收总览.md)：需求、协议、T3/T4/T5 关系和 DATA/安全限制。
- [MQTT 架构与验收总览](MQTT_架构与验收总览.md)：ThingsBoard MQTT v1、命令权限、历史缺陷和 CMD-15。
- [WIFI-OP-RESOURCE-RACE 主文档](WIFI-OP-RESOURCE-RACE_主文档.md)：B1–B5/C1–C3 设计、fencing 最终复审、S6 阶段 A/B 验证边界。

### S6 现场验收

- [S6 现场验收总览](S6_现场验收总览.md)：当前阶段、各轮状态与阅读顺序。
- [S6-OWNER-STUCK 固件修复](S6-OWNER-STUCK固件修复_2026-11-15.md)：修复与 FI 注入闭环。
- [S6 阶段 A 回归结果](S6阶段A回归结果_R1R2R3R5_2026-11-15.md)：65D8 功能镜像 R1/R2/R3/R5。
- [S6 阶段 B 操作授权申请](S6阶段B操作授权申请_2026-11-16.md)：最新单次申请，**待批准**。
- [S6 烧录事故记录](S6烧录事故记录_2026-11-15.md)：端口映射/镜像身份事故及强制停止条件。

## 测试与工具入口

- [tests README](../tests/README.md)：主机测试。
- [BLE P0 工具 README](../tools/ble_p0/README.md)：BLE 测试工具与历史 harness。
- [阶段变更摘要（2026-11-14 至 11-15）](近期变更记录_2026-11-14至11-15.md)：历史阶段摘要，不是 11-17 完整变更清单，也不是授权来源。

## 历史、证据与归档

- [archive/README.md](archive/README.md)：按 BLE、MQTT、Wi-Fi fencing、S6、网络/静态 IP、缺陷与历史验收分类。
- [evidence/](evidence/)：原始证据、manifest、设备身份和分区信息。
- [soak_results/](soak_results/) 与 [t5_results/](t5_results/)：历史长稳结果数据。

## 状态与维护规则

- **ACTIVE**：当前有效规范、主文档、仍在执行/审查的专项。
- **PLANNED**：只有设计或申请，尚未获得实施/设备操作授权。
- **CODE_DONE**：代码改动完成；不等于构建通过或验收 PASS。
- **PASS**：必须有对应构建/测试/设备证据，且明确适用的镜像与范围。
- **ARCHIVED**：保留历史审计，不代表现行行为或当前授权。
- **LEGACY**：旧架构/旧协议材料，不得用于描述当前产品能力。

1. 一个专题一个当前主入口；协议字段以 `contract/v1/` 为权威。
2. 任务状态只在对应主台账维护，索引只做导航。
3. 设计签收、代码完成、host 测试、构建、设备验收、操作授权彼此独立。
4. 不删除有审计价值的授权、执行、失败、回滚、事故与原始测试证据；重复索引和已被取代的入口可以删除。
5. 文档整理不扩大源码修改、设备、串口、烧录、Flash 或配置写入授权。
