# BLE 命令协议规范 v1.0

> 适用对象：iothub 固件 BLE CMD 通道、微信小程序/App BLE 客户端。
> 基线：`feature/shared-config-cmd`，G0 代码 `972f5dd`、G1 代码 `b50426e`。
> 文档修订：1.1（补充事务结果语义、协议探测、安全门槛、网络边界、页面/联调分级）。
> 协议版本：v1.0。
> **本文件是目标接口合同，不是当前固件全部已实现的证明。**

### 0.0 开发前 P0 前置条件

1. **v1.0 适配层只实现了现有命令子集。**
   `device.info`、`auth.login`、`config.get/set`、`network.profile.list/delete`、GPIO、`uart.send`、`data.loopback`、`auth.admin.set`、`provisioning.pin.set` 已可用 v1.0 外壳；其中 `device.info`、`auth.login`、`config.get` 已通过手机实测。`network.scan/validate/tryconnect/apply/cancel/status`、`operation.*`、`config.validate`、状态类命令仍未实现。未实现命令返回 `unknown_command`，不得在客户端伪装成功。

2. **BLE 链路安全尚未验收。**
   `login` token、TTL 和限频只是应用层保护，不是链路加密/绑定。链路安全专项完成前，敏感写操作在生产策略下必须关闭；`device.info.security` 会报告 `l2_ready`/`sensitive_writes_allowed`，客户端必须据此禁用对应页面入口。

3. **页面可开发 ≠ 设备可联调。**
   本文件会分别标注“页面可开发”“固件接口已实现”“可联调”；未标“可联调”的功能只能做 UI/协议桩，不能进入验收。

4. **事务未完成不得留下半提交状态。**
   配置保存、运行态应用、回滚的错误码和顺序见 6.3；`success` 只表示约定的全部成功条件已满足。

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

v1.0 的 `version/cmd/args/data/error object` 已对现有命令子集实现；`operation.event` 和网络事务命令尚未实现。未列出的 v1 命令返回 `unknown_command`，不能用 v0 结果冒充。

| 方言 | 请求标识 | 成功响应 | 错误响应 | 当前状态 |
| --- | --- | --- | --- | --- |
| v0 legacy | 无 `version`，`method`/`params` | `result` | 字符串 `error` | ✅ 当前固件 |
| v1.0 子集 | `version:"1.0"`，`cmd`/`args` | `data` | 对象 `error` | 🟡 已实现现有命令子集；网络事务/operation 未实现 |

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

## 2.5 v0/v1 探测与降级

v1.0 请求不能直接发给只能理解 v0 的固件并期待正常执行。客户端必须先探测：

1. 连接、发现 NUS、订阅 CMD TX。
2. 发送无 token 的 v1.0 探测：

```json
{"id":"probe-1","version":"1.0","cmd":"device.info","args":{}}
```

3. 判定：
   - 收到 v1 对象响应且 `data.protocol_versions` 包含 `"1.0"`：使用 v1.0。
   - 收到 v0 字符串错误（如 `auth_required`、`unknown_method`）或无法解析为 v1 响应：当前固件不支持 v1，改用 v0；不要继续发送 v1 的 `network.*`、`operation.*`。
   - 收到 `unsupported_version` 对象错误：明确降级到 v0，或提示用户升级固件；不能把 v1 命令自动改写成 v0 继续执行。

4. 一旦选定方言，整个连接只使用该方言；不要对单条命令失败再猜测另一种方言。
5. v0 没有 `device.info`；探测失败后，登录并读取 `getStatus` 作为旧固件状态来源。

`device.info` 必须是无副作用、无需 token 的只读命令，且返回安全能力：

```json
{
  "id": "probe-1",
  "ok": true,
  "data": {
    "device_id": "IOT-65D8",
    "firmware": "1.3",
    "protocol_versions": ["1.0"],
    "security": {
      "link_encrypted": false,
      "bonded": false,
      "l2_ready": false,
      "sensitive_writes_allowed": false
    }
  }
}
```

