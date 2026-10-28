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

## 9. R1 评审处置（2026-10-28，评审结论：暂缓合并，P0/P1 完成后复审）

评审四项处置与落点（提交见实验分支 R1 commit）：

| 项 | 处置 | 落点 |
| --- | --- | --- |
| P0 异步父上下文 | **已改**：`s_net_async` 新增 `parent_ctx`，受理（`app_cmd_net_tryconnect_async`，签名改收 `const app_cmd_context_t *ctx`）时整包快照，apply 内部再入从 `&s_net_async.parent_ctx` 派生 child；不再由 `owner` 事后重建，origin/trace/auth 继承 tryconnect 请求真实值 | `cmd.c` job 结构体、受理函数、Hunk A |
| P0 并发论证 | ~~受理与 apply.async 同在 dispatcher 任务串行，worker 不触碰窗口~~ **R2 修正：原论证不成立**——cleanup 在 `s_ops_mux` 临界区外执行，GOT_IP 300s TTL 过期 / BLE 断连取消的 `memset(&s_net_async,…)` 可与已过 COMMITTING 的 apply 再入并发。已加 job 生命周期锁（§9.3），受理写入 / apply 读取与再入 / 全部终态 memset 共用一把互斥 | `cmd.c` `s_net_job_lock`、§9.3 |
| P1 valid 收紧 | 入口集合 = MQTT..SERIAL(1..5)，未定义值(如 99)拒绝；INTERNAL 要求 `source==INTERNAL`（防冒充）且 origin 属入口集合；auth_level 越界、`channel_epoch!=0` 整包拒绝（epoch 恒 0 为阶段 1 占位契约，阶段 3 放宽）；exec_env 非法值天然落入拒绝 | `cmd_context.c` `app_cmd_ctx_valid`、`app_cmd_src_is_entry` |
| P1 等价性复核 | §9.1 分支清单逐项对照（下表），全部读点仅 4 处，行为等价成立 | 下表 |
| P2 trace 原子化 | `_Atomic uint32_t` + `atomic_fetch_add(relaxed)`；host 并发测试 4×500 全唯一；32 位回绕不设防（消费方不假定唯一） | `cmd_context.c`、`t_trace_concurrent` |

测试：host **85/85**（原 60 + R1 新增 25）；固件零告警（app 0x117670）。板上冒烟待 R1 复审通过后在实验构建上重跑（当前设备运行 mainline `1.2-753483f`，不作为阶段 1 证据）。

### 9.1 内部 network.apply 等价性审计（旧:execute_ex(BLE) vs 新:child(parent_ctx 快照)）

内部再入 frame 恒为 `{"method":"network.apply"}`（`app_cmd_net_apply_async` 构造，全仓唯一再入点，`s_net_async_apply_internal` 仅 3 处引用）。`source`/`io_source`/owner 的全部读取点：

| 读取点 | 旧值(BLE) | 新值(INTERNAL/origin) | 终态等价性 |
| --- | --- | --- | --- |
| `network.apply` 守卫 `source==MQTT` → guard_rejected | ≠MQTT，过 | ≠MQTT，过 | 同过 |
| busy 检查 `s_net_async.active && !apply_internal` | flag=true 跳过 | 同（flag 机制未动） | 同 |
| trial 校验（`trial_invalid`/TTL） | 不读 source | 不读 source | 同 |
| config patch / snapshot / profile 保存 / restart | 不读 source | 不读 source | 同 |
| io 事件归属（`app_cmd_io_source`，仅 io.set/led/relay 分支消费） | io_source(BLE) | exec_env=INTERNAL → io(origin)=io(owner 快照)；apply 链路无 io 事件，计算值不被消费 | 不可达/同值 |
| `out.reboot_after_response`（net 域 REBOOT 置位） | 内层 out 被 apply_async 丢弃（先于本改造即如此，属既有行为，source 无关） | 同 | 同（既有） |
| owner 读取：断连取消（owner==BLE）、run 快照、store_trial | 不变 | 不变（P0 只删"事后重建"一读） | 同 |
| 终态路径：SUCCESS/FAILED(storage/rollback/save)/CANCELLED/超时/BLE 断连取消/COMMITTING 放行 | 不读 source | 不读 source | 同 |

