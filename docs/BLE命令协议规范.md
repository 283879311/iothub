# BLE 命令协议规范 v1.0

> 适用对象：iothub 固件 BLE CMD 通道、微信小程序/App BLE 客户端。
> 基线：`feature/shared-config-cmd`，G0 代码 `972f5dd`、G1 代码 `b50426e`。
> **本文件是目标接口合同，不是当前固件全部已实现的证明。**

## 0. 当前状态与兼容声明

当前固件 BLE CMD 只有 **v0 legacy 封装**：

```json
{"id":1,"method":"config.get","params":{"domains":["network"]},"token":"..."}
```

响应：

```json
{"id":1,"ok":true,"result":{...}}
{"id":1,"ok":false,"error":"invalid_config"}
```

v1.0 的 `version/cmd/args/data/error object/operation.event` **尚未实现**。必须先完成固件适配层，小程序才能使用 v1.0；在此之前只能用 v0 做联调。

| 方言 | 请求标识 | 成功响应 | 错误响应 | 当前状态 |
| --- | --- | --- | --- | --- |
| v0 legacy | 无 `version`，`method`/`params` | `result` | 字符串 `error` | ✅ 当前固件 |
| v1.0 | `version:"1.0"`，`cmd`/`args` | `data` | 对象 `error` | ❌ 目标，需适配层 |

固件适配层目标：收到 v1.0 请求时，把 `cmd` 映射到现有 `method`、`args` 映射到 `params`；响应再转换为 `data`/对象错误。不得改变现有 v0 客户端的行为。

## 1. 传输与分帧

