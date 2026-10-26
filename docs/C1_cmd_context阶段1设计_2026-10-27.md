# C1 · 命令路由阶段 1 设计：可信来源上下文（评审稿，未实现）

> 日期：2026-10-27 · 基线：`feature/shared-config-cmd @ 5045174`（A 阶段签收 `4bb492d` 之上）
> 评审结论（2026-10-26）：阶段 1 方向通过；本稿把评审意见落成可执行设计。
> **阶段 1 目标**：引入 `cmd_context_t`、`CMD_SOURCE_UNKNOWN` 零值默认拒绝、显式入口注入、内部派生显式标记；**不改变任何权限行为**。
> **非目标**：策略表（阶段 2）、异步归属快照/通道 epoch 生效（阶段 3）、审计与单一命令队列（阶段 4）。
> 前置事实（已核实）：`app_cmd_execute_ex` 调用点共 4 处——`mqtt.c:734`、`ble_cmd.c:712`、`state.c:150`、`cmd.c:397`（内部再入）；MQTT RPC 入口无应用层 token（鉴权在 broker 连接层）。

---

## 1. 枚举调整（含重编号依据）

```c
typedef enum {
    APP_CMD_SOURCE_UNKNOWN  = 0,   /* 零值默认拒绝:未注入/未初始化一律不可信 */
    APP_CMD_SOURCE_MQTT     = 1,
    APP_CMD_SOURCE_BLE      = 2,
    APP_CMD_SOURCE_HTTP     = 3,   /* 服务已删,枚举保留(代码/注释仍在引用语义) */
    APP_CMD_SOURCE_TOUCH    = 4,
    APP_CMD_SOURCE_SERIAL   = 5,   /* 预留:承载未定,不接线、不注入 */
    APP_CMD_SOURCE_INTERNAL = 6,   /* 内部派生执行:只作 exec_env/origin 语义,不作入口值 */
} app_cmd_source_t;
```

- **重编号安全依据**：`app_cmd_source_t` 是纯 RAM 运行时值，不落 NVS、不进 BLE/MQTT 线格式、不进日志解析契约；全仓仅 4 个调用点 + `cmd.c` 内部比较分支，全部显式传值。逐点核对过：`s_net_trial.owner` / `s_net_async.owner` 先 `memset` 后**在读取前显式赋值**（`app_cmd_net_tryconnect_async` 内 `s_net_async.owner = source`，`run_fn` 才读 `job->owner`），不存在"依赖 0==MQTT 的隐式默认"路径。
- 行为不变性：现有 11 处 `source == APP_CMD_SOURCE_MQTT` / `source != HTTP && != BLE` 比较只依赖常量身份，重编号不改变任何比较结果。
- `INTERNAL` 不是入口：任何入口不得注入 `INTERNAL` 作为 `source`；它只出现在内部派生上下文的 `exec_env`（和快照的 `origin` 对照里）。

## 2. `cmd_context_t` 字段表

| 字段 | 类型 | 语义 | 注入者 | 阶段 1 读取者 |
| --- | --- | --- | --- | --- |
| `source` | `app_cmd_source_t` | 本请求直接入口 | 入口构造，之后只读 | dispatch 的 source 分支（语义不变） |
| `origin` | `app_cmd_source_t` | 因果链最初入口；内部任务**不得改写** | 入口构造；派生时复制 | `exec_env=INTERNAL` 时的 io 归属（见 §3） |
| `exec_env` | `app_cmd_exec_env_t`（ENTRY/INTERNAL） | 当前代码运行上下文 | 入口=ENTRY；派生=INTERNAL | 内部再入标记、日志 |
| `auth_level` | `app_cmd_auth_level_t`（`UNAUTH=0`/`LOCAL_TOKEN=1`） | 认证等级，**不从 source 推导** | transport 填写 | **阶段 1 无人读取**（信息性；阶段 2 策略表消费） |
| `depth` | `uint8_t` | 派生深度；上限 `APP_CMD_CTX_DEPTH_MAX = 2` | 入口=0；派生=父+1 | 派生构造器（超限拒绝） |
| `trace_id` | `uint32_t` | 日志关联计数（模块内自增，不持久） | 构造器自动填 | 日志行 |
| `channel_epoch` | `uint32_t` | 通道代际；**阶段 1 恒为 0（占位）** | 入口传 0 | 无人读取；阶段 3 接管（BLE 断连计数/MQTT client 代数） |