唯一新增拒绝路径：快照派生失败（`parent_ctx` 非法）→ `internal_error` → op FAILED；该快照在受理时已被 execute_ctx 校验且仅值拷贝，正常流不可达（fail-loud 兜底）。

### 9.2 阶段 3 锚点更新

阶段 1 起 `valid()` 钉死 `channel_epoch==0`；阶段 3 接通道代数时须**同步放宽该判据并加真实校验**（回写前验 epoch、失效丢弃计数）——此约束已写入 `cmd_context.c` 注释，防止占位契约被静默绕过。

### 9.3 job 生命周期并发协议（R2-P0，回应复审 §8 疑点）

**复审指出的问题属实**：R1 的"dispatcher 串行、worker 不触碰窗口"论证不成立。
`operation.c` 的 `cancel`/`expire_one` 在 `s_ops_mux` 临界区**外**执行
`cleanup_fn`（先置 `expiring=true` 出锁，再跑清理，最后回锁 finalize），
而 `set_state(GOT_IP→COMMITTING)` 的迁移表允许在 cleanup 尚未 finalize 时
成功——存在真实窗口：GOT_IP 300s TTL 过期（worker）或 BLE 断连取消
（NimBLE host 任务）的 `memset(&s_net_async,…)` 与已过 COMMITTING 的
apply 再入并发，撕碎 `parent_ctx`。

**修复：job 生命周期锁 `s_net_job_lock`**（`app_mutex_ensure` 静态非递归互斥，
与 wifi.c restart/scan 锁同一模式），覆盖：

| 访问 | 任务 | 保护方式 |
| --- | --- | --- |
| 受理写入（memset→快照→`operation_start`→op/id 写入） | dispatcher | 持锁区间 |
| apply 再入（child 派生读取 parent_ctx → execute_ctx → 终态 clear_job） | dispatcher | `set_state(COMMITTING)` 成功后取锁，三出口均解锁 |
| cleanup（trial 清理 + job memset） | worker(过期) / NimBLE host(断连取消) | `_sync` 版自取锁；apply 持锁期间在此等待 |
| run 失败路径 memset ×3 | worker | `_sync` 版自取锁 |

**互斥论证**（对应复审四问）：

1. *GOT_IP 下 worker 是否已停*：`run_fn` 成功路径置 GOT_IP 后即返回；
   worker 对本 job 的残余活动只剩 `expire_one`（got_ip_ttl 到期后可选中）。
   该路径的 cleanup 现在与 apply 互斥（锁）。
2. *cancel/超时/断连能否在状态检查后、复制前清理*：dispatcher 复制
   parent_ctx 的前置不是 get_status 快照，而是 `set_state(GOT_IP→COMMITTING)`
   成功——它是在 `s_ops_mux` 临界区内的 CAS 式迁移。cleanup 若先完成
   finalize（CANCELLED/EXPIRED），迁移被迁移表拒绝，apply 在读
   parent_ctx 之前失败；cleanup 若在飞（`expiring=true` 未 finalize），
   set_state 会成功，但 cleanup 随后在 job 锁上等待，apply 全程独占。
3. *同步原语*：`s_ops_mux` 临界区（全部状态迁移）+ `s_net_job_lock`
   （job 生命周期）+ `expiring` 标志协议（阻止 cancel/expire 重复进入）。
4. *锁序*：`s_net_job_lock` → `s_ops_mux`；反向不存在（`ops_mux` 临界区
   不回调 cmd.c——cleanup/run 都在临界区外执行）。apply 持锁期间调用的
   wifi/config 锁均在更内层，无环。`app_cmd_net_on_ble_disconnect` 对
   `owner/op` 的读取不持锁（对齐枚举/指针读，最坏读到陈旧值 → cancel 被
   状态机拒绝），未纳入锁范围。

