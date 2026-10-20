# BLE 现有功能与目标功能清单

> 基线：`feature/shared-config-cmd`，G1 代码提交 `b50426e`。
> 结论：产品 BLE 使用现有 NUS + JSON 命令体系，不使用官方 ESP BLE Provisioning 协议。
> `probe/ble-prov` 是独立探针分支，不属于产品配置路径。

## 1. 当前已有功能

### 1.1 GATT 服务与通道

| 项目 | 当前实现 |
| --- | --- |
| 服务 | Nordic UART Service（NUS），UUID `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| 数据 RX | `6E400002-...`，手机写入，进入 `data_pipe` |
| 数据 TX | `6E400003-...`，设备 notify，数据面输出 |
| CMD RX | `6E400004-...`，手机写入，进入 JSON 命令行重组 |
| CMD TX | `6E400005-...`，设备 notify，命令响应 |
| 通道隔离 | 数据特征和命令特征物理分开；命令回调只重组/入队，数据写入只入 `data_pipe` |
| 单次写入 | 数据 RX 缓冲 256B；CMD 行上限 512B |
| 通知分片 | 按 `MTU-3` 分片；CMD 分片结果在 G1 中改为实际发送字节数 |

### 1.2 角色、广播和连接

| 项目 | 当前实现 |
| --- | --- |
| Peripheral | 已实现：广播、GATT Server、命令/数据特征、断连后恢复广播 |
| Central | 已实现但产品不使用：扫描/连接其他 NUS 设备，走 central 数据面 |
| 角色选择 | `bt_role=off/central/peripheral`；`bt_mode=BLE` 兼容旧配置 |
| 默认角色 | 出厂默认 `bt_role=0`（off），恢复出厂后 BLE 不启动 |
| 广播名 | 正常固件 `IotHubBLE-<MAC 后4位>` |
| 断连恢复 | `app_ble_gatts_nimble_on_disconnect()` 调用 `app_bt_restart_advertising()` |
| 启动位置 | BLE 在 Wi-Fi、captive、OTA 挂载之后初始化（`main.c`），Wi-Fi 失败会阻断 BLE |

### 1.3 命令协议与认证

| 项目 | 当前实现 |
| --- | --- |
| 命令格式 | `{"id":N,"method":"...","params":{...},"token":"..."}` |
| 行结束 | 支持 `\n` 结尾，也支持一次写入完整 JSON、无换行 |
| 响应 | `{"id":N,"ok":true,"result":{...}}` 或 `{"id":N,"ok":false,"error":"..."}` |
| 登录 | `login` 使用 Web 管理员凭据换 600s token |
| token | 连接级、断线失效；重启不保留 |
| 限频 | 约 5 次/秒；超限返回 `rate_limited` |
| 超长命令 | G1 后返回 `{"ok":false,"error":"line_too_long"}` |
| 分片失败 | G1 后低层返回实际发送字节数；部分发送记录 `truncated`，不误报完整成功 |
| 链路安全 | NimBLE `SM_LVL=0`，无强制配对/绑定/加密；token 只是应用层认证 |
| 命令权限 | 统一进入 `app_cmd_execute_ex()`；G1 已收紧 MQTT 来源，BLE 来源保留既有本地能力 |

### 1.4 当前可用命令

| 类别 | 命令 | 状态/限制 |
| --- | --- | --- |
| 配置读取 | `config.get`（`domains` 过滤） | ✅；仍混有部分运行态 |
| 配置写入 | `config.set` | 🟡；网络字段直接落盘，无试连/事务 |
| 状态摘要 | `getStatus` / `io.get` | 🟡；只有 IO/网络摘要 |
| GPIO | `led.set/toggle`、`relay.set/toggle`、`io.set` | ✅ |
| 网络扫描 | `network.scan` | ✅；同步 |
| profile 列表/删除 | `network.profiles`、`network.profile.delete` | ✅/🟡；无“正在使用”保护 |
| Wi-Fi 重配 | `network.restart` | 🟡；无完成状态/取消 |
| UART 发送 | `uart.send` | ✅；响应字段 `sent` |
| 数据回环 | `data.loopback` | ✅ |
| 管理员凭据 | `web.auth.set` | 🟡；不作废旧 Web 会话 |
| 配网 PIN | `provisioning.pin.set` | ✅ HTTP 实测；存储 `ble_prov/pop` |
| 异步查询 | 无 | ❌ |

### 1.5 当前数据面

- BLE 数据面通过 `data_pipe` 与 UART1、MQTT 路由。
- 默认路由：UART1 → BLE；BLE → UART1/MQTT；MQTT → BLE。
- 数据面与 CMD TX/RX 使用不同特征，命令处理不占用数据 RX/TX。
- 当前没有 BLE 文件传输或固件 OTA 数据通道。

### 1.6 当前持久化

| 数据 | NVS |
| --- | --- |
| 主配置 | `iothub/runtime_cfg` |
| Wi-Fi profiles | `iothub/wifi_profiles` |
| 配网 PIN | `ble_prov/pop` |
| 输出记忆 | `iothub/io_relay_last`、`io_led_last` |
| BLE token | RAM，不持久化 |

## 2. 目标功能（Web/MQTT 配网退出后）

### 2.1 常态化 Peripheral

- BLE 始终以 Peripheral / GATT Server 运行，不依赖 Wi-Fi/MQTT。
- 启动顺序：加载配置 → 初始化 BLE → 初始化 Wi-Fi/MQTT/HTTP。
- 旧配置中的 `off/central` 在启动时收敛为 Peripheral；恢复出厂后也必须是 Peripheral。
- `app_bt_release_unused_memory()` 不再释放 BLE 所需内存。
- Central 代码可以在收益确认后关闭/删除；未实测前不宣称 Flash/RAM 收益。
- Wi-Fi 未配置、断开、MQTT 离线时，BLE 仍可发现、连接、查询和配置。

### 2.2 唯一本地配置入口

Web 配置页和 SoftAP 最终删除后，BLE 必须覆盖：

| 能力 | 目标命令/能力 |
| --- | --- |
| 设备信息 | `device.info` |
| 设备/运行状态 | `device.status`、`gpio.status`、`uart.runtime` |
| 网络状态 | `network.status`（SSID/IP/RSSI/失败原因/MQTT 状态分开） |
| 网络扫描 | `network.scan` |
| 网络校验 | `network.validate` |
| 候选试连 | `network.tryconnect` + `operation_id` |
| 取消试连 | `network.cancel` |
| 提交网络 | `network.apply`（IP 成功后提交，失败恢复） |
| 重连 | `network.reconnect` |
| profile 管理 | `network.profile.list/delete` |
| 配置读写 | `config.get/set/validate`，覆盖 network/MQTT/UART/GPIO/web |
| UART 配置 | 接入 `config_apply`，保留硬件回滚 |
| GPIO | `gpio.status` + `set/toggle` |
| 管理员凭据 | `auth.admin.set`，响应后作废旧会话 |
| 配网/维护 PIN | `provisioning.pin.set`（不回显） |
| 维护 | `system.reboot`、受保护的 `factory.reset` |
| 异步查询 | `operation.get` |

### 2.3 网络配置事务

目标流程：

```text
network.validate
  → network.tryconnect（候选代次/trial_id）
  → 获得 IP
  → network.apply（提交主配置和 profile）
  → network.status / operation.get