| 项目 | 规范 |
| --- | --- |
| 服务 | Nordic UART Service 风格，UUID `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| CMD RX | `6E400004-...`，客户端写入命令 |
| CMD TX | `6E400005-...`，设备 notify 响应和异步事件 |
| 数据 RX/TX | `6E400002/3`，只走 `data_pipe`，不得混入配置 JSON |
| 编码 | UTF-8 JSON 对象，一次逻辑消息一个对象 |
| 命令行上限 | 512B；超限返回 `line_too_long` |
| 换行 | 不要求 `\n`；客户端可发送完整 JSON 对象，也可按行结束 |
| 单次写入 | 客户端必须按协商 MTU-3 分片；设备支持跨多次写入重组，收齐完整 JSON 后才解析 |
| 响应分片 | 设备按 MTU-3 分片 notify；客户端必须重组到完整 JSON |
| 消息交错 | CMD TX 同一时刻只允许一个逻辑消息分片序列；客户端以完整 JSON 边界重组，不能假设一个 notify 就是一个完整响应 |

客户端约束：

- 不能假设一次 `writeBLECharacteristicValue` 能写入完整命令。
- 一次写入成功只代表 GATT 写成功，不代表设备执行成功。
- 必须等待对应 `id` 的响应；异步操作用 `operation_id` 关联。
- 响应必须按 `id` 匹配，不能依赖到达顺序。
- 断连后清除 token、待处理请求和已订阅状态；重连后重新订阅 CMD TX 并重新登录。

## 2. v1.0 请求、响应与错误

### 2.1 请求

```json
{
  "id": "req-0001",
  "version": "1.0",
  "cmd": "device.info",
  "args": {},
  "token": "optional"
}
```

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `id` | string | 是 | 客户端生成的请求标识，响应原样回显；建议 1–64 字符 |
| `version` | string | 是 | 固定 `"1.0"`；缺失按 v0 legacy 处理 |
| `cmd` | string | 是 | `模块.动作`，见命令目录 |
| `args` | object | 是 | 无参数传 `{}` |
| `token` | string | 条件 | 受保护命令必填；`auth.login` 不需要；位置保持在顶层，与现有固件一致 |

### 2.2 同步成功

```json
{
  "id": "req-0001",
  "ok": true,
  "data": {
    "device_id": "IOT-65D8",
    "firmware": "1.3",
    "protocol_versions": ["1.0"]
  }
}
```

### 2.3 同步错误

```json
{
  "id": "req-0001",
  "ok": false,
  "error": {
    "code": "invalid_argument",
    "message": "ssid is required",
    "details": {}
  }
}
```

`code` 是稳定判断依据；`message` 只用于展示/日志，客户端不得依赖其文案。

### 2.4 异步受理

```json
{
  "id": "req-0002",
  "ok": true,
  "data": {
    "operation_id": "op-7f21",
    "state": "accepted"
  }
}
```

## 3. 错误码

### 3.1 v1.0 通用错误码

| code | 含义 |
| --- | --- |
| `invalid_json` | JSON 解析失败 |
| `line_too_long` | 命令超过 512B |
| `unknown_command` | 命令不存在 |
| `invalid_argument` | 参数缺失、类型或范围错误 |
| `unauthorized` | 未登录、token 无效或过期 |
| `forbidden` | 已认证但当前来源无权执行 |
| `busy` | 同领域有互斥操作 |
| `rate_limited` | 超过命令频率限制 |
| `not_found` | 对象不存在 |
| `operation_not_found` | operation_id 不存在或已清理 |
| `operation_expired` | operation_id 已过期 |
| `unsupported_version` | 不支持的协议版本 |
| `network_failed` | Wi-Fi 扫描/连接/提交失败 |
| `storage_failed` | NVS 持久化失败 |
| `apply_failed` | 配置已保存但运行态应用失败 |
| `rollback_failed` | 失败恢复自身失败 |
| `internal_error` | 未分类内部错误 |

### 3.2 当前 v0 错误到 v1 的映射

| v0 字符串 | v1 code |
| --- | --- |
| `invalid_json` | `invalid_json` |
| `line_too_long` | `line_too_long` |
| `unknown_method` | `unknown_command` |
| `params_required` / `invalid_config` / `invalid_param` | `invalid_argument` |
| `auth_required` / `invalid_credentials` / `unauthorized` | `unauthorized` |
| `guard_rejected` | `forbidden` |
| `rate_limited` | `rate_limited` |
| `not_found` | `not_found` |
| `save_failed` | `storage_failed` |
| `wifi_scan_failed` / 网络连接失败 | `network_failed` |
| `restart_failed` / `apply_failed` | `apply_failed` |
| `out_of_memory` / 未分类 | `internal_error` |

v0 客户端继续收到原字符串；v1 适配层负责映射，不能把 v1 对象直接回给 v0 客户端。

## 4. 命令目录

状态说明：

- **v0现有**：当前固件已有 handler，可直接用 v0 调用。
- **v1适配**：handler 已存在，但 v1.0 封装尚未实现。
- **未实现**：目标命令不存在。
- **仅目标**：设计已冻结，代码未开始。

### A. 设备信息与状态

| cmd | args | 当前 | v1 状态 | 说明 |
| --- | --- | --- | --- | --- |
| `device.info` | `{}` | ❌ | 未实现 | 型号、固件版本、设备 ID、支持的协议版本；用于探测 v1.0 是否可用 |
| `device.status` | `{}` | `getStatus` 部分 | 未实现 | 网络/BLE/MQTT/堆/分区摘要；MQTT 状态单独字段 |
| `network.status` | `{}` | 无正式命令 | 未实现 | STA 状态、SSID、IP、RSSI、失败原因、候选操作 |
| `gpio.status` | `{}` | `io.get` 部分 | 未实现 | 引脚、输入/输出、策略、io_seq/source |
| `uart.runtime` | `{}` | HTTP UART GET | 未实现 | UART 参数、驱动状态、统计 |

### B. Wi-Fi 网络管理

| cmd | args | 当前 | v1 状态 | 说明 |
| --- | --- | --- | --- | --- |
| `network.scan` | `{}` | ✅ v0 同步 | v1 异步目标 | 扫描结果含 ssid/rssi/auth_mode；不得回密码 |
| `network.profile.list` | `{}` | ✅ v0 `network.profiles` | v1适配 | 只返回 SSID、`password_set` |
| `network.profile.delete` | `{ssid}` | 🟡 v0 | v1适配 | 删除当前 profile 的行为需定义 |
| `network.validate` | `{ssid,password,static?}` | ❌ | 未实现 | 只校验，不连接、不落盘 |
| `network.tryconnect` | `{ssid,password,static?,idempotency_key?}` | ❌ | 未实现 | 启动候选；返回 `operation_id` |
| `network.cancel` | `{operation_id}` | ❌ | 未实现 | 幂等取消；`committing` 不承诺撤销 |
| `network.apply` | `{operation_id,idempotency_key?}` | ❌ | 未实现 | 只接受 `got_ip` 的候选 |
| `network.reconnect` | `{}` | 🟡 v0 `network.restart` | v1 目标 | 用当前正式配置重连 |

### C. 统一配置

| cmd | args | 当前 | v1 状态 | 说明 |
| --- | --- | --- | --- | --- |
| `config.get` | `{domains?: ["network","mqtt","bluetooth","uart","gpio","web"]}` | ✅ v0 | v1适配 | 只返回配置；运行态走 status 命令 |
| `config.validate` | `{patch:{...}}` | ❌ | 未实现 | 通用字段/跨字段校验 |
| `config.set` | `{patch:{...},idempotency_key?}` | 🟡 v0 直接 patch | v1适配 | v0 使用 `params` 直接作 patch；v1 统一 `patch` |
| UART 参数 | 包含在 `config.set` 的 UART 域 | ❌ | 未实现 | 必须走 `config_apply` + 硬件回滚 |

### D. 异步操作

| cmd/能力 | args | 当前 | v1 状态 |
| --- | --- | --- | --- |
| `operation.get` | `{operation_id}` | ❌ | 未实现 |
| `operation.event` Notify | 设备主动推送 | ❌ | 未实现 |

### E. 认证与安全

| cmd | args | 当前 | v1 状态 |
| --- | --- | --- | --- |
| `auth.login` | `{username,password}` | ✅ v0 `login` | v1适配 |
| `auth.admin.set` | `{old_password,new_password,new_username?}` | 🟡 v0 `web.auth.set` | v1适配；需作废旧会话 |
| `provisioning.pin.set` | `{pin,password}` | ✅ HTTP 实测 | v1适配；BLE 未验 |

### F. GPIO / UART / 数据面

| cmd | args | 当前 | v1 状态 |
| --- | --- | --- | --- |
| `led.set` / `led.toggle` | `{value}` / `{}` | ✅ v0 | v1适配 |
| `relay.set` / `relay.toggle` | `{value}` / `{}` | ✅ v0 | v1适配 |
| `io.set` | `{relay_on?,led_on?}` | ✅ v0 | v1适配 |
| `uart.send` | `{encoding:"text"/"hex",data}` | ✅ v0 | v1适配 |
| `data.loopback` | `{enabled}` | ✅ v0 | v1适配；仅本地 |

### G. 维护

| cmd | args | 当前 | v1 状态 |
| --- | --- | --- | --- |
| `system.reboot` | `{}` | ❌ | 未实现 |
| `factory.reset` | `{confirm:true}` | ❌ | 未实现；需额外授权/物理确认 |

## 5. 关键参数 Schema

### 5.1 `network.tryconnect`

```json
{
  "type": "object",
  "required": ["ssid"],
  "properties": {
    "ssid": {"type": "string", "minLength": 1, "maxLength": 32},
    "password": {"type": "string", "maxLength": 64},
    "idempotency_key": {"type": "string", "minLength": 8, "maxLength": 64},
    "static": {
      "type": "object",
      "properties": {
        "enabled": {"type": "boolean"},
        "ip": {"type": "string"},
        "gateway": {"type": "string"},
        "netmask": {"type": "string"},
        "dns1": {"type": "string"},
        "dns2": {"type": "string"}
      }
    }
  },
  "additionalProperties": false
}
```

### 5.2 `network.apply` / `network.cancel` / `operation.get`

```json
{
  "type": "object",
  "required": ["operation_id"],
  "properties": {
    "operation_id": {"type": "string", "minLength": 1, "maxLength": 64},
    "idempotency_key": {"type": "string", "minLength": 8, "maxLength": 64}
  },
  "additionalProperties": false
}
```

### 5.3 `config.set`

```json
{
  "type": "object",
  "required": ["patch"],
  "properties": {
    "patch": {"type": "object"},
    "idempotency_key": {"type": "string", "minLength": 8, "maxLength": 64}
  },
  "additionalProperties": false
}
```

## 6. 网络配置事务

推荐流程，客户端必须按此执行：

```text
network.validate
  → network.tryconnect（返回 operation_id）
  → operation.event(state=got_ip) 或 operation.get
  → network.apply(operation_id)
  → operation.get / operation.event(success)