**降级但安全的行为**：BLE 在提交中断连时，apply 不再被撕碎——cleanup 等
锁至 apply 以 SUCCESS/FAILED 收场后清理，其 finalize 因状态已终态而成为
no-op；代价是断连方要等 NVS 提交完成（数十 ms 级）。

测试：host 85/85（不变）；固件零告警（app 0x117710）。板上冒烟仍待复审
通过后在实验构建执行。

### 9.4 R3：`expiring` 与 `COMMITTING` 的状态机互斥 + 断连快照（回应复审 T1/T2）

复审指出 R2 的 job 锁**没有**关闭全部窗口：cleanup 在 job 锁内清完、解锁
返回后，`operation.c` 还要回 `s_ops_mux` finalize；finalize 之前状态仍是
GOT_IP，dispatcher 的 `set_state(GOT_IP→COMMITTING)` 仍可成功,随后读到
已被清零的 job。单纯叠加 job 锁确实不充分——裁决必须上收到状态机。

**修复 1（`operation.c` `set_state`）**：临界区内新增 `expiring` 闸——
`expiring=true`（cleanup 在飞、op 注定终态）时拒绝一切状态迁移
（`ESP_ERR_INVALID_STATE`）。由此两个竞争方向由**同一把 `s_ops_mux`
原子裁决**：

- cleanup 先赢（`expiring=true` 先落锁）→ dispatcher 的 `COMMITTING`
  迁移被闸拒绝,在读 job 数据之前失败返回；
- dispatcher 先赢（`COMMITTING` 落锁）→ 后续取消/超时无法启动 cleanup：
  `cancel` 入口 `can_cancel(COMMITTING)=false`（`operation_state.c:31`），
  `expire_one` 选中条件要求 `deadline_ms!=0` 而 COMMITTING 已清 0
  （`operation.c:357-358`）。

两条都满足复审"同一同步机制、不可同时成立"的要求；job 锁此后只负责
内存访问串行,不再承担状态裁决。

**连带效应（如实记录）**：`run_fn` 的 `set_state(GOT_IP/FAILED/CANCELLED)`
若恰逢 cleanup 在飞（tryconnect 期间过期/取消）现在会失败,run 返回错误
后由 `run_pending` 的兜底直接写 FAILED（`s_ops_mux` 内,先检查
`is_active`）——终态结果等价（FAILED/EXPIRED 二选一,内存安全）,与
R2 之前"cleanup finalize 与 run 收尾直接互写"的混沌相比是收敛。

**修复 2（`cmd.c` `app_cmd_net_on_ble_disconnect`）**：`owner`/`op` 改为
job 锁内快照,锁外调用 `app_operation_cancel`（cancel→cleanup 会再取本锁,
持锁调用即自锁）。句柄有效性论证：operation 槽位是静态数组（`s_ops[4]`,
永不释放）,回收仅针对终态且需 600s 保留期（`RETENTION_MS`）,本函数解锁
到 cancel 的窗口远小于该值；cancel 内部 finalize 另有 generation 守卫,
陈旧 finalize 不会写坏更新的 operation。`s_net_trial` 的无锁访问维持
现状（既有边界,阶段 3 归属快照一并解决）。

**复审三项要求对账**：①cleanup 已启动 ⇒ `COMMITTING` 失败 ✓（闸）；
②dispatcher 先进 `COMMITTING` ⇒ 取消/超时无法启动 cleanup ✓（既有
can_cancel/deadline 双闸,已核实引用行）；③同一同步机制裁决 ✓
（`s_ops_mux`;job 锁降级为纯内存串行）。

测试：host 85/85（`operation.c` 为设备侧代码,host 单测不覆盖——板上
`device.info→login→tryconnect→apply` 全事务 + 取消/断连竞争验证待复审
通过后在实验固件执行）；固件零告警（app 0x117730）。

