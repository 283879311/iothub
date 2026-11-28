# C15-2 Telemetry REST 混合观测 Host 验收（2026-11-28）

> 文档状态：CURRENT（Host 级验收记录）。
> 结果等级：`HOST_VERIFIED`，仅限观测/判据/harness 的 Host 测试；**不含**真实 REST、BLE 或设备运行。
> 基线：`feat/c152-mqtt-criterion-20261128 @ 8d7c430`（保留不覆盖）。
> 实现分支：`feat/c152-telemetry-rest-observer-20261128 @ caeb92e`。

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
| 操作终态 | BLE `operation.get` | `state=failed` + `wrong_password`/`ap_not_found` |

Contract 核对结论：

- Contract 中**没有 `device.get`**；身份字段来自 `device.status.data`。
- `operation.get` 的 terminal_states 为 `success/failed/cancelled/expired`；`wrong_password`/`ap_not_found` 属 `op_terminal`。
- `network.status` 提供 `net_mode`/`sta_ssid`/`connected_ssid`/`sta_has_ip`/`sta_ip`。
- 就绪门禁改用 `device.status.network` 的 `trial`/`mqtt_connected`/`sta_connected`（契约已列），不依赖 `network.status.trial`。

## 3. 实现

| 文件 | 职责 | SHA256 |
|---|---|---|
| `tools/review/c152_policy.py` | 纯判定（350 行） | `3a8d2fe99209adaaea37c26cb12bd05a163083b373d43116a48b6ac6ae7aaaa4` |
| `tools/review/c152_rest_observer.py` | REST 只读适配层（138 行） | `1c296852bb00fffb6b5e528436e475d2205731f18fa7fab218397e829e2bd622` |
| `tools/review/c152_ble.py` | BLE 只读命令封装（201 行） | `e60c0be6588e703a8af9e1dca6e87bd8c9de0e72ac88416a5cabdb9ccfb8b3db` |
| `tools/review/c152_telemetry_rest.py` | 执行器（342 行） | `741814db9f8f07188823a95595870cdb895fa42093c92bce9d97d01f09326659` |
| `tests/c152_policy/test_c152_policy.py` | 纯判定测试（330 行） | `45d93d5a65d2ad904b836823903119da97c317e843606318f2a6ac67fa692f82` |
| `tests/c152_policy/test_c152_ble.py` | BLE 读测试（164 行） | `0858e5d303bcb093659cc053422c709536f44847ed66c2af1d016d09673da8c1` |
| `tests/c152_policy/test_c152_runner.py` | 执行器测试（592 行） | `20496486657b220da3f680968f5b1b6b00a964e5410b1f5e6533754b87eb34f1` |
| 设计文档 | 判据/字段/矩阵（150 行） | `1462f96fac94b0f4fa2144ed556a5a7e6de7287b25c4ddf7acd8fc0c43e2d5ae` |

- 删除 `c152_mqtt_attrs.py`（设备 Access Token + 第二 MQTT 会话路径）。
- 新模块不引用 paho/MQTT、不订阅 `v1/devices/me/*`、不读取设备 token。
- REST 凭据只从 `IOTHUB_TB_USER`/`IOTHUB_TB_PASSWORD` 读取；测试全部用 fake transport。

## 4. 阶段 A 独立复验

在从 `caeb92e` 检出的干净 detached worktree 执行：

| 命令 | 结果 |
|---|---|
| `git rev-parse HEAD` | `caeb92edb718cbea40540687d678f6d6cba4bc3e` |
| `git status --short` | 空（clean tree） |
| `make -C tests/c152_policy run` | **Ran 87 tests, OK** |
| `python3 -m py_compile tools/review/*.py tests/c152_policy/*.py` | 通过 |
| `git diff --name-only 8d7c430 caeb92e -- main contract CMakeLists.txt` | 空（无生产/契约变更） |
| 源码扫描 `v1/devices/me|paho|mqtt.client` | 新模块无命中 |
| 凭据扫描（customerC/ApiKey/139.9.93.245/21883/IOTHUB_MQTT_TOKEN） | 新文档/新代码无命中；仅 `tools/review/README.md` 的 v9 历史段落保留旧说明 |

变更范围（相对 `8d7c430`）：`docs/README.md`、两份 C15-2 设计文档、`tests/c152_policy/*`、`tools/review/{README.md,c152_*.py}`；无 `main/`、`contract/`、构建配置变更。

证据：`/tmp/c152_stage_a.log`，SHA256 `bfb4fb3ebd785d683e61e73221375905d9f637b41466970b73fd0de5e59ee08d`。

## 5. Host 覆盖（87 项）

| 组 | 覆盖 |
|---|---|
| 解析/窗口/基线 | ms/s 时间戳、乱序、端点包含、未来样本剔除、每键缺失、基线缺失/超龄 |
| telemetry 自洽 | `net_mode` ap/非法、`uart_baudrate` 0/文本/bool、`sta_has_ip` false/非法 |
| BLE 状态 | `net_mode` 非 sta、未连接、`sta_has_ip` false、IP/SSID 空 |
| attributes | 缺失/空/SSID 不一致、陈旧仅记录、`sta_ip` 差异不判失败 |
| REST 失败 | 401/403/404/429/5xx/超时、非法 JSON/结构、写前登录失败、写后失败 |
| BLE 失败 | 写后状态读超时、`operation.get` 超时、畸形响应；均不写成 operation 失败 |
| 操作结果 | `busy`、拒绝、终态 `success`、40 s 未终态 |
| 写门禁 | transport 异常仅 1 次、deadline 耗尽 0 次、慢写取消 |
| 身份 | 写前/写后不匹配、字段缺失、写前不启动 observer |
| 隔离 | 全局唯一 ID、迟到响应、无 MQTT/token 逻辑、退出码 |

## 6. 未覆盖与限制

- 未执行真实 TB REST 查询；测试只证明映射与判定逻辑。
- TB 对未变化 attributes 保留旧 `lastUpdateTs` 仍是**推断**（UNCONFIRMED）；判据不依赖 attributes 时间戳。
- 时钟容差（5 s/15 s/30 s）未在真机标定；Stage B 需记录样本间隔与主机/TB 时间差。
- REST 不做自动重试：单次 5xx/429 即 `STOP/observer_*`；是否加有界重试留待评审。
- `operation.get` 40 s 未终态判 `FAIL/terminal_state`；BLE 读重试仍超时判 `STOP/device_observation_read_timeout`。
- 未连接设备、未发送任何 BLE 写命令、未访问 TB、未烧录、未改配置。

## 7. 状态

| 项 | 状态 |
|---|---|
| 原基线 `8d7c430` | 保留，未覆盖 |
| 新观测 harness | 完成独立实现，Host 87/87 |
| `main/` / `contract/` | 无变更 |
| 客户凭据 | 未进入代码/日志/测试报告 |
| 设备操作 | 未执行 |
| C15-2 | `NOT_COVERED`（不变） |
| CMD-15 | `TEST_PENDING`（不变） |
| 历史 BUSY | 未解释（不变） |
| 阶段 B 设备复测 | 未启动，需单独授权 |
| 阶段 C 合入 | 暂缓 |