客户端必须在发送敏感命令前检查 `sensitive_writes_allowed`；该字段为 false 时只能展示只读/低风险功能，不能把登录成功当成已具备链路安全。

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
| `conflict` | 同一 `idempotency_key` 对应不同参数，或状态冲突 |
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

### 4.0 v1 正式名与 v0 现有名映射

| v1 正式名 | v0 现有名 | 迁移规则 |
| --- | --- | --- |
| `auth.login` | `login` | 仅 v1 适配层映射；登录不需要 token |
| `device.status` | `getStatus` | `getStatus` 只是摘要；v1 `device.status` 需补 MQTT/BLE/资源字段 |
| `gpio.status` | `io.get` | `io.get` 保留为兼容别名 |
| `network.profile.list` | `network.profiles` | v1 只暴露列表；旧名保留一个版本周期 |
| `network.reconnect` | `network.restart` | 旧名保留；语义必须按 v1 重新定义 |
| `auth.admin.set` | `web.auth.set` | 旧名只做适配；v1 必须包含会话失效 |
| `config.get` | `config.get` | 名称相同；v1 响应外壳从 `result` 变为 `data` |
| `config.set` | `config.set` | v1 使用 `{patch}`；v0 直接把 `params` 当 patch |
| `network.scan` | `network.scan` | v0 同步；v1 目标为异步 operation |
| `device.info` | 无 | v1 新增探测/安全能力入口 |

v1 客户端只使用正式名；旧名只用于 v0 兼容，不在小程序中新增使用。

### A. 设备信息与状态

| cmd | args | 当前 | v1 状态 | 说明 |
| --- | --- | --- | --- | --- |
| `device.info` | `{}` | ✅ v1 | 已实现 | 无 token、无副作用探测；返回型号、固件、设备 ID、`protocol_versions`、`security` |
| `device.status` | `{}` | `getStatus` 部分 | 未实现 | 网络/BLE/MQTT/堆/分区摘要；MQTT 状态单独字段 |
| `network.status` | `{}` | ✅ v0/v1 | 已实现 | STA 状态、SSID、IP、失败原因、MQTT 状态、trial 摘要；RSSI 待补 |
| `gpio.status` | `{}` | `io.get` 部分 | 未实现 | 引脚、输入/输出、策略、io_seq/source |
| `uart.runtime` | `{}` | HTTP UART GET | 未实现 | UART 参数、驱动状态、统计 |

### B. Wi-Fi 网络管理

| cmd | args | 当前 | v1 状态 | 说明 |
| --- | --- | --- | --- | --- |
| `network.scan` | `{}` | ✅ v0 同步 | v1 异步目标 | 扫描结果含 ssid/rssi/auth_mode；不得回密码 |
| `network.profile.list` | `{}` | ✅ v1 映射 v0 `network.profiles` | 已适配 | 只返回 SSID、`password_set` |
| `network.profile.delete` | `{ssid}` | ✅ v1 映射 v0 | 已适配 | 当前 v0 行为尚未实现“使用中返回 busy” |
| `network.validate` | `{ssid,password?,use_saved?,static?}` | ❌ | 未实现 | 只校验，不连接、不落盘 |
| `network.tryconnect` | `{ssid,password?}` | 🟡 v0 同步 | v1 未适配 | 同步阻塞最多 15s；返回 `trial_id`，不是 `operation_id`；`use_saved/static` 未实现 |
| `network.cancel` | `{trial_id}` | 🟡 v0 同步 | v1 未适配 | 仅丢弃已恢复的候选 trial；不是异步取消 |
| `network.apply` | `{trial_id}` | 🟡 v0 同步 | v1 未适配 | 只接受当前有效 got_ip trial；快照提交/回滚 |
| `network.reconnect` | `{}` | 🟡 v0 `network.restart` | v1 目标 | 用当前正式配置重连 |

### C. 统一配置