### 9.5 R4：generation 复用保护与 FAILED 兜底终态（复审两重点的代码证据）

**发现 1：generation 校验实际失效（已修）。** 原 `app_operation_start` 先
`memset(slot,0,…)` 再 `slot->generation++`，每次复用 generation 都回到 **1**。
槽满时 `app_operation_oldest_terminal_locked()` 会**立即**回收最老终态槽
（不受 600s 保留期约束），因此"静态数组不释放"不能推出"句柄长期有效"：
陈旧 cancel/expire 捕获的 generation（1）会与新 op 撞上,绕过 finalize 守卫。
修复：复用前取 `next_generation = slot->generation + 1`，memset 后写回；
类型升为 `uint32_t`（内部 `app_operation_t` 与公开 `app_operation_status_t`
同步）。**代码证据**：`operation.c:287-296`（保留 generation）、
`operation.c:391/413/421/437`（cancel 捕获与 finalize 守卫）、
`operation.c:533/558`（expire 同一协议）、`operation.c:289`（slot 复用路径）。

**发现 2：run_pending 兜底写 FAILED 与 cleanup 冲突（已修）。** `run_fn`
返回错误后，原兜底只检查 `is_active(op->state)`；cleanup 在飞时状态仍是
active（`expiring=true` 但未 finalize），兜底会写 FAILED，随后 cleanup 的
finalize 因状态非 active 落空——即"对外 FAILED、清理仍在执行/终态被覆盖"。
修复：兜底条件增加 `!op->expiring`；`expiring=true` 时终态**独占**交由
cleanup 裁决（CANCELLED / EXPIRED / FAILED(cleanup_failed)）。**清理恰好
一次**的论证：`expiring` 只在 `cancel`/`expire_one` 的 `s_ops_mux` 临界区内
置位，两者互斥；`run_pending` 不再写终态；`reclaim` 只回收终态；因此
`cleanup_fn` 对本 op 至多执行一次。**代码证据**：`operation.c:509-521`
（兜底 + expiring 条件）、`operation.c:370-440`（cancel 单次入口）、
`operation.c:528-568`（expire 单次入口）。

**终态一致性矩阵（expiring 置位后）**：

| 竞争方 | 结果 |
| --- | --- |
| cancel/expire 先置 expiring | set_state(任何) 拒绝；run_pending 不写；cleanup finalize CANCELLED/EXPIRED/FAILED(cleanup_failed) |
| run_pending 先写 FAILED（expiring 仍 false） | cancel/expire 因状态终态/非 active 不启动 cleanup；无重复清理 |
| run_fn 先成功置 GOT_IP | 之后 cancel/expire 按 can_cancel/deadline 正常进行 |

**仍未解决（按复审裁决登记）**：`s_net_trial` 的无锁访问（worker
store_trial vs dispatcher cleanup/status）。本轮不宣称已消除；板上竞争
测试会记录是否命中，若命中另立 R5 用同一 job 锁收口。

测试：host 85/85（operation.c 设备侧，host 不覆盖）；固件零告警
（app 0x117730）。板上验证见 §9.6。

### 9.6 R5：槽位 running 保护与 run_fn 代际校验（板上发现，已修）

**板上发现（R4 实验固件，2026-10-28，取消竞争用例）**：cancel 一个
CONNECTING 操作后，旧 `run_fn` 仍在 `app_wifi_try_connect_ip` 内阻塞；取消
finalize 为 CANCELLED 后槽位立即可复用，下一个 tryconnect 复用了同一槽位。
旧 run 返回时：
- 裸 `app_operation_state(op)` 读到的是**新 op 的 ACCEPTED/CONNECTING**，
  不是"已终态"；
