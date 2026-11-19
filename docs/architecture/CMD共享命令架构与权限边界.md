# CMD 共享命令架构与权限边界（固件 1.3）

> **文档状态：CURRENT（当前代码架构说明）**
> 本文记录当前 1.3 命令层的分层、入口、来源授权与维护边界；不另立一套协议契约。
> BLE wire 格式以 [BLE 命令协议规范](../protocol/BLE命令协议规范.md) 为准；命令字段、参数和错误码以 `contract/v1/` 为准（当前仍为 `1.1-draft`，不得把 draft 误称正式冻结）。
> CMD 重构任务状态只以 [CMD 当前任务台账](../status/CMD任务台账.md) 为准。

## 1. 当前架构

当前共享命令核心由以下模块组成：

| 模块 | 当前职责 |
|---|---|
| `cmd.c` | 业务命令执行、配置事务、网络操作编排及结果构造 |
| `cmd_v1.c` | v1 命令注册表、入口来源白名单和 v1 分发 |
| `cmd_policy.c` | 网络命令来源策略、operation owner 检查和取消权限 |
| `cmd_context.c` | 命令上下文、来源与执行环境校验 |
| `cmd_cfg_txn.c` / `config_apply.c` | 配置事务及配置字段应用 |
| `operation.c` / `operation_state.c` | 异步 operation 生命周期与状态管理 |
| `cmd_error.c` | v1 错误码映射 |
| `ble_cmd.c` / `mqtt.c` | 各自传输外壳、认证上下文及 v1 调用 |

`main/CMakeLists.txt` 当前编入上述命令模块及 BLE、MQTT、Wi-Fi 等产品模块；没有编入 Web/HTTP server 或 Web 页面源文件。当前产品命令入口是 BLE NUS CMD 与 ThingsBoard MQTT RPC。源码中保留的 `APP_CMD_SOURCE_HTTP` 分支/兼容处理不代表 HTTP 命令传输仍是当前产品能力。

## 2. v1 命令注册与入口

### 2.1 生产注册表

`main/cmd_v1.c` 的生产注册表（不含条件编译测试钩子）包含以下 26 个命令：

| 类别 | 当前注册命令 |
|---|---|
| 设备/状态 | `device.info`、`device.status`、`gpio.status`、`uart.runtime`、`network.status` |
| 配置与 IO | `config.get`、`config.set`、`io.get`、`io.set`、`led.set`、`led.toggle`、`relay.set`、`relay.toggle` |
| 网络事务 | `network.scan`、`network.profile.list`、`network.profile.delete`、`network.reconnect`、`network.tryconnect`、`network.apply`、`network.cancel`、`operation.get` |
| 身份/维护 | `auth.login`、`auth.admin.set`、`provisioning.pin.set` |
| 数据面 | `uart.send`、`data.loopback` |

`diag.m1hook` 和 `diag.testhook` 仅在相应故障注入/测试构建宏开启时注册，不属于默认生产命令。

### 2.2 来源白名单

注册表逐命令声明 BLE/MQTT 来源：

- **BLE**：生产注册表中的全部命令均允许进入 v1 分发；实际执行仍受 token、命令内授权和策略检查约束。
- **MQTT**：允许设备/状态、配置、IO、网络状态、网络事务、operation 查询等注册命令；不允许 `network.scan`、profile list/delete、`network.reconnect`、`auth.login`、管理员凭据/PIN 修改、UART 发送和 data loopback。
- **HTTP**：不在 v1 注册表入口来源中；当前构建也没有 HTTP server 命令传输。

上述是注册入口白名单，不等同于每条命令必然执行成功。还要经过上下文有效性、网络策略、operation owner 和命令内部的参数/授权检查。

## 3. 认证与命令授权

认证证明请求持有相应传输凭据；命令授权决定该来源可执行哪些操作。两者不能混为一谈。

### BLE

- `auth.login` 使用设备管理员凭据签发连接级短时 token。
- token TTL 为 600 秒；BLE 断连时清除，重连后必须重新登录。
- `device.info` 是无需 token 的 v1 能力/版本探测入口；其余受保护命令使用 token。
- token 有效不意味着可以绕过命令注册表、来源策略、参数检查或 operation owner 检查。

### MQTT

- MQTT RPC 上下文由已建立的 ThingsBoard 设备连接提供，命令执行仍走 v1 注册表和策略层。
- `config.set` 对 MQTT 来源拒绝网络、MQTT、蓝牙、UART、管理员认证等本地/身份相关配置域；GPIO 和 data route 按当前代码策略处理。
- `auth.admin.set`、`provisioning.pin.set`、`uart.send`、`data.loopback` 等本地维护命令不允许 MQTT 来源。
- 网络事务命令的 MQTT 支持由 `APP_CMD_MQTT_NET_TXN_ENABLE` 控制；当前 `main/cmd_policy.h` 在未由构建参数覆盖时将宏默认定义为 `1`；`main/CMakeLists.txt` 另有仅供测试构建使用的显式定义选项。默认源码策略因此允许 MQTT 发起网络事务，但实际镜像仍应以其编译宏/ `device.info` 构建变体为准。MQTT 只能操作其自身拥有的 operation，不能接管 BLE operation。
- MQTT 的网络试连受理路径会先完成响应投递协调，再进入可能导致链路中断的 Wi-Fi 操作；不得把“命令已接受”误解为“网络切换已完成”。

## 4. 网络命令策略与 operation 所有权

`cmd_policy.c` 对网络命令按来源实施额外约束：