```

### 6.1 规则

- `validate` 只检查参数/跨字段/硬件能力，不连接、不落盘。
- `tryconnect` 使用候选配置；候选期间不得修改正式配置，不得触发 MQTT 正式切换。
- 只有候选关联成功且**获得有效 IP** 才进入 `got_ip`；`STA_CONNECTED` 不算成功。
- v1.0 以 `operation_id` 作为候选句柄；`trial_id` 只作为内部实现名，不在协议中单独暴露。
- `apply` 必须校验 operation 属于当前设备、当前候选代次、状态为 `got_ip`。
- `apply` 提交主配置和 Wi-Fi profile；成功后 operation 进入 `success`。
- 失败恢复旧主配置和旧 profile；恢复失败返回 `rollback_failed` 并保留诊断信息。
- `cancel` 仅对 `connecting`/`got_ip` 有效；`committing` 不承诺撤销已写入的 NVS。
- 同一时间只允许一个活动候选；新 `tryconnect` 不得静默覆盖旧候选。
- 老候选/旧连接事件不得完成或提交新候选。
- MQTT 重连与网络提交分开报告；`success` 不代表 MQTT 已连接。

### 6.2 示例

试连受理：

```json
{
  "id": "req-0104",
  "ok": true,
  "data": {"operation_id": "op-try-01", "state": "accepted"}
}
```

通知：

```json
{
  "type": "operation.event",
  "operation_id": "op-try-01",
  "event_seq": 2,
  "state": "got_ip",
  "data": {"ip": "192.168.1.45"}
}
```

提交：

```json
{"id":"req-0105","version":"1.0","cmd":"network.apply","args":{"operation_id":"op-try-01"}}
```

取消：

```json
{"id":"req-0106","version":"1.0","cmd":"network.cancel","args":{"operation_id":"op-try-01"}}
```

## 7. 异步操作与通知

状态集合：

```text
accepted → connecting → got_ip → committing → success
                ↘ failed / cancelled / expired
