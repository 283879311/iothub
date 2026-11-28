# C15-2 MQTT 路径判据与专用 harness 设计（2026-11-28）

> 文档状态：CURRENT（设计/判据修订，待评审）。
> 基线：`main@49e3f33`。本文件只定义判据与 Host 级 harness，不构成设备验证授权。
> 背景：C15-2 仍为 `NOT_COVERED`；原始 BUSY 现象未解释。BLE hook 路径的 A0/A1 设备结果不能替代原始 MQTT 路径。

## 1. 问题定性

退役的 v9 harness 在 C15-2 中把 `sta_has_ip` 当作 attributes 字段读取，但固件当前 attributes 发布契约**不包含**该字段。该冲突使 C15-2 判据在结构上不可满足，与设备时序、超时或 busy 无关。

| 字段 | attributes | telemetry | `network.status`（BLE 只读） |
|---|---|---|---|
| `net_mode` | ✅ | ✅ | —（`mode`） |
| `sta_ssid` | ✅ | ❌ | ✅ |
| `sta_ip` | ✅ | ❌ | ✅ |
| `sta_has_ip` | **❌** | **✅** | **✅** |
| `uart_baudrate` / `uart_frame` | ✅ | ✅ | — |
| `bt_mode` | ✅ | ✅ | — |
| `relay_state`/`led_state`/`input_state`/`io_seq`/`io_source` | ✅ | — | —（`device.status`） |
| `relay_on`/`led_on`/`input_active`/`free_heap` | ❌ | ✅ | — |

来源：`main/mqtt.c` 的 `app_mqtt_publish_attributes()` 与 `app_mqtt_publish_state()`；`main/cmd.c` 的 `network.status`/`device.status`。21883（ThingsBoard）订阅 telemetry 被 SUBACK 0x80 拒绝，因此 C15-2 只能订阅 attributes，`sta_has_ip` 必须走 `network.status`。

## 2. 修订后的 C15-2 判据

前提：BLE 登录成功；身份为授权镜像；`trial=idle`、`mqtt_connected=true`、`sta_connected=true` 连续稳定 10 s；attributes 订阅 CONNACK/SUBACK 成功。

| ID | 判据 | 观测点 |
|---|---|---|
| C15-2-a | 错误口令 `network.tryconnect` 被受理并返回 `operation_id` | 命令回包 |
| C15-2-b | 终态 `failed`，错误码 `wrong_password` 或 `ap_not_found` | `operation.get` 轮询 ≤40 s |
| C15-2-c | STA 在 60 s 内恢复，`device_id` 匹配 | `device.status` |
| C15-2-d | `[w0,w1]` 内至少 1 条 attributes；每条样本自洽：`net_mode ∈ {ap,sta,apsta}`；`net_mode=sta` 时 `sta_ssid` 非空；`net_mode=ap` 时 `sta_ip` 为空；`uart_baudrate` 为正数 | attributes 订阅 |
| C15-2-e | w1 之后采样的 `network.status`：`sta_has_ip=true`；`sta_ip` 等于窗口内最后一条 attributes 的 `sta_ip`；status 采样时间不早于该 attributes 样本时间 | `network.status` |

说明：

- `apsta` 不作 `sta_ip` 为空/非空的断言（该模式可能同时含 STA）；
- 不断言 `sta_has_ip` 出现在 attributes；当前位置以“当前固件 attributes 发布契约不包含该字段”表述，不推断设计意图；
- `w0`=tryconnect 发送前，`w1`=STA 恢复与身份核验后；status 采样在 w1 之后、且记录 `status_ts` 与 `sta_ip`。

## 3. 判定与容错语义

| 情形 | 判定 | 说明 |
|---|---|---|
| 窗口内 `n_attr=0` | `NOT_COVERED` | 不得用 telemetry 或其它时点样本冒充 |
| attributes 样本违反自洽规则 | `FAIL` | 记录时间戳与违规项 |
| 终态非 `failed`/错误码不符 | `FAIL` | |
| STA 未在 60 s 内恢复 | `FAIL` | |
| 身份不匹配 | `FAIL` | |
| 对读不一致 / status 早于最后样本 | `FAIL` | |
| 只读命令超时（最多 3 次、间隔 2 s）仍失败 | `STOP`（记 `NOT_COVERED` 原因） | 不补造采样 |
| tryconnect 返回 `busy`（就绪门禁之后） | `NOT_COVERED` | 不自动重发写命令 |
| 写命令 transport 异常 | `STOP` | outcome unknown，不重发 |

## 4. 专用 harness

- `tools/review/c152_policy.py`：纯判定（自洽规则、窗口、对读、重试边界、决策表），无 I/O；
- `tools/review/c152_mqtt_attrs.py`：I/O 执行器（BLE 登录、就绪门禁、attributes 订阅、tryconnect、终态/恢复/对读），复用 `p0_acceptance` 的 `Session`；
- `tests/c152_policy/`：Host 单元测试（字段缺失、零样本、超时、busy、终态、恢复、身份、对读、停止条件）。

流程：身份 → 就绪（10 s 稳定）→ attributes 订阅 → `w0` → 错误口令 tryconnect → 终态 ≤40 s → STA 恢复 ≤60 s → 身份复核 → `w1` → `network.status` 采样 → 窗口/对读判定 → JSON 结果与退出码（0/1/2/3 = PASS/FAIL/NOT_COVERED/STOP）。

## 5. Host 测试矩阵

| # | 场景 | 期望 |
|---|---|---|
| T1 | 合法 `sta` 样本 | 无违规 |
| T2 | `ap` 但 `sta_ip` 非空 | `ap_mode_with_ip` |
| T3 | `sta` 但 `sta_ssid` 为空 | `sta_mode_without_ssid` |
| T4 | `apsta` 带 `sta_ip` | 无违规（不断言） |
| T5 | 非法 `net_mode` / `uart_baudrate` | 对应违规 |
| T6 | 窗口零样本 | `NOT_COVERED` |
| T7 | 窗口外样本 | 不计入 |
| T8 | 窗口内违规 | `FAIL/attribute_inconsistent` |
| T9 | 对读：status 早于最后样本 | `status_not_after_last_attr` |
| T10 | 对读：`sta_has_ip=false` / `sta_ip` 不一致 | 对应错误 |
| T11 | 终态错误码不符 | `FAIL/terminal_state` |
| T12 | STA 未恢复 / 身份不符 | `FAIL` |
| T13 | busy | `NOT_COVERED` |
| T14 | 只读持续超时 | `STOP/read_timeout` |
| T15 | 重试边界 | 1/2 重试，3 停止 |

## 6. 边界

- 本设计不改生产固件、MQTT 发布结构、BLE CMD、UART 接口或 operation 生命周期；
- Host 测试只证明判定逻辑，不构成设备验证；C15-2 设备验证需另行授权；
- 不改变 C15-2 `NOT_COVERED`、CMD-15 `TEST_PENDING` 与原始 BUSY 未解释的事实；
- 不把本修订当作 C15-2 整体通过。
