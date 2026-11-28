# C15-2 Telemetry REST + Attributes + BLE 混合观测判据与 harness 设计（2026-11-28）

> 文档状态：PENDING（设计/判据修订，Host 级实施中；不构成设备验证授权）。
> 基线：`feat/c152-mqtt-criterion-20261128 @ 8d7c430`（保留不覆盖）。
> 命名：**Telemetry REST + Attributes + BLE 混合观测方案**（不使用“路径 B”简称，避免与历史 Rule Chain 方案混淆）。
> 背景：C15-2 原始 MQTT 路径为 `NOT_COVERED`；历史 BUSY 未解释。旧 harness 通过设备 Access Token 建立第二 MQTT 会话，会驱逐设备会话，已停止使用。

## 1. 方案边界

- 观测通道改为 **TB REST 只读 GET**：telemetry timeseries + attributes；不再使用 MQTT 订阅，不使用设备 Access Token。
- 命令与状态通道保持 BLE：`device.status`、`network.status`、`operation.get`、`network.tryconnect`。
- 不改 `main/` 生产源码、MQTT 发布结构、BLE CMD/Contract、UART 接口、operation 状态机与锁序、设备配置/NVS。
- REST 凭据只从运行环境读取，不进入代码、日志、测试报告；Host 测试全部使用模拟 REST 响应。

## 2. 字段映射（唯一/首选来源）

| 字段 | 首选来源 | 交叉核对 | 验收要求 |
|---|---|---|---|
| `net_mode` | telemetry timeseries | BLE `network.status.net_mode` | 窗口内每个样本均为 `sta`；BLE 也为 `sta` |
| `uart_baudrate` | telemetry timeseries | — | 窗口内每个样本为数值且 `>0` |
| `sta_has_ip` | telemetry timeseries | BLE `network.status.sta_has_ip` | 两侧均须为 `true` |
| `sta_ssid` | attributes | BLE `network.status.sta_ssid`（正式配置） | attributes 非空且与 BLE 相等 |
| `sta_ip` | BLE `network.status.sta_ip` | attributes 仅记录，不强制相等 | BLE 非空；attributes 陈旧不判失败 |
| 设备身份 | BLE `device.status` | — | `device_id`/`firmware`/`build_sha` 全部匹配，写前 fail-closed |
| 操作终态 | BLE `operation.get` | `network.tryconnect` 受理回包 | `state=failed` 且 `error ∈ {wrong_password, ap_not_found}` |
| 恢复 | BLE `device.status.network` / `network.status` | — | STA 在 60 s 内恢复 |

Contract 依据（`contract/v1/commands.json`、`states.json`、`errors.json`）：

- 不存在 `device.get` 命令；设备身份来自 `device.status.data` 的 `device_id`/`firmware`/`build_sha`。
- `operation.get.data.state` 的 terminal_states 为 `success/failed/cancelled/expired`；`error` 为扁平字符串；`wrong_password`/`ap_not_found` 属 `op_terminal`。
- `network.status.data` 含 `net_mode`、`sta_ssid`、`connected_ssid`、`sta_has_ip`、`sta_ip`、`sta_connected`、`mqtt_connected`、`trial`。

TB REST 依据（2026-11-28 只读实测）：

- telemetry `values/timeseries` 返回 `{key: [{ts, value}, ...]}`；`net_mode`/`uart_baudrate`/`sta_has_ip` 约 10 s 一拍、时间戳新鲜。
- attributes `values/attributes` 返回 `[{key, value, lastUpdateTs}, ...]`；未变化值的 `lastUpdateTs` 可能保持旧值（**推断，未确认**），因此 attributes 只作最后上报值，不作窗口内样本。

## 3. 修订判据

前提：BLE 登录成功；写前身份匹配；`trial=idle`、`mqtt_connected=true`、`sta_connected=true` 连续稳定 10 s；REST observer 登录与基线读取成功。

