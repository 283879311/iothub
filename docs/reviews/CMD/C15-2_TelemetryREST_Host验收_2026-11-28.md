# C15-2 Telemetry REST 混合观测 Host 验收（2026-11-28）

> 文档状态：CURRENT（Host 级验收记录，rev2）。
> 结果等级：`HOST_VERIFIED`，仅限观测/判据/harness 的 Host 测试；**不含**真实 REST、BLE 或设备运行。
> 基线：`feat/c152-mqtt-criterion-20261128 @ 8d7c430`（保留不覆盖）。
> 实现分支：`feat/c152-telemetry-rest-observer-20261128 @ 11727a3`（rev2；rev1 为 `caeb92e`）。

## 1. 范围

- 方案：**Telemetry REST + Attributes + BLE 混合观测**。
- 本阶段完成：字段映射与判据修订、REST 只读适配层、BLE 只读封装、混合判据、Host 测试。
- 本阶段不做：真实 TB REST 查询、BLE 连接、设备写命令、烧录、配置修改、`main` 合入。

## 2. 字段映射与 Contract 核对

| 字段 | 首选来源 | 交叉核对 |
|---|---|---|
| `net_mode` | telemetry timeseries | BLE `network.status.net_mode` |
| `uart_baudrate` | telemetry timeseries | — |
| `sta_has_ip` | telemetry timeseries | BLE `network.status.sta_has_ip` |
| `sta_ssid` | attributes | BLE `network.status.sta_ssid`（正式配置） |
| `sta_ip` | BLE `network.status.sta_ip` | attributes 仅记录，不强制相等 |
| 设备身份 | BLE `device.status` | `device_id`/`firmware`/`build_sha` |
| REST 设备身份 | `GET /api/device/{uuid}` | `id`/`name` 匹配声明；与 BLE `device_id` 同名或显式 alias |
| 操作终态 | BLE `operation.get` | `state=failed` + `wrong_password`/`ap_not_found` |

Contract 核对结论：

- Contract 中**没有 `device.get`**；身份字段来自 `device.status.data`。
- `operation.get` 的 terminal_states 为 `success/failed/cancelled/expired`；`wrong_password`/`ap_not_found` 属 `op_terminal`。
- `network.status` 提供 `net_mode`/`sta_ssid`/`connected_ssid`/`sta_has_ip`/`sta_ip`。
- 就绪门禁改用 `device.status.network` 的 `trial`/`mqtt_connected`/`sta_connected`（契约已列），不依赖 `network.status.trial`。

## 3. 实现

| 文件 | 职责 | SHA256 |
|---|---|---|
| `tools/review/c152_policy.py` | 纯判定（431 行） | `a5189fd59acb6796c813c52dc8db5712f3ce14d89f2a58e93ffc38d67a371675` |
| `tools/review/c152_rest_observer.py` | REST 只读适配层（148 行） | `6654db7967e68d29b1bdd617237c2cb104c40848e7670dc23d8010e8a04d8940` |
| `tools/review/c152_ble.py` | BLE 只读命令封装（201 行） | `e60c0be6588e703a8af9e1dca6e87bd8c9de0e72ac88416a5cabdb9ccfb8b3db` |
| `tools/review/c152_telemetry_rest.py` | 执行器（396 行） | `a9de0721676b4cac47624297437c2856c70346d144740c77d71d45dbad15c37a` |
| `tests/c152_policy/test_c152_policy.py` | 纯判定测试（401 行） | `a2a7c38591a34f515f44fe5cffb05c9eefceef4f21db557bf1b9859301cb3544` |
| `tests/c152_policy/test_c152_ble.py` | BLE 读测试（164 行） | `0858e5d303bcb093659cc053422c709536f44847ed66c2af1d016d09673da8c1` |
| `tests/c152_policy/test_c152_runner.py` | 执行器测试（533 行） | `ea8ec0f9d0fe2e976710e8f8aab68e91327ed5eca65a5d8ce9b4532ffcad0fc9` |
| `tests/c152_policy/test_c152_evidence.py` | 新鲜度/身份测试（110 行） | `28dcd381d13eabea2c2a03a576e84555116d7d230231556f140a7811a434b88f` |
| `tests/c152_policy/test_c152_rest.py` | REST 映射/隔离测试（106 行） | `dddf190014358e7f0aa86799fdfae6e9fa2d95f2b0d37715fdcbccd90384bb29` |
| 设计文档 | 判据/字段/矩阵（约 160 行） | `58d8f68f0a93224aaffb2a59ffeb08bf83058d40756ed28290ebee7832390ce4` |

- 删除 `c152_mqtt_attrs.py`（设备 Access Token + 第二 MQTT 会话路径）。
- 新模块不引用 paho/MQTT、不订阅 `v1/devices/me/*`、不读取设备 token。
- REST 认证用 `POST /api/auth/login`；业务观测仅 GET（device/timeseries/attributes），不调用配置或数据写入接口。
- 凭据/JWT 只在内存使用，不写入日志、结果或测试；客户账号查询成功不等于已验证其全部权限只读。

## 4. 阶段 A 独立复验（rev2）

在从 `11727a3` 检出的干净 detached worktree 执行：