| cmd | args | 当前 | v1 状态 | 说明 |
| --- | --- | --- | --- | --- |
| `config.get` | `{domains?: ["network","mqtt","bluetooth","uart","gpio","web"]}` | ✅ v1 子集 | 已适配 | 只返回配置；运行态走 status 命令 |
| `config.validate` | `{patch:{...}}` | ❌ | 未实现 | 通用字段/跨字段校验 |
| `config.set` | `{patch:{...},idempotency_key?}` | 🟡 v1 子集 | 已适配但受限 | v1 拒绝 network 字段和 `idempotency_key`；UART 域仍不可用 |
| UART 参数 | 包含在 `config.set` 的 UART 域 | ❌ | 未实现 | 必须走 `config_apply` + 硬件回滚 |

### D. 异步操作

| cmd/能力 | args | 当前 | v1 状态 |
| --- | --- | --- | --- |
| `operation.get` | `{operation_id}` | ❌ | 未实现 |
| `operation.event` Notify | 设备主动推送 | ❌ | 未实现 |

### E. 认证与安全

| cmd | args | 当前 | v1 状态 |
| --- | --- | --- | --- |
| `auth.login` | `{username,password}` | ✅ v1 映射 `login` | 已适配 |
| `auth.admin.set` | `{old_password,new_password,new_username?}` | ✅ v1 映射 `web.auth.set` | 已适配；仍不作废旧会话 |
| `provisioning.pin.set` | `{pin,password}` | ✅ v1 映射 | 已适配；HTTP 实测，BLE 未验 |

### F. GPIO / UART / 数据面

| cmd | args | 当前 | v1 状态 |
| --- | --- | --- | --- |
| `led.set` / `led.toggle` | `{value}` / `{}` | ✅ v1 子集 | 已适配 |
| `relay.set` / `relay.toggle` | `{value}` / `{}` | ✅ v1 子集 | 已适配 |
| `io.set` | `{relay_on?,led_on?}` | ✅ v1 子集 | 已适配 |
| `uart.send` | `{encoding:"text"/"hex",data}` | ✅ v1 子集 | 已适配 |
| `data.loopback` | `{enabled}` | ✅ v1 子集 | 已适配；仅本地 |

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
    "use_saved": {"type": "boolean", "default": false},
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

> **G2 中间态（2026-10-20，提交 `82cef42`）**：固件已实现同步 `network.tryconnect/apply/cancel/status`，但使用 `trial_id`，尚未实现 `operation_id`/Notify；v1 适配层只开放 `network.status`，`tryconnect/apply/cancel` 仍返回 `unknown_command`。下面的 `operation_id` 流程是 G3 目标，不是当前 v0 命令格式。

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
- **开放网络**：`ssid` 仍必填，`password` 可为空；`validate` 只检查空口令与开放网络的组合是否合法，实际认证结果由 `tryconnect` 返回。设备不能把空口令直接当成“无需认证”而跳过候选。
- **已有 profile 复用**：`use_saved=true` 时必须同时提供匹配的 `ssid`，设备从本地 profile 取口令；找不到 profile 返回 `not_found`，不得回传口令。`use_saved=true` 与显式 `password` 同时出现时返回 `invalid_argument`，不能静默选择其中一套。
- **静态 IP**：`static.enabled=true` 时 `ip/gateway/netmask` 必填，DNS 可选；`validate` 校验地址/掩码/网关同一网段；`tryconnect` 必须实际应用候选静态配置并等待 IP；`apply` 只提交同一份已验证 plan。
- **取消恢复**：`connecting`/`got_ip` 取消后恢复旧 STA 驱动配置并重新连接旧网络；`committing` 取消返回当前状态，不承诺撤销 NVS 写入；重复取消幂等。
- **profile 删除当前网络**：删除正在使用的 profile 返回 `busy`，要求先切换到其他 profile；不得静默断开当前连接。

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

### 6.3 事务结果与回滚语义

网络 `apply` 的目标顺序：

```text
校验 operation（属于当前设备/候选代次/state=got_ip）
  → 保存旧主配置快照 + 旧 profile 快照
  → 写主配置
  → 写 profile
  → 运行态确认（候选已获得 IP，标记正式配置）
  → success
```

任一步失败都尝试恢复旧快照和旧网络；只有全部目标条件满足才返回 `success`，不允许“部分保存 + 报告成功”。