- 旧 run 执行 `app_cmd_net_async_clear_job_sync()`，把**新 op 的 job 清零**；
- worker 随后启动新 op 时 `job->active` 已为 false → `run failed:
  ESP_ERR_INVALID_ARG` → 兜底写 `internal_error`。

串口证据（R4 实测）：`accepted op-EC59…` 后 `run op-EC59… -> connecting` →
`run op-EC59… failed: ESP_ERR_INVALID_ARG`，且此前 `op-45F20…` 的 tryconnect
在取消后仍完成（`tryconnect failed reason=15`）。这属于复审关注的
"清理/终态兜底引入新问题"，不是测试脚本问题。

**修复（operation.c + cmd.c）**：

1. `app_operation_t` 增加 `running`：`run_pending` 在置 CONNECTING 的同一
   临界区内置 true，`run_fn` 返回后（再次持锁）置 false。`reclaim_terminal_
   locked` 与 `oldest_terminal_locked` 跳过 running 槽——run_fn 在飞期间该槽
   不可回收/复用（槽满时新请求返回 no_slot/busy，而非踩旧指针）。
2. `app_cmd_net_async_run` 在阻塞调用前取 `app_operation_get_status` 的
   `generation`，返回后复核 `generation` 与 `state==CONNECTING`；不一致
   （陈旧/已终态）直接返回 ESP_OK，**不触碰 job**。这是第二道防线，防止
   未来框架改动再次引入复用窗口。
3. 终态仍由 §9.5 的规则裁决：cleanup 在飞时 run_pending 不写 FAILED。

**板上结果（R4 发现缺陷 → R5 复测，2026-10-28）**：

| 用例 | R4 | R5 复测 |
| --- | --- | --- |
| device.info / 默认管理员登录 / network.status | PASS（token 32、600s） | PASS（同） |
| 不存在 SSID tryconnect | failed/ap_not_found | failed/ap_not_found；apply（operation_id）→ operation_state |
| CONNECTING 时 cancel | cancelled；**下一 op internal_error（缺陷）** | cancelled；apply→operation_state；下一 op 被受理；重连后查询被中断 op=cancelled，新 absent op=ap_not_found；串口 `stale after wait (gen=1 state=7), no job touch`，无旧 worker 污染 |
| CONNECTING 时 BLE 断开 | cancelled；下一 op internal_error | cancelled；重连后 op=cancelled；下一 op 被受理（紧随取消的 absent op 得到 `wifi_scan_failed`——Wi-Fi 过渡期预扫描失败，P1-10 既有行为，非 stale job） |
| 连续 3 次 cancel | 3/3 cancelled，无死锁 | 未重跑（主机 BLE 约 40s 断链，见下）；R4 已证无死锁 |
| GOT_IP → apply 成功事务 | BLOCKED | **仍 BLOCKED**：`IOTHUB_WIFI_PASSWORD` 未提供 |
| 资源 | 基线正常 | 竞争期间 heap min 33,028 / free 44,132 / largest 31,744；结束后 free 61,388 / largest 45,056；无 panic/WDT/assert |

**主机 BLE 链路注记**：本次 R5 会话在 44.6s 出现 `central disconnected
(reason=0x222)`；连接初始参数 45ms/0/420ms 不满足设备策略（监督超时 <4s），
设备按 P1-1 请求更新、主机未应答——这是已登记的 P1-1 D 分支（主机侧），
不是 R5 回归。主机 BLE 单次会话因此限制在 ~40s；重连后设备和 op 状态正常。

**R6 附注**：R5 后另修一处 stale 日志路径的未初始化读取（纯日志，无行为
变化）；R5 板测证据对应 ELF SHA `b3caedeb8…`（`c1_r5_board_20261028.log`，
已脱敏），R6 未重烧、未重跑板上用例。

**仍未解决**：`s_net_trial` 无锁访问（复审裁决保留）；完整 GOT_IP/apply
事务与"cleanup 在飞 vs COMMITTING"的确定性窗口仍需有效 AP 口令与测试钩子。