| 命令 | 结果 |
|---|---|
| `git rev-parse HEAD` | `11727a30afb732a2c3e46b4dc5b4718fa11dddee` |
| `git status --short` | 空（clean tree） |
| `make -C tests/c152_policy run` | **Ran 104 tests, OK** |
| `python3 -m py_compile tools/review/*.py tests/c152_policy/*.py` | 通过 |
| `git diff --name-only 8d7c430 11727a3 -- main contract CMakeLists.txt` | 空（无生产/契约变更） |
| 源码扫描 `paho|v1/devices/me|IOTHUB_MQTT_TOKEN` | 新执行器/适配层无命中（policy 文档字符串保留主题名说明） |

变更范围（相对 `8d7c430`）：`docs/`、`tests/c152_policy/*`、`tools/review/{README.md,c152_*.py}`；无 `main/`、`contract/`、构建配置变更。

证据：`/tmp/c152_stage_a_rev2.log`，SHA256 `0d6169c74a2ea5d265409be2d8bc08a4de761573451fcb10b7906d9415e36777`。

## 5. Host 覆盖（104 项）

| 组 | 覆盖 |
|---|---|
| 解析/基线 | ms/s 时间戳、乱序、端点包含、未来样本剔除、每键缺失、基线缺失/超龄 |
| **样本新鲜度** | 基线样本不得复用；`ts > baseline_ts`；基线后 1 ms；服务器时钟落后主机 8 s；上界含端点/超 1 ms 排除；只有写前样本 → `telemetry_no_fresh_sample_*` |
| telemetry 自洽 | `net_mode` ap/非法、`uart_baudrate` 0/文本/bool、`sta_has_ip` false/非法 |
| BLE 状态 | `net_mode` 非 sta、未连接、`sta_has_ip` false、IP/SSID 空 |
| attributes | 缺失/空/SSID 不一致、陈旧仅记录、`sta_ip` 差异不判失败 |
| REST 失败 | 401/403/404/429/5xx/超时、非法 JSON/结构、写前登录失败、写后失败 |
| **REST 身份** | UUID 不符、名称缺失/不符、device 读取失败；均在写前 STOP，写 0 次 |
| BLE 失败 | 写后状态读超时、`operation.get` 超时、畸形响应；均不写成 operation 失败 |
| 操作结果 | `busy`、拒绝、终态 `success`、40 s 未终态 |
| 写门禁 | transport 异常仅 1 次、deadline 耗尽 0 次、慢写取消 |
| 身份 | 写前/写后 BLE 不匹配、字段缺失、写前不启动 observer |
| 隔离 | 全局唯一 ID、迟到响应、无 MQTT/token 逻辑、退出码 |

## 6. 未覆盖与限制

- 未执行真实 TB REST 查询；测试只证明映射与判定逻辑。
- TB 对未变化 attributes 保留旧 `lastUpdateTs` 仍是**推断**（UNCONFIRMED）；判据不依赖 attributes 时间戳。
- 新鲜度：窗口内样本以 `ts > baseline_ts` 判定，上界用 `monotonic` 实测 elapsed；**基线本身的新鲜度**仍用主机时间 `op_start - ts ≤ 30 s`。若主机/TB 偏差超过 30 s，会保守地 `NOT_COVERED`（不会误 PASS）；Stage B 必须记录时间偏差。
- TB 名称与 BLE `device_id` 的绑定是**操作者声明**：同名自动通过，异名必须 `--allow-alias-binding`；结果记录 `binding.kind=same_name|operator_alias`。C1/65D8 属 alias，不能据此宣称二者身份已被密码学证明。
- REST 不做自动重试：单次 5xx/429 即 `STOP/observer_*`；是否加有界重试留待评审。
- `operation.get` 40 s 未终态判 `FAIL/terminal_state`；BLE 读重试仍超时判 `STOP/device_observation_read_timeout`。
- 未连接设备、未发送任何 BLE 写命令、未访问 TB、未烧录、未改配置。

## 7. 状态

| 项 | 状态 |
|---|---|
| 原基线 `8d7c430` | 保留，未覆盖 |
| 新观测 harness | rev2 完成，Host 104/104 |
| `main/` / `contract/` | 无变更 |
| 客户凭据 | 未进入代码/日志/测试报告 |
| 设备操作 | 未执行 |
| C15-2 | `NOT_COVERED`（不变） |
| CMD-15 | `TEST_PENDING`（不变） |
| 历史 BUSY | 未解释（不变） |
| 阶段 B 设备复测 | 未启动，需单独授权 |
| 阶段 C 合入 | 暂缓 |

## 8. 修订记录

| 版本 | 提交 | 结果 | 说明 |
|---|---|---|---|
| rev1 | `caeb92e` | 87/87 Host PASS | 首轮实现；review 发现样本新鲜度与 REST/BLE 身份绑定缺口 |
| rev2 | `11727a3` | **104/104 Host PASS** | 修复：每键 `baseline_ts` 严格新鲜过滤、`telemetry_no_fresh_sample_*`、`elapsed` 上界、REST `device` 身份前置核验、alias 绑定声明、认证表述修订 |

rev1 的 87/87 与 rev2 的 104/104 不是同一代码状态；rev2 的测试全部在 rev1 基础上新增/改写，必须整体运行，不能用 87 项结果替代。