不变量（构造器强制，host 单测断言）：

1. 入口构造：`source == origin`、`exec_env == ENTRY`、`depth == 0`、`source != UNKNOWN && source != INTERNAL`。
2. 派生构造（`app_cmd_ctx_internal_child(parent)`）：`origin` 复制自父、`exec_env = INTERNAL`、`depth = parent.depth + 1`、超上限返回失败。
3. `source == UNKNOWN` 的上下文不接受（执行入口直接拒绝，映射 `internal_error`）。
4. `origin` 是追踪信息不是授权凭据：`exec_env=INTERNAL` 不提升任何权限（阶段 1 无策略表，本条以测试+注释钉死）。

## 3. API 与兼容

```c
/* cmd_context.h(新文件,沿用 auth_token.c 先例:纯 C、host 可编译、CMake 加一行) */
esp_err_t app_cmd_execute_ctx(const app_cmd_context_t *ctx, const cJSON *frame,
                              cJSON **resp_out, app_cmd_result_t *out,
                              const char **err_msg);

/* 旧签名保留为 wrapper:四个现有调用点零改动即可编译 */
esp_err_t app_cmd_execute_ex(const cJSON *frame, app_cmd_source_t source,
                             cJSON **resp_out, app_cmd_result_t *out,
                             const char **err_msg);   /* = execute_ctx(ctx_entry(source), ...) */
```

- **io 归属派生规则**（行为等价的关键）：`app_cmd_io_source(ctx)` = `exec_env==INTERNAL ? io(origin) : io(source)`。今天的内部 apply 再入硬编码 `SOURCE_BLE`，io 归属 BLE；改造后 `origin=受理时 owner`，owner 现只可能是 BLE（HTTP 已删、MQTT 被 tryconnect 守卫拒绝），归属不变。
- **`cmd.c:397` 内部再入改造**：`app_cmd_net_apply_async` 保存的 job 已含 `owner`（值语义）；改为 `ctx = ctx_internal_child(受理时 ctx)`。需要把受理时的 origin 存进 `s_net_async`（`app_cmd_source_t origin`，一个字段），这是本阶段唯一的数据结构新增。
- 例外：`web.auth.set` 成功后的 `clear_all`、token store 不在本阶段触碰。

## 4. 入口注入点清单

| # | 入口 | 位置 | source | exec_env | 说明 |
| --- | --- | --- | --- | --- | --- |
| 1 | MQTT RPC | `mqtt.c:734` | MQTT | ENTRY | topic `v1/devices/me/rpc/request/+` 或 `iothub/rpc`；限频在 transport，不入 context |
| 2 | BLE CMD | `ble_cmd.c:712` | BLE | ENTRY | token/限频在 transport（阶段 1 不动 A 阶段 token store） |
| 3 | TOUCH | `state.c:150` | TOUCH | ENTRY | GPIO/定时器上下文，现状即"入口任务内执行" |
| 4 | 内部 apply | `cmd.c:397` | —（source 语义上不存在） | INTERNAL | origin=受理 owner；depth+1 |
| — | SERIAL | 无 | 预留 | — | UART0 救援通道已取消；承载未定前不接线 |

阶段 1 实际改动点：`cmd.h`（枚举+context 类型+新 API 声明）、新 `cmd_context.c/h`、`cmd.c`（io 派生规则 + 内部再入两处 + `s_net_async.origin`）、**四个入口调用点保持旧签名不变**（wrapper 已注入）。可选收紧：把 4 个入口改为显式 `ctx_entry(...)` 构造，行为相同——本稿按"最小改动"处理，入口显式化留给实现评审时定。

## 5. 契约测试（golden 先行）

### 5.1 host 单测 `tests/cmd_context`（纯 C，Makefile 沿用 token_store 模式）

- UNKNOWN 上下文被 `execute_ctx` 拒绝；
- 入口构造不变量 4 条；
- 派生构造：depth 0→1→2 通过、第 3 层拒绝；origin 不可改写；
- io 派生规则：`INTERNAL` 取 origin、`ENTRY` 取 source（含 TOUCH/HTTP/MQTT 全枚举）。

### 5.2 设备侧 golden 捕获 `tools/ble_p0/cmd_contract_golden.py`

