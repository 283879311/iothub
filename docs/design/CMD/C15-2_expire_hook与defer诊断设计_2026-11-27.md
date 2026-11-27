# C15-2 test-only expire hook 目标过滤、并发同步与 MQTT defer 诊断设计（2026-11-27）

> 文档状态：PENDING（设计待审）。设计成熟度：DESIGN_CANDIDATE。
> 范围：仅 test-only 诊断与 hook 增强设计；不修改生产行为、不构建设备镜像、不操作设备。
> 关联：[C15-2 剩余覆盖设计](C15-2_BUSY剩余覆盖设计_2026-11-26.md)。

## 1. 源码事实（main@5aaabea）

| 事实 | 位置/结论 |
|---|---|
| hook 全局单次 | `s_c1_hook_armed`、`s_c1_hook_until_ms`、`s_c1_hook_probe`、`s_c1_hook_probe_to` 为普通静态变量 |
| 无同步 | arm/clear 与 pause 无锁读写；pause 可在 op_worker、expire 任务或 BLE 命令任务执行 |
| 无目标过滤 | `app_operation_test_hook_pause()` 只检查 armed，随即清除并消费 |
| 两条路径共用 | `cancel_post_cleanup`（cancel 路径）与 `expire_post_cleanup`（request_finish，target=EXPIRED） |
| expire 有两个调用者 | `app_operation_expire_task` 每秒调用；`app_operation_worker` 也每秒调用。两者都不是 BLE 命令任务 |
| expire 选择 | 在 `s_ops_mux` 内按槽位顺序扫描，条件含 active、`!expiring`、`!finish_registered`、`deadline_ms!=0`、已到期；非 deadline 排序 |
| finish 生命周期 | `request_finish` 在 O 内置 `finish_registered=true`、`expiring=true`、`finish_target_state`；调用 finish_fn 后、finalize 前调用 hook |
| hook 调用条件 | 仅 `outcome != PENDING/STALE` 且 `finish_target_state == APP_OP_EXPIRED` 时调用 expire hook |
| MQTT defer | `app_operation_start_with_finish()` 先 `app_mqtt_defer_restart_begin()`，再取 `s_ops_mux`、检查 `app_operation_has_active_locked()`；两处都可返回 `ESP_ERR_INVALID_STATE` |
| job 映射 | `app_cmd_net_job_begin_locked()` 把 `ESP_ERR_INVALID_STATE` 统一映射为 `BEGIN_BUSY` |

因此，当前 hook 无法证明目标关联，也无法证明跨任务发布一致性；不能靠实验步骤缩短时间解决。

## 2. 目标 hook 设计

### 2.1 状态与接口（test-only）

统一 hook 状态，保留旧 C1 全局 hook 行为：

```c
typedef struct {
    bool armed;
    bool targeted;                  /* false=旧全局 hook；true=按 operation 目标 */
    char op_id[APP_OPERATION_ID_LEN];
    uint32_t generation;
    int64_t until_ms;
    bool probe;
    app_op_state_t probe_to;
} app_operation_hook_state_t;
```

```c
/* 仅 APP_C1_TEST_HOOKS && APP_C15_HOOK_TARGET 构建。
 * 在 O 内按 op_id 捕获 generation；只接受 GOT_IP 且未进入 finish 的目标。
 * 返回 ESP_OK 表示已武装；NOT_FOUND/INVALID_STATE/INVALID_ARG 表示拒绝。
 * out_generation/out_deadline_ms 可为 NULL，成功时返回实际绑定值。 */
esp_err_t app_operation_test_hook_arm_target(const char *op_id,
                                             uint32_t hold_ms,
                                             int probe_to_state,
                                             uint32_t *out_generation,
                                             int64_t *out_deadline_ms);
void app_operation_test_hook_clear(void);   /* 旧/新 hook 共用 */
```

`app_operation_test_hook_arm()` 保留，语义为全局非目标 hook；它与目标 hook 共用同一 `armed` 状态，互斥。目标 hook 只能单次武装。

### 2.2 arm 条件（在 `s_ops_mux` 内）

