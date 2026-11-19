# MQTT 架构与验收总览

> **文档状态：CURRENT（MQTT 唯一维护入口）**  
> 固件基线：1.3。当前行为以源码、`contract/v1/` 和本文件链接的专项证据为准。

## 1. 职责与边界

- 云端链路使用 ThingsBoard MQTT，承载遥测、共享属性/状态同步和受限 RPC。
- 命令仅使用 v1；旧 v0 `method/params` 入口已删除。历史 v0 报文只作为拒绝路径证据。
- MQTT 来源遵循 command source policy，不拥有 BLE 专属的本地网络管理权限；配置域允许范围以 `contract/v1/` 和当前实现为准。
- 配置持久化成功、Wi-Fi STA/IP 状态、MQTT 运行态连接状态是不同事实，不能互相替代。
- MQTT 配置读取通过 C 锁快照，Wi-Fi 运行态通过 W 锁快照；两份快照先后独立取得，**不构成跨域原子快照**。

## 2. 当前维护入口

| 内容 | 权威入口 |
|---|---|
| MQTT 职责、边界与当前缺口 | 本文 |
| MQTT 配置/运行态快照实施证据 | [运行态与 config 同步复核](../reviews/MQTT/MQTT运行态与config同步复核_2026-11-15.md) |
| CMD-15 任务状态、验收阶段与授权边界 | [CMD 当前任务台账](../status/CMD任务台账.md) |
| v1 字段、状态、错误码、来源权限 | `contract/v1/` |
| 命令注册、来源白名单与 operation owner | [CMD 共享命令架构与权限边界](CMD共享命令架构与权限边界.md) |
| 网络事务、Wi-Fi owner 与静态 IP | [Wi-Fi owner fencing 主文档](WIFI-OP-RESOURCE-RACE_主文档.md) |
| BLE 本地管理通道 | [BLE 架构与验收总览](BLE_架构与验收总览.md) |

## 3. 当前运行态快照与 CMD-15

- **R1/R2**：MQTT 配置读取集中到 `app_mqtt_cfg_snapshot()`，在 C 锁内复制，锁外构造 URI、属性和状态数据。
- **R4**：Wi-Fi 的 `sta_has_ip/sta_ip` 读取集中到 `app_mqtt_wifi_snapshot()`，在 W 锁内复制。
- C 与 W 不嵌套；按顺序分别取快照。既有 `M→W` 路径不变，不新增 `W→M`。
- 交给 esp-mqtt 的 token 使用模块静态快照缓冲 `s_token_snap`，不直接指向可被配置更新覆盖的全局配置字段。
- R3（pending/deferred 可观测字段）未实施；R5（esp-mqtt 凭据字符串复制语义专项确认）未授权/未完成。
- R1/R2/R4 当前为 **TEST_PENDING**：联合验证阶段 1 构建、阶段 2 host tests 按执行方报告通过；阶段 3 设备回归未授权/未执行。不得写成整体验收 PASS。

具体任务状态、报告限制和后续授权依赖只在 CMD 主台账维护，避免在 MQTT 文档重复维护第二份任务表。

## 4. ThingsBoard 属性与数据面边界

- 共享属性下行仍需验证 `data` 非字符串、空值、超长值、重复下发、实体归属不匹配及 Base64 语义；初始同步的实体归属缺口已有修复记录，但不能外推为所有输入边界均已验收。
- `data_pipe` 支持 UART1、BLE、MQTT 三端口路由；队列深度 16，数据面为 best-effort，不提供应用层 ACK、序号、重传或背压保证。源码具备路由能力不等于 BLE↔MQTT 端到端验收完成。
- 历史 QoS0/store=false 漏发问题已有修复记录，改为 QoS1/store=true；历史 6 分钟、1KB/s 测试报告 0 丢失、outbox 0、2,140 PUBACK。该结果仅适用于当时镜像和负载，不代表当前版本回归。
- MQTT 发布计数需区分 telemetry 与全部发布：历史 `total_published` 实际只在 telemetry 分支递增，不能当作全量 PUBACK 计数。

上述未验收边界不得标记 PASS；测试范围和结论以对应报告为准。

## 5. 仍需保留的风险与证据

- **F2 stop 位来源：UNKNOWN**。现有 R-NET 证据未闭合 stop 位完整来源链；不能推断为已定位缺陷或当前必然复现。后续取证需单独授权。
- **测试 Broker 安全结论仅属历史环境**：2026-11-06 曾发现匿名连接、公网主题读写及 Dashboard 暴露风险；不能据此断言该主机当前仍暴露或由本项目控制。原始探测记录保留在归档目录。测试 Broker 不具备 ThingsBoard token、设备主题隔离和属性响应语义，不能替代 TB 验证。
- `app_wifi_sta_config_updated()` 是否无调用者仍由 CMD-12 全仓审查闭环；在完成前保持 UNKNOWN，不据旧 backlog 删除函数。
- 历史证据：[MQTT v1 38/40 真机回归](../archive/2026-11/MQTT_v1_真机回归报告_2026-11-05.md)、[DEF-MQTT-003 R1–R4 设备报告](../archive/2026-11/DEF-MQTT-003_设备验收报告_2026-11-05.md)、[DEF-MQTT-001/002 r2b 设备报告](../archive/2026-11/MQTT受理回包竞态_设备验收报告_r2_2026-11-05.md) 均限定于原镜像和用例范围，不代表当前固件 1.3 的整体 PASS。

## 6. 维护规则

- MQTT 命令协议、字段和权限只在 `contract/v1/` 维护。
- CMD-15 的任务状态只在 CMD 主台账维护；MQTT 本文只保留当前模块边界和验收缺口。
- 历史设备结果必须连同原设备、镜像、用例范围解释，不得外推。
- 文档整理不授权配置写入、设备操作、broker 变更或新的 MQTT 验收。