### 9.7 R7：确定性测试钩子与板上闭环（回应复审"测试钩子"要求）

**钩子设计（仅实验构建）**：`APP_C1_TEST_HOOKS=ON`（独立 CMake option +
独立构建目录 `build-hooks/`，正式 `build/` 不含钩子符号）。`diag.testhook`
命令（BLE 来源、仍需 token、正式构建返回 unknown_command）：
`arm {hold_ms≤30000, probe_to}` / `clear` / `try {operation_id, to}`。
`operation.c` 在 cancel/expire 的 cleanup 返回与 finalize 之间插入暂停窗
（单次武装、自动到期），暂停入口对同一 op 直调一次 `set_state(probe_to)`
并把结果打到串口——"expiring=true 时一切迁移被拒"获得确定性证据，不依赖
随机调度。

**钩子首测即抓到真崩溃（R7 勘误，已修）**：`fallback skipped` 的
`ESP_LOGI` 原本写进了 `s_ops_mux` **临界区内部**（ESP_LOG 禁入 spinlock），
cleanup 在飞 + run 兜底的交错首次命中即 `abort()` 重启。该路径随机压测
不可达——确定性钩子的直接价值实证。修复：锁内只取判据，日志移到锁外。

**板上确定性结果（hooks 构建 ELF `d772b4d7…` 后续修正版，证据
`c1_r7_hooks_20261028.log`，已脱敏）**——cancel CONNECTING 操作、hold
25s，全序列：

```
(13678) C1 hook: pause cancel_post_cleanup enter (expiring=1 state=connecting gen=1)
(13688) C1 hook: probe set_state(got_ip) -> ESP_ERR_INVALID_STATE [rejected by expiring gate]
(26708) run op-… failed: ESP_ERR_INVALID_STATE          ← worker 窗口内返回,set_state(FAILED) 被闸拒
(26718) run op-… fallback skipped: cleanup owns terminal (expiring=1)
(37798) C1 hook: pause cancel_post_cleanup exit         ← 25s 后 finalize
final= cancelled（恰好一次;无第二次 cleanup;无 FAILED 覆盖;无崩溃）
```

| 复审要求 | 结果 |
| --- | --- |
| expiring=true ⇒ 一切 set_state 拒绝 | **确定性 PASS**（探针 + FAILED 写入两路均被拒） |
| cleanup 在飞 ⇒ 兜底不得写 FAILED | **确定性 PASS**（skip 日志 + 终态 cancelled 而非 failed） |
| 旧 worker 不读写新一代 job | PASS（R5 板测 `stale after wait, no job touch` + 本轮 generation 一致时行为正确） |
| 终态只裁决一次、cleanup 不重复 | PASS（单一 finalize;cancel 与 run 兜底互斥由闸保证） |
| COMMITTING 级完整 T1/T2（GOT_IP 态） | **BLOCKED**：需有效 AP 口令到达 GOT_IP。闸代码与状态无关（同一判据先于迁移表执行）,CONNECTING 级证据已闭环,COMMITTING 级待口令后补一发 |
| 完整 `tryconnect→GOT_IP→apply` 事务 | **BLOCKED**：同上 |

资源：竞争期间 heap min 34,524 / largest 32,768；`ble_cmd` 栈水位
1,312B（暂停在该任务执行,4KB 栈余量充足）。日志中唯一 "abort" 匹配为
MQTT errno 字符串（`Software caused connection abort`），非崩溃；测试后
设备自动回网,MQTT 在线。设备测试结束时代码在实验 hooks 构建（ELF 见
证据日志头部）,正式构建（钩子关,ELF `79b5de44…`）未烧录。

### 9.8 R8：`s_net_trial` 并发边界修复（复审"不要让文档代替修复"）