| 来源 | 允许范围与限制 |
|---|---|
| BLE | 网络状态、扫描、profile 删除、重连、tryconnect/apply/cancel、operation 查询；取消权限受本地 token 和 owner 规则约束 |
| MQTT | 网络状态和 operation 查询；scan/profile 管理/reconnect 禁止；tryconnect/apply/cancel 只有构建开关启用时允许，且只能操作 MQTT-owned operation |
| HTTP 兼容上下文 | 策略代码保留 HTTP owner 分支，但当前产品构建没有 HTTP server 命令入口，不作为当前产品功能承诺 |
| Internal | 只允许由现有 operation 内部提交路径执行 `network.apply`；不能借 internal context 暴露通用外部命令 |

operation 查询对 MQTT/HTTP 按 owner 隔离；BLE 本地 token 可按现行策略查询不同来源 operation。取消操作另由 `app_cmd_cancel_allowed()` 判定，不应只依据“能查询”推导“能取消”。

## 5. 共享命令必须保持的实现原则

1. **传输外壳与业务语义分离**：BLE/MQTT 负责各自帧格式、传输认证和回包；业务操作复用命令层，不在传输层复制配置/网络规则。
2. **来源授权集中且可审查**：新增命令必须同步审查 `cmd_v1.c` 注册来源、`cmd_policy.c` 策略、命令内部检查及对应测试。
3. **配置写入走统一配置应用路径**：字段归属、校验、持久化和运行态应用应核对 `cmd_cfg_txn.c`、`config_apply.c` 与相应领域模块；不能在 BLE/MQTT handler 里各自写 NVS。
4. **网络操作走 operation 状态机**：受理、运行、终态、取消、超时、owner 隔离及清理由 operation 机制管理；不能用传输连接存活代替 operation 生命周期。
5. **敏感信息不回显、不进日志**：Wi-Fi 密码、MQTT token、管理员凭据及 provisioning PIN 不得通过配置读取、错误 details、日志或命令回包泄露；UART 数据也可能携带外部凭据。
6. **协议语义不由本文定义**：请求外壳、字段、错误码、operation 状态及兼容要求按 BLE 规范和 `contract/v1/` 核对。任何契约差异先报告，单独评审授权；不得借文档迁移顺手修改协议。
7. **验证分层**：源码注册/静态检查、host tests、ESP-IDF 构建、BLE 实机、MQTT 实机是不同证据。某一通道通过不能写成跨通道完成。

## 6. 当前事实与契约差异

本轮源码对照发现以下差异，登记为后续契约审查项；本次文档迁移不修正代码或 Contract：

- `cmd_v1.c` 生产注册表为 26 项，而 `contract/v1/commands.json` 当前列出的命令集合并未覆盖全部注册命令；源码独有命令包括 `gpio.status`、`uart.runtime`、`io.get`、`io.set`、`led.set`、`led.toggle`、`relay.set`、`relay.toggle`、`network.reconnect`、`auth.admin.set`、`provisioning.pin.set`、`uart.send`。
- `contract/v1/commands.json` 的 firmware baseline 为 `1.3-9e1d6c5 / build_sha 8cc566bd0 (2026-11-10)`；`contract/v1/errors.json` 为 `1.3-2ef2a38 / build_sha 98f2465cb (2026-11-10)`。二者均为 `1.1-draft`，基线彼此不一致，不能直接作为已统一的固件基线声明。
- BLE 协议规范说明了当前 BLE v1 wire 入口；MQTT 透传 v1 payload 的实际命令覆盖和逐命令设备验收，不能仅由注册表存在推导为已通过。
- MQTT 网络事务构建开关当前为 ON，但该构建事实不替代 MQTT 真实链路、断连响应投递和 operation owner 的设备回归证据。

差异的正式登记应在 Contract 维护范围内单独评审和授权；当前不依赖已不存在的整改方案文档作为入口。

## 7. 变更门禁与状态入口

- 新增/修改命令：先检查 Contract 字段和错误码，再检查注册表、来源白名单、策略与内部授权；不允许只加 handler。
- 修改来源权限：必须同时审查 BLE token、MQTT 设备身份、operation owner 和敏感配置边界。
- 修改配置事务或网络 operation：必须核对锁序、回调时序、cancel/expire 清理和资源预算；CMD-01～CMD-15 状态以主台账为准。
- 既有失败收尾审计依赖 BLE disconnect 路径先触发取消/cleanup；若新增绕过该路径的候选事务清理方式，或改变 `store_trial` 失败/取消顺序，必须重新审查前置条件并增加可注入时序测试。旧审计结论不外推为所有未来失败路径均安全。
- 本文不是任务台账，不记录逐项完成状态，也不授予构建、host 测试、设备、串口、烧录或 Flash 操作权限。

## 8. 关联文档

- [CMD 当前任务台账](../validation/cmd_refactor_plan.md)
- [BLE 命令协议规范](../protocol/BLE命令协议规范.md)
- [MQTT 架构与验收总览](MQTT_架构与验收总览.md)
- [Wi-Fi owner fencing 主文档](WIFI-OP-RESOURCE-RACE_主文档.md)
- [OTA 与启动回滚总览](OTA_架构与验收总览.md)
- [项目 README](../../README.md)
- [CMD-02/03 并发静态审查](../reviews/CMD/CMD-02_03_并发静态审查_2026-11-15.md)
- [CMD-15 MQTT 运行态与 config 同步复核](../reviews/MQTT/MQTT运行态与config同步复核_2026-11-15.md)
