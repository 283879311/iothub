# D1 MQTT v1 服务端注入卡（方案 1）

> 状态：等待服务端 MQTT 身份与权限；**未连接、未发布任何控制命令。**
> 设备：C8E4，TB `139.9.93.245:21883`，静态 `.50`，阶段 0 生产 BIN。
> 此卡只覆盖 **D1 生产 BIN**。MQTT 事务开闸矩阵属于 D2 测试 BIN。

## 1. 服务端身份要求

- 不使用 C8E4 设备 token；
- 具备向 `v1/devices/me/rpc/request/{requestId}` 发布的权限；
- 具备订阅 `v1/devices/me/rpc/response/+` 的权限；
- 能发布原始 v1 envelope，不被 TB 改写为 `method/params`；
- 不影响其他设备/正式业务。

若 TB broker 的 ACL 不允许服务端 MQTT 用户发布该 topic，本方案不可行，应停止并改走 TB REST/JWT 或硬件侧方案，不能降级为设备 token 自发布。

## 2. 消息格式

请求 topic：

```
v1/devices/me/rpc/request/{requestId}
```

请求 payload（原始 JSON，无外层改写）：

```json
{"id":"d1-status-1","version":"1.0","cmd":"network.status","args":{}}
```

响应 topic：

```
v1/devices/me/rpc/response/{requestId}
```

预期响应外壳：

```json
{"id":"d1-status-1","ok":true,"data":{...}}
```

错误：

```json
{"id":"d1-status-1","ok":false,"error":{"code":"forbidden","message":"..."}}
```

## 3. D1 生产 BIN 测试矩阵

| 顺序 | cmd | 期望 | 说明 |
| --- | --- | --- | --- |
| 1 | `network.status` | success + data | 保存原始 JSON；D2 形状对照基准 |
| 2 | `network.scan` | `forbidden` | **阶段 0 MQTT v1 白名单不含 scan**；不要期望长响应 |
| 3 | `network.tryconnect` | `forbidden` | 生产 `mqtt_net_txn=off`；不得创建 operation |
| 4 | `network.apply` | `forbidden` | 不得提交配置 |
| 5 | `network.cancel` | `forbidden` | 不得取消/影响 BLE 事务 |
| 6 | active BLE 事务 + MQTT `config.set {"__gate_probe":1}` | `busy` | 未知字段；门控失效时无配置副作用 |
| 7 | cancel 后 MQTT `config.set {"__gate_probe":1}` | success, changed=[] | 门控释放 |

`network.scan`/`apply`/事务启动不是 D1 成功项，而是生产关闭路径的反例；真正的 MQTT tryconnect/apply/cancel 成功路径在 D2 测试 BIN 验证。

## 4. 执行顺序

1. 服务端 MQTT 身份就绪并只读验证 publish/subscribe ACL；
2. 发送 `network.status`，保存请求/响应/设备串口；
3. 依次发送 scan/tryconnect/apply/cancel，确认 forbidden，不创建 operation；
4. BLE 建 active 事务；
5. 服务端 MQTT 发 `config.set {"__gate_probe":1}` → busy；
6. cancel BLE 事务；
7. 服务端 MQTT 再发 config.set → success/changed=[]；
8. 保存所有原始 JSON 与串口日志。

## 5. 中止条件

- 服务端 MQTT 发布被 ACL 拒绝或 payload 被改写；
- `network.status` 无响应；
- 任一 forbidden 操作意外成功/创建 operation；
- `config.set` 在 active 期间意外成功；
- 设备失联、重启循环或 MQTT 掉线。