| 结果 | 触发条件 | operation 状态 | error.code | 可留下的状态 |
| --- | --- | --- | --- | --- |
| `success` | 主配置、profile、运行态确认全部成功 | `success` | — | 新配置 |
| `network_failed` | 候选连接/获得 IP 失败 | `failed` | `network_failed` | 旧配置不变 |
| `storage_failed` | NVS 写主配置或 profile 失败，且旧主配置和旧 profile 恢复成功 | `failed` | `storage_failed` | 旧配置和旧 profile |
| `apply_failed` | NVS 写入成功，但运行态确认/切换失败，且回滚成功 | `failed` | `apply_failed` | 旧配置和旧 profile |
| `rollback_failed` | 上述失败后，旧配置/profile/网络恢复自身失败 | `failed` | `rollback_failed` | 可能半提交；必须报告 `original_error` |
| `cancelled` | 在可取消状态成功取消并恢复 | `cancelled` | — | 旧配置 |
| `expired` | 候选超时且恢复成功 | `expired` | `operation_expired` | 旧配置 |

`operation.get` 的 `data` 至少包含：

```json
{
  "operation_id": "op-try-01",
  "state": "failed",
  "error_code": "rollback_failed",
  "original_error": "storage_failed",
  "rollback": "failed",
  "committed": false
}
```

- `rollback`：`none` / `old_config` / `old_profile` / `old_config_and_profile` / `failed`。
- `committed`：只有主配置和 profile 都确认提交后才为 true。
- `apply_failed` 与 `rollback_failed` 不能混用：前者表示回滚成功、最终仍是旧配置；后者表示回滚失败，设备可能处于半提交状态。
- 其他域：UART 固定“校验 → 应用硬件 → 持久化 → 保存失败回滚硬件”；硬件应用失败不写 NVS。MQTT 保存成功但 broker 连不上只算“配置已保存、连接待恢复”，不返回 `apply_failed`。

## 7. 异步操作与通知

状态集合：

```text
accepted → connecting → got_ip → committing → success
                ↘ failed / cancelled / expired
```

### 7.1 各类 operation 的合法状态

| operation type | 合法状态 | 终态/保留 | 重复提交/查询 |
| --- | --- | --- | --- |
| `network_scan` | `accepted → running → success/failed` | 结果保留 5 分钟 | 同 `idempotency_key` 返回同一 operation；否则同一时间只允许一个 scan，冲突返回 `busy` |
| `network_tryconnect` | `accepted → connecting → got_ip → cancelled/expired`；失败 `failed` | 候选 300s，终态保留 10 分钟 | 同 key 返回同一候选；新 key 且已有活动候选返回 `busy` |
| `network_apply` | `accepted → committing → success/failed` | 终态保留 10 分钟 | 同 operation_id 重复 apply 返回当前状态，不重复提交 |
| `network_reconnect` | `accepted → connecting → success/failed` | 终态保留 10 分钟 | 同 key 去重；不产生候选代次 |
| 同步命令（config.set 等） | 无 operation | — | 同值/同 key 由幂等规则处理 |

补充：

- `network_tryconnect` 的 `got_ip` 是候选状态，不是提交成功；只有 `network_apply` 成功后才是正式网络。
- `network_apply` 只接受 `got_ip` 且未过期的候选；`committing` 之后 `cancel` 不能承诺撤销。
- `operation.get` 对终态只读，不重新执行；终态状态不可逆。
- `operation_id` 在设备本次上电期间唯一，不复用；v1 默认不跨重启保留，重启后查询返回 `operation_not_found`。
- 相同 `idempotency_key` 不同参数返回 `conflict`；`details.reason` 可填 `idempotency_mismatch`。
- `event_seq` 在同一 operation 内单调递增；客户端忽略小于等于已处理序号的事件。
- 终态之后不再发送状态变化事件；查询是最终兜底。

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

### 9.1 链路安全未完成前的使用限制

`device.info.security` 是客户端判断权限的依据，不是装饰字段：

| 字段 | 含义 |
| --- | --- |
| `link_encrypted` | 当前 BLE 链路是否已加密 |
| `bonded` | 是否完成绑定/可信设备 |
| `l2_ready` | 是否满足 L2 敏感操作门槛 |
| `sensitive_writes_allowed` | 当前是否允许敏感写操作 |