- `op_id` 非空且长度小于 `APP_OPERATION_ID_LEN`；按 `op_id` 找到 `used` 槽位；
- 目标 `state == APP_OP_GOT_IP`，且 `deadline_ms != 0`；
- `!expiring` 且 `!finish_registered`（目标尚未进入 finish）；
- 当前无任何 armed hook（旧全局或目标）；
- `hold_ms` 在 `[100, 360000]`；`probe_to_state` 为 `APP_OP_FREE` 或合法状态；
- 检查通过后在 O 内用有界 `memcpy` 发布 `targeted/op_id/generation/until/probe`（不用 `snprintf`）；`out_generation/out_deadline_ms` 先在 O 内复制到局部变量，释放 O 后再写出；
- 日志在 O 外打印，包含 `op_id`、`generation`、`deadline_ms`、`hold_ms`。

只接受 `GOT_IP` 的理由：GOT_IP 的 deadline 为 `now + 300000 ms`，在目标 operation 上稳定；若在 `ACCEPTED/CONNECTING` 武装，目标进入 GOT_IP 时 deadline 会被重写为 300 s，早先的 `until_ms` 可能提前超时。

### 2.3 pause 匹配（在 `s_ops_mux` 内）

- 若 `!armed`，直接返回；
- 若 `targeted == false`，保持旧语义：任何 cancel/expire 路径都可消费并暂停；
- 若 `targeted == true` 且 `now_ms > until_ms`：清除 armed，释放 O 后记录 `timeout`，不暂停、不再进入匹配；
- 若 `targeted == true` 且未超时，仅当以下全部满足才消费：
  - `op->used && op->id == target.op_id && op->generation == target.generation`；
  - `op->state == APP_OP_GOT_IP`；
  - `op->finish_registered`；
  - `op->finish_target_state == APP_OP_EXPIRED`；
  - `op->expiring`；
  - `now_ms <= until_ms`。
- 匹配：清除 armed、复制 probe 参数，释放 O 后记录 `hit` 日志，再执行 probe/延时；
- 不匹配（未超时）：释放 O 后记录 `skip` 原因（id/gen/state/kind），**不消费**。

cancel 路径的 `cancel_post_cleanup` 没有 `finish_registered=true`，因此不会消耗目标 expire hook；它只会被旧全局 hook 消费。

### 2.4 锁序与执行上下文

- 旧/新 arm、clear、pause 的 hook 状态统一由 `s_ops_mux`（O）保护；O 是 spinlock；
- O 内只做字段读取/复制/状态位修改，不 `ESP_LOG`、不 `vTaskDelay`、不 `snprintf`、不调用 job/mqtt/wifi；
- pause 在 O 外执行 probe `set_state()` 与延时；probe 的 `set_state` 重新取 O，此时已释放，无嵌套；
- arm/clear/pause 均不调用 job 层，不形成 O→J；也不取 MQTT/Wi-Fi 锁；
- 调用方约定：任何调用方不得在持有 O 时调用 arm/clear/pause；
- pause 可发生在 op_worker 或 expire 任务；两者都通过 `expiring`/`finish_registered` 避免重复 finish。

**锁序证明（新增路径）**

| 路径 | 获取的锁 | 结论 |
|---|---|---|
| arm/clear（BLE 命令任务） | O → 释放；无 J/M/W | O 是叶子锁 |
| pause（expire，经 `finish_fn` 返回后） | O → 释放 → probe set_state 再取 O → 延时 | 无 J/M/W；O 内无阻塞 |
| pause（cancel，经 cleanup 返回后） | O → 释放；若调用方持有 J，则仅 J→O；hook 不取 J | 与既有 J→O 顺序不冲突 |
| MQTT defer diag | 沿用既有 J（调用方持有）→ M → 释放 → O | defer 的 M 在 O 之前且已释放，无 M→O 嵌套 |

新增代码不形成 O→J、O→M、O→W；不把 O 带入 `ESP_LOG`/`vTaskDelay`；`app_operation_set_state` 只在 O 释放后调用，其内部重新取 O，不与外层 O 嵌套。

### 2.5 生命周期/竞争场景