```

| 字段 | 说明 |
| --- | --- |
| `operation_id` | 设备生成的唯一操作标识 |
| `state` | `accepted/connecting/got_ip/committing/success/failed/cancelled/expired` |
| `event_seq` | 同一 operation 内单调递增，客户端忽略重复/过期事件 |
| `data` | 当前状态的数据，如 IP、失败原因、恢复结果 |

`operation.get` 返回最终/当前状态；终态建议保留 10 分钟，具体以实现为准。设备重启后是否保留结果必须在实现时明确；v1 默认不承诺跨重启。

Notification 示例：

```json
{
  "type": "operation.event",
  "operation_id": "op-try-01",
  "event_seq": 3,
  "state": "success",
  "data": {"ssid": "Office-WiFi", "ip": "192.168.1.45", "mqtt_state": "reconnecting"}
}
```

可选状态通知（目标，非现有功能）：

| type | 数据 | 说明 |
| --- | --- | --- |
| `device.status.changed` | `device.status` 的摘要 | 网络/BLE/MQTT/堆发生重要变化 |
| `network.status.changed` | `network.status` 的摘要 | 连接、IP、RSSI、候选状态变化 |

状态通知没有 `operation_id`，客户端按 `type` 处理；不要用状态通知推断某个 operation 已成功。

规则：

- Notify 只是主动提示，不能作为唯一结果通道；必须能通过 `operation.get` 查询。
- 重复 `operation.get` 不重新执行操作。
- 相同 `operation_id` 的重复 `apply` 不重复提交；返回当前状态。
- `idempotency_key` 用于请求重试去重；与 `operation_id` 分离。
- CMD TX 响应和 operation 事件都必须是完整 JSON 对象；客户端按 JSON 边界重组，不按单次 notify 边界。

## 8. 配置域与字段

`config.get` 的 `domains`（v0 已支持）取值：

```text
network, mqtt, bluetooth, uart, gpio, web
```

建议 v1.0 继续使用 `domains` 数组；`domain` 单值可作为兼容别名。示例：

```json
{"id":"req-0201","version":"1.0","cmd":"config.get","args":{"domains":["uart"]}}
```

规则：

- 口令/token/hash 永不回明文，只回 `*_set`。
- 运行时状态不放在配置域；走 `device.status/network.status/gpio.status/uart.runtime`。
- `network` 写入在 v1.0 只能通过 `network.tryconnect/apply`；`config.set` 收到 network 字段返回 `invalid_argument`（`details.use = "network.tryconnect/apply"`）。
- `bluetooth` 域只允许明确开放的字段（设备名）；运行角色固定 Peripheral，不允许把 BLE 关闭。
- `web` 域只读用户名；管理员凭据通过 `auth.admin.set`。
- `mqtt` token 只写不读；`token_set` 表示是否设置。
- UART 配置通过统一 `config_apply` 路径，硬件应用失败不落盘。

## 9. 权限矩阵

目标权限，不是当前实现：

| 能力 | HTTP | BLE | MQTT |
| --- | --- | --- | --- |
| 读取脱敏配置/状态 | 允许 | 登录后允许 | 允许（脱敏） |
| GPIO/LED/继电器控制 | 允许 | 登录后允许 | 允许（按策略） |
| Wi-Fi 扫描 | 允许 | 登录后允许 | 禁止 |
| Wi-Fi validate/tryconnect/apply/cancel/reconnect | 允许 | 登录后允许 | 禁止 |
| MQTT host/token 修改 | 允许 | 登录后允许 | 禁止 |
| UART 配置修改 | 允许 | 登录后允许 | 禁止 |
| `uart.send` | 允许 | 登录后允许 | 默认禁止 |
| `data.loopback` | 允许 | 登录后允许 | 禁止 |
| 蓝牙名称/（角色固定） | 允许名称 | 登录后允许名称 | 禁止 |
| 管理员凭据/PIN | 管理员验证 | L2 授权 | 禁止 |
| 重启/恢复出厂 | 受保护 | L2/物理确认 | 默认禁止 |

BLE 登录 token 只证明应用层身份，不是链路加密/绑定。敏感操作最终需要 L2 安全专项；当前 `SM_LVL=0` 不具备该保证。

## 10. 微信小程序交互流程

1. 扫描：按服务 UUID `6E400001-...` 和广播名 `IotHubBLE-*` 过滤，展示 RSSI。
2. 连接：建立 GATT 连接，发现 NUS 服务。
3. 订阅：订阅 CMD TX `6E400005` notify；未订阅时命令写入可能收不到响应。
4. 认证：发送 `auth.login`，保存 token；断开后清空。
5. 读取：`device.info` 判断协议版本；`device.status/network.status` 展示状态。
6. 配置：Wi-Fi 走 validate/tryconnect/apply；其他配置走 config.get/set。
7. 异步：保存 `operation_id`，监听 `operation.event`，同时支持 `operation.get` 兜底。
8. 断连：清除 token、待处理请求、订阅状态；重连后重新登录。
9. 错误：按 `error.code` 处理，不依赖 message。

客户端分片：

- 按 MTU-3 写入；单条命令总长 ≤512B。
- 每次写入后等待设备重组；不要在一次写入后立即认为命令完成。
- 响应/通知按完整 JSON 重组；按 `id` 或 `operation_id` 关联。
- 同一连接建议串行发送命令；设备限频约 5 次/秒。

## 11. 验收用例

| 用例 | 期望 | 当前状态 |
| --- | --- | --- |
| 无换行完整 JSON 命令 | 正常响应 | ✅ |
| 超过 512B 命令 | `line_too_long` | G1 代码，待实测 |
| 响应跨多个 notify | 客户端可重组 | G1 代码，待实测 |
| 分片中途失败 | 有 truncated 诊断，不伪报成功 | G1 代码，待实测 |
| MQTT 调用网络/UART/loopback/BT 修改 | `forbidden` | G1 代码，待实测 |
| BLE 登录/断连 token 失效 | 断连后重新登录 | ✅ 现有 |
| `network.tryconnect` 错误密码 | `failed`，不进入 got_ip | ❌ |
| `network.tryconnect` AP 不存在 | `network_failed`/明确原因 | ❌ |
| 候选获得 IP 后才可 apply | apply 成功；否则拒绝 | ❌ |
| 旧候选事件不提交新候选 | 拒绝旧 operation | ❌ |
| apply 时主配置成功/profile 失败 | 恢复旧快照或 `rollback_failed` | ❌ |
| cancel 在 connecting/got_ip | 恢复旧网络 | ❌ |
| cancel 在 committing | 返回提交中，不假装取消 | ❌ |
| operation.get 重复查询 | 不重复执行 | ❌ |
| UART config 硬件应用失败 | 不落盘，返回 apply_failed | ❌ |
| BLE 常态开启、Wi-Fi 失败仍可连接 | G0 验收 | 代码已编译，硬件待验 |
| 恢复出厂后 BLE 仍启动 | G0 验收 | 代码已编译，硬件待验 |

## 12. 实施边界

- v1.0 封装、`operation_id`、网络候选状态机尚未实现；小程序不得把本规范中的目标命令当作已可用接口。
- G1 代码已提交（`b50426e`），运行态验收未完成。
- G0 代码已提交（`972f5dd`），硬件验收因 B 板串口设备消失未完成。
- G2–G5 尚未开始；Web/SoftAP 不删除。
- BLE 安全（D5）独立专项，不能因为 token/限频存在就认为链路安全。
- 不使用官方 `network_provisioning` 作为产品路径；`probe/ble-prov` 仅为实验分支。
