# BLE 需求基线（常态本地管理通道）

> 依据：阶段 2 设计冻结、G1–G6 关卡、当前 `iothub` 源码。
> 本文件冻结 BLE 的产品定位与需求；本轮不改代码。现状栏是源码事实，目标栏才是待实现要求。

## 1. 定位

BLE 是设备**常态运行时的本地配置与管理通道**，不是一次性 Wi-Fi 配网工具。

- ESP32 常态作为 BLE Peripheral / GATT Server。
- 手机 App/小程序作为 Central / GATT Client。
- BLE 生命周期不依赖 Wi-Fi 或 MQTT 是否连接。
- Wi-Fi 断开、MQTT 离线时，手机仍应能连接设备、查询状态、修复网络配置。
- 数据透传继续走现有 NUS 数据特征；控制命令继续走 CMD 特征和 `app_cmd_execute_ex()`。
- Web 配置页是过渡入口；最终删除 Web 不等于删除 BLE 管理能力。

## 2. 需求与现状对照

| 需求 | 目标 | 当前现状 | 差距/证据 |
| --- | --- | --- | --- |
| 常态广播 | 正常启动即初始化 BLE Peripheral 并广播 | 只有 `bt_role=PERIPHERAL` 或兼容 `bt_mode=BLE` 才启动；出厂默认 `bt_role=0`（off） | `config.c:394-395`、`config.c:415`、`bt.c:402-417`；恢复出厂后 BLE 默认关闭 |
| 生命周期独立于 Wi-Fi | Wi-Fi 初始化失败/未连接时 BLE 仍可管理 | BLE 在 `app_start_wifi()` 和 `app_captive_init()` 之后启动 | `main.c:63-73`；`app_start_wifi()` 内部使用 `ESP_ERROR_CHECK`，失败会中止启动 |
| 生命周期独立于 MQTT | MQTT 离线不影响 BLE | MQTT 在 BLE 之后启动，通常不阻塞 BLE | `main.c:88-91`；仍需断网/无 token 场景实测 |
| 自动恢复广播 | 手机断开后恢复广播 | 断连回调调用 `app_bt_restart_advertising()` | `bt_gatts_nimble.c:271-282`、`bt.c:373-393` |
| 设备发现 | 手机扫描到稳定名称 | 正常模式为 `IotHubBLE-xxxx`；探针模式为 `PROV_IOT_xxxx` | `identity.c:19-41`；两种模式名称不同 |
| 状态查询 | 设备/Wi-Fi/MQTT/资源状态 | `getStatus`/`config.get` 只有部分字段 | `cmd.c:482-503`；缺正式 `device.status/network.status` |
| Wi-Fi 配置 | 校验、试连、提交、取消/重试 | 现有 `network.scan`、`network.profiles`、`network.profile.delete`、`network.restart`；`config.set` 可直接写网络字段 | `cmd.c:747-865`；无 `network.tryconnect/apply/cancel` |
| Wi-Fi 事务 | 候选验证通过后才提交，失败恢复旧配置 | 当前网络配置直接落盘，重启由调用方触发 | `config_apply.c:864`、`http_network.c:184-246`；G2/G3/G4 范围 |
| 参数管理 | 统一命令读取/修改本地配置 | `config.get/set` 已存在；UART 域未接入 `config_apply` | `config_apply.c:688-700`、`uart.c:539-580`；G5 范围 |
| 异步操作 | `operation_id`、查询、通知、去重 | 未实现 | G3 范围 |
| 命令响应 | JSON、稳定错误码、超长错误 | G1 已实现超长 `line_too_long` 与分片发送结果，待运行态验收 | `ble_cmd.c` G1 改动；提交 `b50426e` |
| 分片接收/发送 | 跨写入重组，发送失败不误报 | RX 支持 512B 行重组；TX 返回实际发送字节数 | `ble_cmd.c`、`bt_gatts_nimble.c`；G1 待实测 |
| 数据透传 | 与控制命令分离 | NUS 数据特征 + `data_pipe` 独立 | `transport_ble.c`、`data_pipe.c`；保持 |
| 安全 | 认证、授权、限频、凭据生命周期、链路安全 | login token 600s、断连失效、约 5 次/秒限频；`SM_LVL=0`、无绑定/加密 | `ble_cmd.c:24-26`、`ble_cmd.c:198-207`、`ble_cmd.c:240-260`；D5/G6 |
| 客户端兼容 | 复用现有 JSON 协议 | CMD 已支持无换行完整 JSON、登录 token、统一 `{id,ok,result/error}` | `ble_cmd.c:106-141`、`ble_cmd.c:210-345` |

## 3. 角色边界