当前固件应为 `false/false/false/false`。在此状态下：

- 生产策略**禁止**：`network.tryconnect/apply/cancel`（携带 Wi-Fi 凭据）、MQTT host/token 修改、`auth.admin.set`、`provisioning.pin.set`、`system.reboot`、`factory.reset`，以及任何会改变正式网络/身份的写操作。
- 允许：设备/网络/GPIO/UART 只读状态、`network.scan`、GPIO/LED 控制、`uart.send`（按现有策略）。
- `auth.login` 会把管理员口令送到未加密链路；生产环境必须等 L2 或物理配网窗口，实验室/研发模式才允许。
- 实验室模式必须在 `device.info` 中明确标记为非生产安全状态；不能把实验室联调结果当成量产结论。
- 在 L2 验收完成前，Web/SoftAP 不能删除；BLE 还不能作为唯一的生产配置通道。

客户端必须根据 `sensitive_writes_allowed` 隐藏/禁用敏感入口，不能把“登录成功”当成“已具备链路安全”。

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

### 10.1 页面可开发 vs 设备可联调

| 功能 | 页面可开发 | 固件接口已实现 | 可联调 |
| --- | --- | --- | --- |
| 扫描/连接/订阅/分片 | 是 | 是（v0 NUS） | 是（v0） |
| 登录/token/断连失效 | 是 | 是（v0 `login`） | 是（v0） |
| 设备/网络状态 | 是 | 部分（`getStatus`/`config.get`） | 只到 v0 摘要 |
| Wi-Fi 扫描/profile 列表 | 是 | 是（v0） | 是（v0 扫描/列表） |
| Wi-Fi validate/tryconnect/apply/cancel | 是（按 v1.0 合同做桩） | 否 | 否 |
| config.get/set | 是 | 部分（v0 直接 patch） | 只到 v0 能力，v1 外壳未实现 |
| UART 发送 | 是 | 是（v0） | 是（v0） |
| UART 配置 | 是 | 否 | 否 |
| GPIO 控制/状态 | 是 | 控制是；完整 status 否 | 控制可 v0，status 待补 |
| operation 中心/通知 | 是（按 v1.0 合同做桩） | 否 | 否 |
| 管理员/PIN/维护 | 是（UI 可设计） | 管理员部分、PIN 部分；维护否 | 安全门槛未过，不联调 |
| BLE 链路安全信息 | 是 | 否（`device.info.security` 待实现） | 否 |

页面验收补充：

- 连接中/已连接/断连/重连状态必须可见，不能只显示“成功”。
- 扫描空结果、扫描超时、无权限、蓝牙关闭必须有独立提示。
- 登录过期或断连后必须回到认证态并清空 token。
- 同一命令重复点击必须串行或禁用；必须按 `id` 匹配响应。
- operation 通知丢失后必须能通过 `operation.get` 恢复；不能只依赖 Notify。
- 配置失败必须区分网络失败、保存失败、应用失败和回滚失败。
- 客户端日志不得记录 Wi-Fi 口令、token、PIN、管理员口令；只记录 `id/cmd/state/error.code`。
- 页面开发完成不等于固件可联调；上表“可联调”列为“否”的功能只能使用 mock，不得进入验收。

## 11. 验收用例