| ID | 判据 | 观测点 |
|---|---|---|
| C15-2-a | 错误口令 `network.tryconnect` 被受理并返回 `operation_id` | 命令回包 |
| C15-2-b | 终态 `failed`，错误码 `wrong_password` 或 `ap_not_found` | `operation.get` ≤40 s |
| C15-2-c | STA 在 60 s 内恢复，写后身份仍匹配 | `device.status` |
| C15-2-d | telemetry 基线：操作前每键存在样本，`op_start - ts ≤ 30 s` | REST timeseries |
| C15-2-e | telemetry 窗口内样本：每键至少 1 条，`ts ∈ [op_start - 5 s, op_end + 15 s]`，且不在未来 | REST timeseries |
| C15-2-f | 窗口内 telemetry 自洽：`net_mode=sta`、`uart_baudrate>0`、`sta_has_ip=true` | REST timeseries |
| C15-2-g | BLE 状态：`net_mode=sta`、`sta_connected=true`、`sta_has_ip=true`、`sta_ip` 非空 | `network.status` |
| C15-2-h | attributes：`sta_ssid` 非空且等于 BLE `network.status.sta_ssid`；`sta_ip` 仅记录 | REST attributes + BLE |

说明：

- 时间容差：`CLOCK_SKEW_S=5`、`MAX_DATA_DELAY_S=15`、`BASELINE_MAX_AGE_S=30`；窗口上下界含端点。
- 无基线 → `NOT_COVERED/baseline_missing`；基线超龄 → `NOT_COVERED/baseline_stale`；窗口内某键无样本 → `NOT_COVERED/telemetry_no_sample_<key>`。
- attributes 陈旧（`lastUpdateTs` 早于读取超过 `ATTR_STALE_WARN_S=600`）只记录 `attrs_stale=true`，不单独判失败；`sta_ssid` 以 BLE 当前配置交叉核对。
- `sta_ip` 不以 attributes 与当前 IP 强制相等：attributes 可能停留在旧 IP；运行态以 BLE `network.status.sta_ip` 为准。

## 4. 三类失败分离

| 类别 | 触发 | 结果字段 | 判定 |
|---|---|---|---|
| observer failure | REST 登录/查询失败、401/403/404/429/5xx、超时、网络错误、JSON/结构错误 | `observer.state=error`、`observer.error.code` | `STOP/observer_<code>`；**不得**写成设备操作失败 |
| device observation failure | BLE `device.status`/`network.status`/`operation.get` 读取超时或非法响应 | `device.errors[]` | `STOP/device_observation_<code>` |
| operation result | `network.tryconnect` 受理、`busy`、拒绝、终态 | `operation.*` | `busy→NOT_COVERED`；拒绝→`FAIL`；终态不符→`FAIL` |

写命令 transport 异常/超时仍为 `STOP/write_outcome_unknown`，只发送一次，不重发。

## 5. 判定优先级

1. observer failure → `STOP/observer_*`
2. device observation failure → `STOP/device_observation_*`
3. 写前身份不匹配 → `STOP/identity`
4. 就绪门禁失败 → `STOP/readiness`
5. 写 deadline 耗尽 → `STOP/write_deadline_exhausted_before_send`
6. 写 outcome unknown → `STOP/write_outcome_unknown`
7. `busy` → `NOT_COVERED/tryconnect_busy`
8. tryconnect 被拒 → `FAIL/tryconnect_rejected`
9. 终态不符 → `FAIL/terminal_state`
10. STA 未恢复 → `FAIL/sta_not_restored`
11. 写后身份不匹配 → `FAIL/identity_mismatch`
12. 基线缺失/超龄 → `NOT_COVERED/baseline_missing|baseline_stale`
13. 窗口内无样本 → `NOT_COVERED/telemetry_no_sample_*`
14. telemetry 自洽失败 → `FAIL/telemetry_inconsistent`
15. BLE 状态失败 → `FAIL/status_inconsistent`
16. attributes 失败 → `FAIL/attributes_inconsistent`
17. 全部通过 → `PASS/ok`

补充：终态等待 40 s 到期仍未进入 terminal_states → `FAIL/terminal_state`（操作结果不明确，不算 device observation 失败）；BLE 读在重试后仍超时 → `STOP/device_observation_read_timeout`。

## 6. Harness 结构

| 文件 | 职责 |
|---|---|
| `tools/review/c152_policy.py` | 纯判定：timeseries/attributes 解析、窗口、基线、自洽、状态/属性交叉核对、决策表；无 I/O |
| `tools/review/c152_rest_observer.py` | REST 只读适配层：登录、timeseries、attributes；HTTP 状态/异常映射；可注入 fake transport |
| `tools/review/c152_ble.py` | BLE 只读命令封装：全局唯一 ID、deadline、身份门、就绪门禁、终态/STA 恢复轮询 |
| `tools/review/c152_telemetry_rest.py` | 执行器：BLE 登录/写命令；REST 观测；三类失败分级与 JSON |
| `tests/c152_policy/` | Host 测试：mock REST、fake BLE session、fake 时钟；不连接设备/TB |