| 场景 | 期望 |
|---|---|
| 目标 GOT_IP、未 finish，到期 | pause 命中并消费 |
| 目标未到 GOT_IP | arm 返回 `INVALID_STATE` |
| 错误 ID | arm 返回 `NOT_FOUND` |
| 目标已 `finish_registered`/`expiring` | arm 返回 `INVALID_STATE` |
| 已存在旧全局或目标 hook | arm 返回 `INVALID_STATE`，不覆盖 |
| cancel 路径 pause | 目标 hook 因缺少 `finish_registered` 而 skip，不消费 |
| 其他 operation expire | id/gen 不匹配，不消费 |
| 目标被其他 BLE 命令 cancel | cancel 不消费目标 hook；目标终态后 hook 只能超时清除 |
| arm/clear 并发 | O 串行化，最后一次操作生效，无撕裂 |
| pause 与 clear 并发 | O 内串行；命中前 clear 则 skip |
| 超时未命中 | 自动清除并记录 timeout，不污染后续 |
| op_worker 与 expire 任务同时到期 | 两者都在 O 内选目标；`expiring` 使第二个跳过；hook 只命中一次 |
| arm 后目标 GOT_IP deadline 被重写 | 设计禁止在非 GOT_IP 武装，避免该窗口 |

### 2.6 BLE 诊断入口

复用 `diag.testhook`（仅 `APP_C1_TEST_HOOKS`，仅 BLE 来源，非 v1 Contract）：

- `action=arm` 且带 `operation_id`：调用目标 arm；响应返回 `armed`、`generation`、`deadline_ms`、`remaining_ms`、`hold_ms`；
- `action=arm` 不带 `operation_id`：保持旧全局 hook 的现有行为（`hold_ms` 取值 100–30000，非法值回落 8000）；
- 目标 arm 的 `hold_ms` 允许 100–360000，覆盖 300 s GOT_IP TTL；
- 目标不存在时返回 `not_found`；非 GOT_IP、已 finish 或已有 hook 时返回现有 test-only 错误名 `not_active`；参数非法返回 `invalid_param`；均不改变 hook；
- `action=clear` 清除任一 hook。

客户端流程：`network.tryconnect` → 轮询 `operation.get` 到 `got_ip` → `diag.testhook action=arm operation_id=... hold_ms=310000` → 等待 `expired` → 在暂停窗口内发送第二次 `network.tryconnect`（必须使用不同 `idempotency_key`）。arm 响应中的 `generation/deadline_ms` 必须写入证据；暂停窗口内不得发送 `network.cancel`/`network.apply`，否则目标可能在第二次请求前终态化。

前置条件：第一次 operation 必须真实到达 `GOT_IP`（候选 AP 可关联且凭据有效）；凭据只由获授权操作者本地输入，不进入对话、串口日志或归档文件。

## 3. MQTT defer per-call 诊断

### 3.1 结构

```c
typedef struct {
    bool defer_attempted;      /* owner != INTERNAL 时为 true */
    bool defer_started;        /* begin 返回 OK 后置 true */
    int defer_result;          /* esp_err_t，attempted 时有效 */
    bool active_check_done;    /* 已进入 O 并完成 active 检查 */
    bool active_check_result;  /* true=存在 active operation */
    int start_result;          /* 本次调用的最终返回值 */
} app_operation_start_diag_t;
```

### 3.2 调用方式

- 生产入口 `app_operation_start_with_finish()` 签名不变，内部调用公共实现并传 `NULL` diag；
- test-only 入口 `app_operation_start_with_finish_diag(..., app_operation_start_diag_t *diag)` 传本次调用结构；
- 公共实现入口先校验 diag 并清零；所有返回路径写 `start_result`；
- `owner != INTERNAL` 时置 `defer_attempted=true`，在 `app_mqtt_defer_restart_begin()` 返回后立即写 `defer_result`；失败时 `start_result=defer_result` 并返回；成功时 `defer_started=true`；
- 进入 O 后置 `active_check_done=true`、写 `active_check_result`；存在 active operation 时 `start_result=ESP_ERR_INVALID_STATE`；
- no slot/worker 创建失败写 `ESP_ERR_NO_MEM`；成功写 `ESP_OK`；
- diag 结构由调用方栈上持有，**不跨调用共享**；`cmd_net_job` 在 begin 调用返回后再把本次结果复制到既有 `s_begin_diag` 快照，禁止把全局 `s_begin_diag` 的地址传给 operation 层；
- INTERNAL owner 不记录 defer，`defer_attempted=false`。

### 3.3 诊断映射

`cmd_net_job` 在 begin 诊断中记录：