| 用例 | 期望 | 当前状态 |
| --- | --- | --- |
| 无换行完整 JSON 命令 | 正常响应 | ✅ |
| 超过 512B 命令 | `line_too_long` | G1 代码，待实测 |
| 响应跨多个 notify | 客户端可重组 | G1 代码，待实测 |
| 分片中途失败 | 有 truncated 诊断，不伪报成功 | G1 代码，待实测 |
| MQTT 调用网络/UART/loopback/BT 修改 | `forbidden` | G1 代码，待实测 |
| BLE 登录/断连 token 失效 | 断连后重新登录 | ✅ 现有 |
| v1 探测发到 v0 固件 | 识别为 v0，不继续发 v1 网络命令 | ❌ 旧固件无 device.info |
| v1 `device.info` 到当前 v1 子集固件 | 返回 `protocol_versions:["1.0"]` 和安全 false | ✅ 手机实测 |
| v1 `auth.login` / `config.get` | 返回 `data` 外壳和 token/配置 | ✅ 手机实测 |
| v1 `unsupported_version` | 降级 v0 或提示升级，不误解为 v0 成功 | 🟡 适配层已实现，待实测 |
| 开放网络空口令 | validate/tryconnect 按开放网络处理，认证失败明确返回 | ❌ |
| `use_saved=true` 复用 profile | 找到 profile 才试连；找不到 `not_found`；不回传口令 | ❌ |
| 静态 IP 地址/掩码/网关非法 | `invalid_argument`，不进入试连 | ❌ |
| 同一 operation 重复 apply/cancel | 返回当前状态，不重复提交 | ❌ |
| `sensitive_writes_allowed=false` | 敏感写命令 `forbidden`，客户端隐藏入口 | ❌ 安全专项未实现 |
| `network.tryconnect` 错误密码 | `failed`，不进入 got_ip | 🟡 G2 代码，待实测 |
| `network.tryconnect` AP 不存在 | `network_failed`/明确原因 | 🟡 G2 代码，待实测 |
| 候选获得 IP 后才可 apply | apply 成功；否则拒绝 | 🟡 G2 代码，待实测 |
| 旧候选事件不提交新候选 | 拒绝旧 operation | ❌ G4 事件代次未实现 |
| apply 时主配置成功/profile 失败 | 恢复旧快照或 `rollback_failed` | 🟡 G2 快照代码，待故障注入 |
| cancel 在 connecting/got_ip | 恢复旧网络 | 🟡 trial 丢弃；异步取消待 G3 |
| cancel 在 committing | 返回提交中，不假装取消 | ❌ 同步 apply 中命令无法并发 |
| operation.get 重复查询 | 不重复执行 | ❌ |
| UART config 硬件应用失败 | 不落盘，返回 apply_failed | ❌ |
| BLE 常态开启、Wi-Fi 失败仍可连接 | G0 验收 | 代码已编译，硬件待验 |
| 恢复出厂后 BLE 仍启动 | G0 验收 | 代码已编译，硬件待验 |

## 12. 实施边界

- 本文档修订 1.1，协议版本仍为 v1.0。
- v1.0 适配层已实现现有命令子集（提交 `3dff417`）：`device.info`、`auth.login`、`config.get/set`、`network.profile.list/delete`、GPIO、`uart.send`、`data.loopback`、`auth.admin.set`、`provisioning.pin.set`；`device.info`、`auth.login`、`config.get` 已手机实测。网络事务、`operation_id`、`operation.event` 仍未实现。
- 未列入 v1 白名单的命令返回 `unknown_command`；小程序不得把本规范中的目标命令当作已可用接口。
- `config.set` 的 v1 适配当前拒绝 network 字段和 `idempotency_key`，UART 域仍不可用；这些限制在 G2/G3/G5 前保持。
- G2 同步网络事务已实现（提交 `82cef42`）：`network.status`、同步 `tryconnect`（等待 IP）、`apply`（旧配置/profile 快照、失败回滚）、`cancel`（trial 丢弃）；使用 `trial_id`，不是 `operation_id`，v1 适配层只开放 `network.status`。
- G2 的 MQTT 候选 IP 隔离、事件代次、异步取消/Notify 尚未实现，属于 G3/G4。
- 页面可以按本规范开发，但“可联调”列标为“否”的功能只能使用 mock，不得进入验收。
- G1 代码已提交（`b50426e`），运行态验收未完成。
- G0 代码已提交（`972f5dd`），硬件验收因 B 板串口设备消失未完成。
- G2–G5 尚未开始；Web/SoftAP 不删除。
- BLE 安全（D5）独立专项，不能因为 token/限频存在就认为链路安全；`sensitive_writes_allowed=false` 时生产敏感写操作必须拒绝。
- 不使用官方 `network_provisioning` 作为产品路径；`probe/ble-prov` 仅为实验分支。
