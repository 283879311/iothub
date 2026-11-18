# BLE 命令协议规范（v1-only，2026-11-04）
> **状态**：ACTIVE（v1 wire 协议规范；语义以 contract/v1 为准）（文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)）


> 适用：iothub 固件 BLE CMD 通道、微信小程序/App BLE 客户端。
> 当前固件：v1-only；v0 `method/params` 入口已**彻底删除**（源码、旧方法别名、编译开关均无，2026-11-05）。
> 旧版规范已归档：`docs/archive/BLE命令协议规范_v0历史_2026-11-04.md`。
> 历史验收记录中的 v0 命令仅作证据，不再是可用接口。

## 1. 传输

| 项 | 规范 |
| --- | --- |
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| CMD RX | `6E400004-...`，客户端写入 |
| CMD TX | `6E400005-...`，设备 notify |
| DATA RX/TX | `6E400002/3`，只走 data_pipe，不混 JSON |
| 编码 | UTF-8 JSON，一条逻辑消息一个对象，`\n` 可作结束符 |
| 分片 | 客户端按实际 MTU-3 分片；保守 20B/片；设备按 512B 行重组 |

## 2. 请求/响应

请求：

```json
{"id":"req-1","version":"1.0","cmd":"device.info","args":{},"token":"<TOKEN>"}
```

- `id`：必填，字符串或数字；响应原样回显。建议 1–64 字符。
- `version`：必填 `"1.0"`；缺失返回 `unsupported_version`。
- `cmd`：必填，见命令目录。
- `args`：必填对象；无参数传 `{}`。
- `token`：受保护命令必填；`auth.login`、`device.info` 不需要。

成功：

```json
{"id":"req-1","ok":true,"data":{...}}
```

失败：

```json
{"id":"req-1","ok":false,"error":{"code":"invalid_argument","message":"..."}}
```

客户端按 `code` 判断，不依赖 `message` 文案。

## 3. 认证

- `auth.login` 参数：`{"username","password"}`。
- 成功：`data.token`、`data.expires_in`（600s）。
- token 放请求顶层；连接断开即失效，重连必须重新登录。
- `auth.admin.set` 成功后会作废所有既有 token；客户端必须丢弃本地 token 并重新 `auth.login`。
- `device.info` 无 token、无副作用，可用于版本/安全能力探测。

## 4. 命令目录

### 4.1 设备/状态

| cmd | args | 说明 |
| --- | --- | --- |
| `device.info` | `{}` | 型号、固件、device_id、`protocol_versions:["1.0"]`、`build_variant`、security |
| `device.status` | `{}` | 运行时长、复位原因、堆、BLE/MQTT、分区、网络摘要 |
| `gpio.status` | `{}` | relay/led/input、io_seq、极性、上电策略、引脚 |
| `uart.runtime` | `{}` | UART 帧/波特率/引脚/驱动/收发计数 |
| `config.get` | `{"domains"?:["network","mqtt","bluetooth","uart","gpio","web"]}` | 脱敏配置 |
| `io.get` | `{}` | IO + 网络摘要（旧 getStatus 语义） |

### 4.2 配置

| cmd | args | 说明 |
| --- | --- | --- |
| `config.set` | `{"patch":{...},"idempotency_key"?}` | 同步 patch；BLE v1 允许 network/static 域；MQTT 仍禁止 net/MQTT 域 |

- BLE v1 静态 IP：`patch` 内 `sta_use_static`/`sta_static_ip`/gateway/netmask/dns1/dns2；`effect:"reboot"` 时响应后重启。
- `network`/`mqtt`/`bluetooth`/`uart`/`web` 域按 `config_apply` 校验；未知字段忽略。
- MQTT v1 `config.set` 仅允许非 net/MQTT/BT/UART/admin 域。

### 4.3 IO / UART / 数据面

| cmd | args |
| --- | --- |
| `led.set` / `led.toggle` | `{"value":bool}` / `{}` |
| `relay.set` / `relay.toggle` | `{"value":bool}` / `{}` |
| `io.set` | `{"relay_on"?:bool,"led_on"?:bool}` |
| `uart.send` | `{"encoding":"text"/"hex","data":"..."}`（仅 BLE） |
| `data.loopback` | `{"enabled":bool}`（仅 BLE） |