| 条件 | 结论 |
|---|---|
| `defer_attempted && defer_result==ESP_ERR_INVALID_STATE && !defer_started` | BUSY 来自 MQTT restart |
| `defer_started && active_check_done && active_check_result` | BUSY 来自 active operation |
| `defer_result` 为其他错误 | 按该错误记录，不映射为 busy |
| `start_result==ESP_ERR_NO_MEM` | no_slot/worker 资源失败 |
| 其他 | 按 `start_result`/begin 枚举记录 |

生产路径、defer 顺序与错误处理不变；仅在 `APP_C15_BUSY_DIAG` 构建启用 diag 入口。

## 4. Host 测试矩阵

| 编号 | 场景 | 期望 |
|---|---|---|
| H1 | GOT_IP 目标正确 id，expire 到期 | hit 一次，generation/deadline 与 arm 响应一致 |
| H2 | 错误 id | arm `NOT_FOUND` |
| H3 | 目标非 GOT_IP | arm `INVALID_STATE` |
| H4 | 目标已 finish/expiring | arm `INVALID_STATE` |
| H5 | 已有全局 hook 时目标 arm | `INVALID_STATE`，旧 hook 保留 |
| H6 | cancel 路径 pause | skip，不消费目标 hook |
| H7 | 其他 operation expire | skip，不消费 |
| H8 | arm 后 clear | 不 hit |
| H9 | 超时未命中 | timeout 清除 |
| H10 | arm/clear/pause 并发压力 | 无撕裂、无重复消费、无锁内日志 |
| H11 | op_worker 与 expire 任务竞争 | 只 finish 一次、只 hit 一次 |
| H12 | defer 返回 `ESP_ERR_INVALID_STATE` | diag 记录 MQTT 来源 |
| H13 | defer OK、active operation | diag 记录 operation 来源 |
| H14 | defer 返回 `ESP_ERR_NO_MEM` | 原错误返回，不映射 busy |
| H15 | INTERNAL owner | `defer_attempted=false` |

Host 测试需覆盖 `APP_C1_TEST_HOOKS`、`APP_C15_HOOK_TARGET`、`APP_C15_BUSY_DIAG` 的组合编译；测试构建单独定义，生产构建不定义。

## 5. 编译隔离

- 新增 `APP_C15_HOOK_TARGET`，默认 OFF；CMake 要求同时 `APP_C1_TEST_HOOKS=ON`，否则 FATAL_ERROR；
- `APP_C15_HOOK_TARGET` 控制目标 arm 实现与 `diag.testhook` 的 `operation_id` 分支；
- `APP_C15_BUSY_DIAG` 控制 start diag 结构与 test-only 入口；
- 生产构建两者均 OFF；生产 ELF 用 `nm` 检查 `app_operation_test_hook_arm_target`、`app_operation_start_with_finish_diag` 符号为 0；
- `tests/operation_host/Makefile` 的 `DEFS` 增加 `-DAPP_C15_HOOK_TARGET=1`（仅测试构建）；`tests/cmd_net_job` 保持 `APP_C15_BUSY_DIAG=1`；
- 不新增 BLE 命令名，不改变 v1 Contract 或生产状态机；`diag.testhook` 属实验构建专用入口。

## 6. 未决问题

1. 目标 hook 的 `hold_ms` 上限 360000 是否足够覆盖 BLE 连接保持与调度抖动；
2. 目标在 arm 后、到期前被 cancel 时，是否需要在 cancel 路径主动清除 hook，避免阻塞后续实验；
3. `diag.testhook` 目标 arm 的响应是否再增加 `state` 字段（当前草案已含 `deadline_ms`/`remaining_ms`）；
4. Host 并发压力测试的线程模型与可重复性；
5. `APP_C15_HOOK_TARGET` 是否与 `APP_C15_BUSY_DIAG` 合并为单一实验宏，或继续保持独立以缩小组合面。

## 7. 边界

- 本文只描述设计，不修改源码、不构建、不运行 Host/设备测试；
- 当前 hook 实现尚未满足目标关联与同步要求；
- 设计通过并实现、Host 验证后，才重新评审设备实验；
- 不启动 P2；
- C15-2 保持 `BUSY_REJECTED / NOT_COVERED`，CMD-15 保持 `TEST_PENDING`。