```

- 失败恢复旧主配置与旧 profile；恢复失败单独上报。
- 候选事件必须绑定代次；旧连接/过期事件不能提交。
- 试连期间 MQTT 不能被候选 IP 误触发；提交成功后才恢复 MQTT。
- 取消在 `CONNECTING/GOT_IP` 生效；`COMMITTING` 不承诺撤销已写 NVS。
- 异步操作通过 `operation_id` 查询，BLE 通知作为主动提示。

### 2.4 安全目标

- 独立 BLE 安全专项：配对/绑定、链路加密、物理配网窗口、每设备 PIN、凭据生命周期。
- `login` token 只能作为应用层认证，不能替代链路安全。
- 敏感命令（网络、MQTT token、管理员、恢复出厂、重启）按 L2 授权。
- 超长命令/分片失败/未授权/限频必须有明确错误，不静默丢弃。
- 任何日志、状态查询、错误消息不返回 Wi-Fi/MQTT/管理员明文凭据。

### 2.5 资源目标

- 保持现有 NUS + JSON + `app_cmd_execute_ex()`，不引入第二套配网框架。
- 不新增常驻任务；异步状态复用现有 Wi-Fi 事件循环。
- 固定候选/操作/通知缓冲，限制峰值堆。
- 记录 BLE 连接前后、Wi-Fi 连接后、MQTT 连接后、试连/提交期间的最低 `free/largest`。

## 3. 当前与目标的差距

| 能力 | 当前 | 目标 | 关卡 |
| --- | --- | --- | --- |
| 常态 Peripheral | 默认 off，启动在 Wi-Fi 后 | 强制 Peripheral，先于 Wi-Fi | G6/always-on 前置 |
| Central | 编译并可按配置启动 | 产品不使用；裁剪需实测 | 资源专项 |
| 网络状态 | `getStatus` 摘要 | `network.status` 完整状态 | 状态命令 |
| 网络试连/提交 | 直接 `config.set` 落盘 | validate/tryconnect/apply/cancel | G2–G4 |
| 异步操作 | 无 | operation_id/查询/通知/去重 | G3 |
| 状态命令 | 部分 | device/network/gpio/uart status | 状态命令 |
| UART 配置 | HTTP 专用 | 统一 `config_apply` | G5 |
| 管理员凭据 | `web.auth.set` 不作废旧会话 | `auth.admin.set` + 会话失效 | G1 后续 |
| BLE 链路安全 | `SM_LVL=0` | 独立安全专项 | D5/G6 |
| 超长/分片错误 | G1 代码已改 | 运行态验收 | G1 |
| 归一化权限 | G1 部分完成 | MQTT 禁止项全部落地 | G1 |

## 4. 明确不做/暂不做

- 不把 ESP 官方 `network_provisioning` 作为产品路径；`probe/ble-prov` 只保留为实验分支。
- 不在 BLE 上传输固件；OTA 走 ThingsBoard 原生 MQTT OTA 或最小 HTTP OTA。
- MQTT 不承担远程配网；MQTT 只做遥测、状态和授权业务控制。
- Web 配置页/SoftAP 暂不删除；在 BLE 独立配置、恢复、OTA 和回归验收前保留。
- 不在 G1 运行态验收前新增 `network.tryconnect/apply/cancel`。
- 不强制关闭 NimBLE Central 编译能力；先测收益。