### 4.4 网络

| cmd | args | 说明 |
| --- | --- | --- |
| `network.status` | `{}` | STA/IP/MQTT/trial 摘要 |
| `network.scan` | `{}` | 同步扫描；BLE 可用，MQTT 禁止 |
| `network.profile.list` | `{}` | SSID + `password_set`；仅 BLE |
| `network.profile.delete` | `{"ssid"}` | 普通删除；最后一个 profile 返回 `last_profile`；仅 BLE |
| `network.reconnect` | `{}` | 用当前正式配置重连；仅 BLE |
| `network.tryconnect` | `{"ssid","password"?,"idempotency_key"?,"ip_mode"?}` | 异步；返回 `operation_id`、`state:"accepted"` |
| `operation.get` | `{"operation_id"}` | 查询状态；MQTT 只能查 MQTT-owned，HTTP 只能查 HTTP-owned；BLE 可查全部 |
| `network.apply` | `{"operation_id"}` | 提交 GOT_IP 事务；owner 必须匹配 |
| `network.cancel` | `{"operation_id"}` | 取消事务；BLE 可跨 owner，MQTT 只能取消 MQTT-owned |

`ip_mode`：`"keep"`（默认）或 `"dhcp"`；`"static"`、`allow_leave_static` 返回 `invalid_argument`。当前静态模式下跨 SSID 或 `ip_mode="dhcp"` 返回 `static_ip_pinned`；离开静态需先 BLE v1 `config.set` 切 DHCP。

### 4.5 身份/维护

| cmd | args | 来源 |
| --- | --- | --- |
| `auth.login` | `{"username","password"}` | BLE |
| `auth.admin.set` | `{"old_password","new_password","new_username"?}` | BLE |
| `provisioning.pin.set` | `{"pin","password"}` | BLE |

## 5. 网络事务

```text
network.tryconnect
  -> data.operation_id / state=accepted
  -> operation.get 轮询到 got_ip（或 failed/cancelled/expired）
  -> network.apply({operation_id}) 或 network.cancel({operation_id})
```

- 不再有 v0 同步 `trial_id` 协议。
- `apply` 成功返回 `state:"success"`、`saved:true`、`applied:true`。
- 失败/取消后事务进入终态；终态保留 600s，可用 `operation.get` 查询。
- 静态 IP 模式跨 SSID 被拒绝；`network.apply` 不再无条件把静态改 DHCP。

## 6. 错误码

常用 v1 code：

`invalid_argument`、`unauthorized`、`forbidden`、`busy`、`not_found`、`operation_state`、`trial_invalid`、`wrong_password`、`ap_not_found`、`wifi_scan_failed`、`connect_timeout`、`connect_failed`、`wifi_config_offline`、`static_ip_pinned`、`last_profile`、`storage_failed`、`rollback_failed`、`apply_failed`、`rate_limited`、`internal_error`。

规则：

- 参数/字段问题 → `invalid_argument`
- 未认证/过期 token → `unauthorized`
- source/权限/生产关闭 → `forbidden`
- 事务忙 → `busy`
- 事务状态不符 → `operation_state`
- 静态 IP 锁定 → `static_ip_pinned`
- 最后一个 profile 删除 → `last_profile`

## 7. v1-only 兼容

- 无 `version` 的 v0 请求返回 `unsupported_version`；不自动降级。
- `device.info.protocol_versions` 只返回 `["1.0"]`。
- 旧固件不支持 v1；小程序如需兼容旧设备，必须先探测再选择方言；新固件只走 v1。
- 速率限制：每 transport 约 5 次/秒（200ms）；超限 `rate_limited`。

## 8. 当前限制

- `operation.event` 未实现，必须轮询 `operation.get`。
- `network.validate`、`config.validate`、`system.reboot`、`factory.reset` 未实现。
- `network.scan` 仍同步。
- `network.apply` 内部仍复用旧提交实现，但对外只暴露 `operation_id`。
- MQTT v1 命令需要原始 v1 payload；TB 服务端不需要协议改造，只负责透传。
- BLE v1 静态 IP 写入/恢复、`network.apply` 成功路径已在 C8E4 自测通过；MQTT v1 原始注入仍未验证。
