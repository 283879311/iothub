# BLE 设备侧要求检查表（微信小程序）

> 日期：2026-10-22
> 固件：HTTP-free + G3 第二轮异步事务
> 对照：`main/bt.c`、`main/bt_gatts_nimble.c`、`main/ble_cmd.c`、`main/cmd.c`、`main/operation.c`

状态说明：

- ✅ 满足
- 🟡 部分满足/有前提
- ❌ 需要调整
- 📱 小程序侧责任，固件无法单独解决

## A. 硬性要求

| # | 要求 | 状态 | 固件现状与缺口 | 建议 |
| --- | --- | --- | --- | --- |
| 1 | MTU/单包短写（按 20B 设计） | ✅ | CMD RX 支持任意短写并做 512B 行重组；Notify 按 `mtu-3` 分片，MTU 未协商时回退 20B | 小程序按 20B 写入即可 |
| 1b | 分片间 20–50ms 间隔 | 🟡 | 已改为独立 `ble_cmd_tx` 任务，CMD TX 每片之间 20ms；命令任务不再被发送阻塞。真机 Android/iOS 长响应尚未复测 | 真机连续 10 次长响应验证 |
| 2 | 固定 128-bit UUID / Service 稳定 | ✅ | NUS Service 与 4 个特征值固定 128-bit，见 `bt_gatts_nimble.c` | 不要按设备随机化 |
| 2b | 广播携带 Service UUID 以便 `services` 过滤 | 🟡 | 代码已在 ADV 加入 NUS 128-bit UUID，ADV=flags 3B + UUID 18B + 短名 9B = 30B；待小程序 `services` 过滤真机验证 | 用微信小程序按 Service UUID 扫描验证 |
| 3 | 写特征支持 Write With/Without Response | ✅ | CMD RX 与 DATA RX 都是 `WRITE | WRITE_NO_RSP` | 小程序可优先用 With Response |
| 4 | 必须有 Notify 特征 | ✅ | CMD TX、DATA TX 都是 `NOTIFY`；需客户端订阅 | 小程序先开 notify 再发命令 |
| 4b | indicate 可选 | 🟡 | 只有 notify，没有 indicate | 当前协议有 `id` + 查询，不必强加 indicate |
| 5 | 设备名可识别（iOS） | 🟡 | ADV 改为短名 `IH-65D8`，完整名 `IotHubBLE-65D8` 进 Scan Response；待 iOS 实测 | iOS 微信扫描确认名称 |
| 6 | ADV ≤31B | ✅ | 当前 flags+name+tx_power 约 19B | 加 UUID 后必须重新分配：短名进 ADV，完整名进 Scan Response |
| 6b | Scan Response | 🟡 | 已配置 Scan Response：完整名 `IotHubBLE-65D8` + tx power；待 iOS 名称显示验证 | 用 iOS 微信小程序扫描验证 |
| 7 | 业务主键不用平台 deviceId | ✅ | `device.info.device_id` 使用主机名 `IotHub-XXXX`，来自 MAC 后缀 | 小程序用 `device_id`，不要用 iOS UUID/Android MAC |

## B. 连接与配对

| # | 要求 | 状态 | 现状 | 建议 |
| --- | --- | --- | --- | --- |
| 8 | 配网阶段不启用加密配对 | ✅ | GATT 特征没有 `ENC`/bond 要求；`device.info` 明确 `link_encrypted=false,bonded=false` | 保持；BLE 链路明文，token 是应用层鉴权 |
| 9 | 单连接限制 | ✅ | 控制器 `CONFIG_BTDM_CTRL_BLE_MAX_CONN=1` | 文档提示用户先在系统蓝牙里忽略旧连接 |
| 10 | 连接后 3–5s 不断开 | ✅ | 固件无连接后主动断开逻辑；supervision timeout 6s | 无需调整 |
| 10b | 空闲不自动断（≥3 分钟） | ✅ | 无空闲自动断开；依赖 central/监督超时 | 保持 |
| 11 | 连接→服务→特征→notify 顺序 | ✅ | 标准 GATT，服务/特征发现由客户端完成 | 小程序不要跳步 |

## C. 协议与幂等