**先于任何阶段 2 改动运行一次，把"旧规则"落成只读基线；阶段 2 重跑必须逐字节等价。**

- **BLE 腿（bleak）**：方法 × {无 token、有效 token、错误 token、小写 token} 矩阵，断言 `ok/error` 文本；覆盖 §6 清单全部 BLE 可达项。
- **MQTT 腿（本地 broker 方案，不依赖云端凭据）**：
  1. PC 起 mosquitto（匿名）；
  2. BLE `config.set mqtt_host/mqtt_port` 指向 PC（BLE 允许写 MQTT 域，现状规则）；
  3. PC paho（venv 已有 2.1.0）发布 RPC 到 `v1/devices/me/rpc/request/1`、订阅 `.../response/1`；
  4. 断言 MQTT 来源的拒绝响应（`guard_rejected` / `forbidden`）与放行响应（GPIO 域 config.set、IO 控制类）；
  5. **测试后必须恢复 mqtt_host 并验证 MQTT 回云**（`mqtt_connected=true`），恢复动作写进脚本 finally。
- 结果文件 `tools/ble_p0/results/cmd_contract_golden_<ts>.json`，口令/token 脱敏（沿用 `mask_key`）。
- 两种编译配置都跑：`APP_CLOUD_ALLOW_NET_CMDS=0`（默认）为主；`=1` 作为补充记录（避免"策略表存在但被宏短路"的假等价，阶段 2 引用）。

## 6. 行为不变性清单（阶段 1 验收判据）

阶段 1 无新策略，验收判据 = **改造前后响应完全一致**（golden 对比）。逐项：

1. MQTT `config.set` 改 `sta_ssid` → `guard_rejected`（不变）。
2. MQTT `config.set` 改 `led_power_policy`（GPIO）→ 成功（不变）。
3. MQTT `config.set` 改 `data_route_uart1`（DATAPIPE）→ 成功（不变）。
4. MQTT `network.tryconnect`/`network.scan`/`profile.delete`/`restart`/`uart.send`/`data.loopback`/`web.auth.set` → `guard_rejected`（不变）。
5. MQTT `provisioning.pin.set` → **`forbidden`**（与 4 的错误码差异保留，不统一）。
6. BLE `config.set` 五域 → 成功（不变）。
7. BLE `provisioning.pin.set` → 走现有参数/口令校验路径（不变）。
8. TOUCH `io.set` 等 → 允许（不变）。
9. apply 内部再入：trial 校验路径、成功/失败语义、**io 事件归属仍为 BLE**（串口 `APP_IO_SOURCE_BLE` 日志佐证）。
10. T2 槽位/淘汰/600s、A 阶段 token 行为、`sessions_invalidated`、错误码全表逐字节不变。

## 7. 风险与回滚

| 风险 | 处理 |
| --- | --- |
| 重编号后存在未发现的隐式 0 依赖 | §1 已逐点核对 memset/赋值顺序；host 单测 + golden 双保险；再发现即 revert |
| 内部再入 io 归属漂移 | §3 派生规则 + 不变性第 9 条串口佐证 |
| golden 的 MQTT 腿改 mqtt_host 造成设备失联 | 脚本 finally 强制恢复 + 回云验证；恢复失败显式 FAIL 并提示 BLE 重配 |
| 为 SERIAL 过早设计 | SERIAL 仅枚举占位，无注入点、无策略条目 |
| 回滚 | 阶段 1 为加法（新文件 + wrapper）+ 两处内部点改造；单 commit revert 即回到 `5045174` 行为 |

## 8. 阶段 2/3 接口预留（不展开，仅锚点）

- **阶段 2**：策略表读取 `ctx.source`（+ `config.set` 的 domain 细分）；`min_auth` 读 `auth_level`；`APP_CLOUD_ALLOW_NET_CMDS` 转为策略编译期条目；验收 = §5 golden 逐字节等价。
- **阶段 3**：`channel_epoch` 由各通道提供（BLE 断连计数 / MQTT client 代数）；异步归属快照 `cmd_origin_snapshot_t`（字段沿用评审建议：source/origin/trace_id/request_id/parent_req_id/channel_epoch/depth + 固定长 identity 摘要，不持通道指针）；回写前验 epoch，失效丢弃并计数；结果记录与响应发送分离（`operation.get` 永远可查）。