- 删除对 paho/MQTT 的依赖；`c152_telemetry_rest.py` 不得引用设备 Access Token。
- REST 凭据仅从 `IOTHUB_TB_USER`/`IOTHUB_TB_PASSWORD` 读取；`IOTHUB_TB_REST_BASE`、`IOTHUB_TB_DEVICE_ID` 由运行参数提供。
- REST 观测不做自动重试：单次失败即 `STOP/observer_*`（避免观测失败被误当设备结果）。
- 就绪门禁读 `device.status.network` 的 `trial`/`mqtt_connected`/`sta_connected`（契约已列），不依赖 `network.status.trial`。
- 所有 BLE 请求仍用全局单调 ID；REST 查询不参与 BLE ID 空间。

## 7. Host 测试矩阵

| # | 场景 | 期望 |
|---|---|---|
| T1 | 合法 PASS 路径 | `PASS/ok` |
| T2 | telemetry 窗口零样本 | `NOT_COVERED/telemetry_no_sample_*` |
| T3 | 基线缺失/超龄 | `NOT_COVERED/baseline_*` |
| T4 | 窗口边界（含端点） | 端点计入 |
| T5 | 样本乱序 | 排序后判定 |
| T6 | `net_mode=ap`/非法值 | `FAIL/telemetry_inconsistent` |
| T7 | `uart_baudrate` 缺失/0/非数值 | `FAIL/telemetry_inconsistent` |
| T8 | `sta_has_ip=false` | `FAIL/telemetry_inconsistent` |
| T9 | BLE `sta_has_ip=false` | `FAIL/status_inconsistent` |
| T10 | BLE `net_mode` 非 sta | `FAIL/status_inconsistent` |
| T11 | attributes `sta_ssid` 缺失/空 | `FAIL/attributes_inconsistent` |
| T12 | attributes/BLE SSID 不一致 | `FAIL/attributes_inconsistent` |
| T13 | attributes 陈旧但一致 | `PASS`，记录 `attrs_stale=true` |
| T14 | attributes `sta_ip` 与当前不同 | `PASS`（不强制相等） |
| T15 | 终态 `success` | `FAIL/terminal_state` |
| T16 | `busy` | `NOT_COVERED/tryconnect_busy` |
| T17 | REST 401/403/404/429/5xx/超时 | `STOP/observer_*`，operation 结果仍记录 |
| T18 | REST 返回非法 JSON/结构 | `STOP/observer_protocol` |
| T19 | BLE 读超时（写后） | `STOP/device_observation_*`，operation 结果仍记录 |
| T20 | 写 transport 异常/超时 | `STOP/write_outcome_unknown`，仅 1 次写 |
| T21 | 写 deadline 耗尽 | `STOP`，0 次写 |
| T22 | 身份不匹配（写前） | `STOP/identity`，0 次写、0 次 REST 写前基线 |
| T23 | 写后身份不匹配 | `FAIL/identity_mismatch` |
| T24 | 就绪门禁真实时间/间隔重置 | ≥10 s 才通过；间隔 >3 s 清零 |
| T25 | 全局唯一请求 ID/迟到响应 | 跨轮询不重复；迟到响应不满足下一次 |
| T26 | 退出码 | 0/1/2/3 |
| T27 | 源码不含 paho/MQTT token 逻辑 | `ast`/文本检查通过 |
| T28 | observer 失败不判 FAIL | verdict=`STOP` |
| T29 | 窗口内样本在未来时间 | 不计入窗口/`NOT_COVERED` |
| T30 | 三个 telemetry 键分别缺失 | 各自的 `telemetry_no_sample_<key>` |
| T31 | 40 s 内始终非终态 | `FAIL/terminal_state`（不是 device observation） |
| T32 | 就绪读取使用 `device.status.network` | 契约字段，不依赖 `network.status.trial` |

## 8. 未授权与不改变项

- 不执行设备连接、BLE 写命令、REST 实际查询、烧录或配置修改（Host 验收阶段）。
- 不改变 C15-2 `NOT_COVERED`、CMD-15 `TEST_PENDING`、历史 BUSY 未解释的事实。
- 本方案通过仅证明观测链路有效，不解释历史 BUSY。