| # | 要求 | 状态 | 现状 | 建议 |
| --- | --- | --- | --- | --- |
| 12 | 帧边界可识别 | 🟡 | 命令以换行或完整 JSON 对象结束；`app_ble_cmd_json_complete()` 会跳过字符串内容，能正确处理 `}`/转义；最大 512B | 当前 JSON 协议可用；若以后传二进制，改长度前缀 |
| 13 | 应用层 ACK | ✅ | 每个 v0/v1 响应带 `id`；v1 有 `operation.get` 补查 | 客户端按 `id` 配对，超时查询/重发 |
| 13b | Notify 丢失兜底 | 🟡 | 通知失败只记录，不重试；已发送前缀不重发 | 客户端以 `operation.get` 为状态真相；关键响应不要只依赖通知 |
| 14 | version 生效 | ✅ | v1 只接受 `"version":"1.0"`，其他返回 `unsupported_version` | 小程序固定带版本 |
| 15 | idempotency_key 保留 ≥5 分钟 | ✅ | operation 终态保留 600s；同 key 返回原终态，不重复启动 | 小程序重试使用同一 key；新事务换新 key |
| 15b | apply/cancel 重复请求 | ✅ | 重复 apply success 返回 success；重复 cancel 返回 cancelled；committing 不可 cancel | 客户端不要用新 key 重放旧事务 |

## D. 小程序/系统侧

| # | 要求 | 状态 | 说明 |
| --- | --- | --- | --- |
| 16 | Android 定位权限/定位服务 | 📱 | 小程序申请；固件无关 |
| 17 | iOS 蓝牙权限时机 | 📱 | 用户点“开始配网”时申请 |
| 18 | 后台蓝牙断开 | 📱 | 固件断连会作废旧 token 并重新广播；前端回前台需重连+重新 login |
| 19 | 基础库版本 | 📱 | 蓝牙 API ≥2.0.0 |
| 20 | 扫描 8–10s 停止 | 📱 | 小程序控制 |
| 21 | 设备去重/RSSI 排序 | 📱 | 小程序控制 |
| 22 | 断连自愈 | 🟡 | 固件重新广播、token 失效；客户端必须重连并重新 login |
| 23 | 每步独立超时 | 📱 | 小程序控制；v1 已把长试连移出 CMD 任务 |
| 24 | 错误码翻译 | 📱 | 小程序把微信 BLE 错误码映射为可读提示 |

## E. 当前最需要改的固件点

### P0 实现状态（2026-10-22，提交 `f05836f`）

已实现代码：

- ADV：flags + NUS 128-bit UUID + 短名 `IH-65D8`，合计约 30B；
- Scan Response：完整名 `IotHubBLE-65D8` + tx power；
- CMD TX：独立 `ble_cmd_tx` 任务 + 队列，分片间 20ms；命令任务只入队；
- 保留 auth、operation 状态机、幂等、MQTT defer、JSON 重组不变；
- NimBLE 与 Bluedroid 回退均编译通过。

**真机验证未完成**：烧录时 C 板 USB 设备从宿主机消失（`/dev/ttyUSB0` 不存在，`lsusb` 无 CH340），需要重新接入后验证：

1. 微信小程序按 Service UUID 能扫到设备；
2. iOS 扫描显示 `IotHubBLE-65D8`；
3. 连续 10 次长响应完整；
4. 分片期间 `operation.get` 仍可响应；
5. G3 第二轮回归。

以下是原 P0 分析，保留作记录。

### P0-1：ADV 加入 Service UUID

原现状：`bt.c` 的 `app_bt_nimble_start_adv()` 只设置：

```c
flags
name
tx_pwr_lvl
```

没有 `fields.uuids128`，因此小程序的 `services` 过滤无法发现设备。

建议实现：

- 在 ADV 中放 NUS Service UUID 128-bit；
- ADV 剩余空间放**短名**（如 `IH-65D8`）；
- 完整设备名放 Scan Response；
- 重新测试 iOS `name` 字段不为空、Android `services` 过滤能命中。

### P0：CMD Notify 分片间隔

现状：`app_ble_gatts_cmd_notify()` 连续调用 `ble_gatts_notify_custom`，没有包间延时。Android 小程序在长 JSON 响应（如扫描列表、operation 状态）上可能丢通知。

建议：

- CMD TX 每片之间加 20–50ms（先试 20ms）；
- 数据面 TX 不强制加，避免影响吞吐；
- 记录通知截断/失败计数，客户端以 `operation.get` 补查。

### P1：Scan Response

建议加入完整设备名/产品短码，避免 ADV 被 Service UUID 挤占后 iOS 无名。

### P2：可选增强

- CMD TX 同时支持 indicate（关键响应重传）；
- 命令协议升级为“定长头 + 长度字段”（当前 JSON 扫描可用，不是立即阻塞）；
- 设备侧会话心跳/最大连接时长（当前不会自动断，需防僵尸连接）；
- `device.info` 增加明确 SN/产品 ID（目前 `device_id` 是主机名）。

## F. 结论

- 写属性、Notify、MTU 回退、20B 短写重组、版本校验、幂等/终态保留、设备 ID、无配对、单连接、无空闲主动断开：**已满足**。
- 小程序接入前必须补：**ADV Service UUID**、**CMD 通知分片间延时**、**Scan Response/短名方案**。
- 通知不是状态真相：客户端必须以 `operation.get` 补查；这一点当前协议已具备。