产品只需要 Peripheral / GATT Server：

- 保留广播、接受连接、GATT 服务/特征、命令写入、通知响应。
- 不需要 ESP32 主动扫描或连接其他 BLE 外设。
- 不需要为 Wi-Fi 配网切换主从角色，也不进入一次性配网模式。
- 暂不强制关闭 NimBLE Central 编译能力；是否关闭需在 G6 前按 Flash/RAM 实测决定，不能只凭产品不用 Central 就推断收益。

当前 `bt_role` 仍允许 off/central/peripheral；目标产品默认应为 peripheral：
`bt.c:402-417`、`bt.c:729-754`。

## 4. 与 Wi-Fi / MQTT / Web 的关系

| 通道 | 最终职责 |
| --- | --- |
| BLE | 本地发现、配置、管理、故障恢复 |
| Wi-Fi | 设备接入局域网/互联网 |
| MQTT | 遥测、云端状态同步、授权远程业务控制；不承担远程配网 |
| Web 配置页 | 过渡入口，最终删除 |

关键约束：

- BLE 不能在 Wi-Fi/MQTT 失败后才开启，也不能在 Wi-Fi 重连时被关闭。
- MQTT 配置保存成功不等于 MQTT 已连接；BLE 状态查询必须分别报告。
- Web 删除必须等 BLE 独立配置、恢复和验收通过；当前不删除。

## 5. 资源约束

当前基线约 `free=14.9 KiB`、`largest=8.7 KiB`。BLE 方案必须：

1. 复用 `app_cmd_execute_ex()`，不复制第二套业务逻辑；
2. CMD 行缓冲固定 512B，队列 8，不随命令动态扩大；
3. 异步操作对象、候选配置和通知缓存使用固定上限；
4. BLE 回调只投递，不阻塞等待 Wi-Fi/MQTT；
5. 试连、提交、断连恢复期间记录 `free/largest` 最低值；
6. 不新增常驻任务；需要隔离长操作时优先复用现有事件循环/任务。

## 6. 现状差距与实施顺序

### 必须优先解决

1. **默认角色**：出厂默认 `bt_role=off` 与“常态广播”冲突；需产品确认将默认改为 peripheral（或启动时强制 peripheral）。
2. **启动顺序**：BLE 目前在 Wi-Fi/captive/OTA 挂载之后，Wi-Fi 初始化失败会阻断 BLE；需把 BLE 初始化前移或在 Wi-Fi 失败时仍保证 BLE 起来。
3. **状态命令**：补 `device.status`、`network.status`，至少覆盖 BLE/Wi-Fi/MQTT/堆/失败原因。
4. **Wi-Fi 事务**：按 G2/G3/G4 实现候选、试连、提交、取消和恢复；G1 验收前不新增这些命令。
5. **安全**：D5 独立专项；`SM_LVL=0` 不能视为链路安全。

### 关卡对应

| 关卡 | 与本需求的关系 |
| --- | --- |
| G1 | 命令权限、BLE 超长/分片错误；已完成代码，待运行态验收 |
| G2 | 主配置/profile 快照恢复 |
| G3 | `operation_id`、候选契约、去重/结果查询 |
| G4 | Wi-Fi 状态机与 MQTT 事件隔离 |
| G5 | UART 统一配置路径 |
| G6 | 常态广播、断网管理、断连重连、异常恢复、安全、资源整体验收 |

## 7. 验收补充（BLE 常态管理）

在 G6 原有验收外，增加：

- 设备启动后无需 Wi-Fi/MQTT 成功即可被手机发现并连接。
- Wi-Fi 未配置、Wi-Fi 断开、MQTT 离线时，BLE 仍可查询状态和修改网络配置。
- 手机断开后设备恢复广播；重复连接不产生重复初始化或泄漏。
- BLE 命令与数据透传并发时，CMD 不阻塞数据通道。
- 超长命令返回 `line_too_long`；分片发送失败有 `truncated` 诊断，不伪报完整成功。
- 恢复出厂后默认角色、广播名和 BLE 可管理性符合产品定义。
- 记录 BLE 连接前后、Wi-Fi 连接后、MQTT 连接后、试连/提交期间的最低 `free/largest`。

## 8. 当前边界

- 本轮只固化需求，不改代码。
- 不新增 `network.tryconnect/apply/cancel`，直到 G1 运行态验收完成。
- 不提前启动网络状态机、Web 清理或 ThingsBoard OTA。
- 不强制关闭 NimBLE Central 编译能力；资源收益需实测。
- BLE 链路安全作为独立专项，不阻塞 G2–G5 的代码设计，但阻塞 G6 的安全结论。