**访问点清点与旧风险确认**：`s_net_trial` 的读写分布在 4 个任务上下文——
worker（`store_trial`@run 成功）、BLE dispatcher（sync/async tryconnect、
apply、op.get、network.status）、NimBLE host（断连清理）、cancel/expire
cleanup。修复前确认存在多对**无同步并发读写**（C 数据竞争，非"未复现"
可接受）：worker 写 trial vs dispatcher op.get/status 读；NimBLE 断连
memset vs dispatcher strcmp/sync apply 读；dispatcher sync apply 读 vs
NimBLE 清理。

**修复**：job 锁升级为**递归互斥**（`app_mutex_ensure(…, true)`，wifi.c
restart/scan 同款模式），trial 四原语内建加锁（`store_trial` 整体、
`trial_clear`、`trial_ready` 含 TTL 过期清理、`app_cmd_net_operation_status`
整体），其余读点逐处收进锁 scope：断连 trial 段、tryconnect.async 忙检、
apply 预检、network.status trial 子对象、同步 tryconnect 忙检/清理/响应
字段、同步 v0 apply 的 patch/profile/trial_id 三个读簇、cancel 后 trial_id
拷贝。异步 apply（小程序主路径）的内部再入本就全程持锁。

**锁序**（不变）：`s_net_job_lock(递归)` → `s_ops_mux` → wifi/config 锁；
反向不存在。递归性用于 apply 再入（持锁）与内层 trial 原语的合法嵌套。

**R8 过程中的两个工程事故（如实登记）**：
1. **递归锁 API 误用死锁**：升级递归互斥后仍用普通 `xSemaphoreTake/Give`
   —— 递归互斥不识别普通 Take 的属主，同任务二次加锁自锁，正式构建首次
   回归在 `network.status` 上当场死锁（host 单测不覆盖 FreeRTOS 锁 API）。
   修复：`xSemaphoreTakeRecursive/GiveRecursive`。
2. **构建流程**：R8 中一次正式构建被管道 `head` 的 SIGPIPE 截断，`build/`
   残留旧二进制——之后一律全量构建落日志再校验 ELF SHA。

### 9.9 正式候选固件回归（复审 §2 步骤 1-4 执行记录）

- **构建身份**：`APP_C1_TEST_HOOKS=OFF` 经三重验证——CMakeCache
  `OFF`、`strings build/iothub.bin` 无 `diag.testhook`/`C1 hook`（0 命中；
  hooks 构建 6 命中）、启动日志 `ELF file SHA256: 8c7abd8cc…`（与 hooks
  构建 `0883a72e…` 不同）。烧录后经 UART0 启动日志 + BLE 登录确认身份。
- **最小回归（全部 PASS）**：

| 用例 | 结果 |
| --- | --- |
| BLE 登录（token 32/600s） | PASS |
| `device.info` / `network.status` | PASS（无死锁——R8-1 修复后验证点） |
| 不存在 SSID tryconnect | failed/ap_not_found；apply→operation_state |
| CONNECTING 时 cancel | cancelled；apply→operation_state；**下一 op 立即受理**（op-CE99…） |
| CONNECTING 时 BLE 断开 | `cancelled on BLE disconnect`；重连查询=cancelled；下一 op=ap_not_found |
| 旧 worker 污染 | **无**：`stale after wait (gen=1 state=7), no job touch` 三次正确触发 |
| 死锁/panic/internal_error | 无；heap min 33,296 / 结束 free 56,448 |

- 证据：`tools/ble_p0/results/c1_r8_formal_20261028.log`（已脱敏）。
- 主机 BLE 0x222（41.9s）再次出现（P1-1 D 分支，主机侧），不影响设备侧
  判定；设备侧全部断开原因均为正常路径。
- hooks 构建同步重建（ELF `0883a72e…`），R7 的钩子证据边界不变。

**仍未实测**：COMMITTING 级 T1/T2 与完整 `tryconnect→GOT_IP→apply` 事务
（`IOTHUB_WIFI_PASSWORD` 未提供）；正式构建上的事务测试在口令到位后于
同一 ELF 上补做。
