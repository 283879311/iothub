# C15-2 test-only expire hook 目标过滤、并发同步与 MQTT defer 诊断设计（2026-11-27）

> 文档状态：PENDING（设计待审）。设计成熟度：DESIGN_CANDIDATE（rev3，处理 R1 生命周期竞态与终态覆盖）。
> 范围：仅 test-only 诊断与 hook 增强设计；不修改生产行为、不构建设备镜像、不操作设备。
> 关联：[C15-2 剩余覆盖设计](C15-2_BUSY剩余覆盖设计_2026-11-26.md)。
>
> rev3 处理：notify 改为捕获 ID/generation 后按值调用；补终态路径覆盖表；明确 PENDING/STALE 清理责任与最长占用；收敛 hold 默认值和 deadline 余量。
> rev2 处理：R1 目标 cancel/失效生命周期；R2 解锁后快照约束；R3 `s_begin_diag` 同步；R4 MQTT defer 锁序逐路径证明。

## 1. 源码事实（main@5aaabea）

| 事实 | 位置/结论 |
|---|---|
| hook 全局单次 | `s_c1_hook_armed`、`s_c1_hook_until_ms`、`s_c1_hook_probe`、`s_c1_hook_probe_to` 为普通静态变量 |
| 无同步 | arm/clear 与 pause 无锁读写；pause 可在 op_worker、expire 任务或 BLE 命令任务执行 |
| 无目标过滤 | `app_operation_test_hook_pause()` 只检查 armed，随即清除并消费 |
| 两条 pause 路径 | `cancel_post_cleanup`（仅 `app_operation_cancel` 路径）与 `expire_post_cleanup`（`request_finish`，target=EXPIRED） |
| 生产 cancel 路径 | `network.cancel` 调用 `app_operation_request_finish(..., APP_OP_CANCELLED, ...)`，**不经过** `cancel_post_cleanup`；只在 expire 分支调用 hook |
| `app_operation_cancel` 使用面 | 主要见 operation selftest 路径，不是当前 BLE `network.cancel` 的生产实现 |
| expire 有两个调用者 | `app_operation_expire_task` 每秒调用；`app_operation_worker` 也每秒调用。两者都不是 BLE 命令任务 |
| expire 选择 | 在 `s_ops_mux` 内按槽位顺序扫描，条件含 active、`!expiring`、`!finish_registered`、`deadline_ms!=0`、已到期；非 deadline 排序 |
| finish 生命周期 | `request_finish` 在 O 内置 `finish_registered=true`、`expiring=true`、`finish_target_state`；调用 finish_fn 后、finalize 前调用 hook |
| hook 调用条件 | 仅 `outcome != PENDING/STALE` 且 `finish_target_state == APP_OP_EXPIRED` 时调用 expire hook |
| MQTT defer | `app_operation_start_with_finish()` 先 `app_mqtt_defer_restart_begin()`，再取 `s_ops_mux`、检查 `app_operation_has_active_locked()`；两处都可返回 `ESP_ERR_INVALID_STATE` |
| job 映射 | `app_cmd_net_job_begin_locked()` 把 `ESP_ERR_INVALID_STATE` 统一映射为 `BEGIN_BUSY` |

关键修正：R1 不能只靠 `cancel_post_cleanup`。生产 `network.cancel` 走 `request_finish(CANCELLED)`，而该路径当前不会调用任何 hook。目标 cancel 清理必须由 finish/terminal 观察点或 arm 时失效检查完成。

因此，当前 hook 无法证明目标关联，也无法证明跨任务发布一致性；不能靠实验步骤缩短时间解决。

## 2. 目标 hook 设计

### 2.1 状态、触发类型与接口（test-only）

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

typedef enum {
    APP_OP_HOOK_TRIGGER_CANCEL_POST_CLEANUP,
    APP_OP_HOOK_TRIGGER_EXPIRE_POST_CLEANUP,
} app_operation_hook_trigger_t;
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

/* 旧入口保留；若已有任一 hook，忽略新 arm 并记录，不覆盖目标 hook。 */
void app_operation_test_hook_arm(uint32_t hold_ms, int probe_to_state);
void app_operation_test_hook_clear(void);
```

内部新增两个 test-only 观察点，均由 `APP_C1_TEST_HOOKS` 编译：

```c
static void app_operation_test_hook_pause(app_operation_hook_trigger_t trigger,
                                          app_operation_t *op);
/* 只按值接收身份；op_id/generation 必须在发布终态或登记 finish 的同一 O
 * 临界区内捕获。禁止在解锁后把 op 指针传进来重新取身份。 */
static void app_operation_test_hook_notify_target_inactive(const char *op_id,
                                                           uint32_t generation);
```

目标 hook 只能单次武装；旧全局 hook 与目标 hook 共用同一 `armed` 状态，互斥。

### 2.2 arm 条件与 stale 兜底（在 `s_ops_mux` 内）

- `op_id` 非空且长度小于 `APP_OPERATION_ID_LEN`；按 `op_id` 找到 `used` 槽位；
- 目标 `state == APP_OP_GOT_IP`，且 `now_ms < deadline_ms`；
- `!expiring` 且 `!finish_registered`（目标尚未进入 finish）；
- 若已有 hook：
  - 若 `now_ms > until_ms`：清除旧 hook 并记录 `timeout`，然后继续本次 arm；
  - 旧全局 hook（未超时）：返回 `INVALID_STATE`；
  - 目标 hook 且目标仍有效：返回 `INVALID_STATE`；若目标正在 pending expire（`finish_registered`、`finish_target_state==EXPIRED`、`expiring`、state 仍为 GOT_IP），也视为仍有效，不清除；
  - 目标 hook 但目标已失效（ID/generation 找不到、state 非 GOT_IP、terminal、非 pending-expire 的 `finish_registered`、无 finish 的 `expiring`、deadline 为 0）：清除旧目标并记录 `target_stale`，然后继续本次 arm；
- `hold_ms` 在 `[100, 360000]`；`probe_to_state` 为 `APP_OP_FREE` 或合法状态；
- 计算 `remaining_ms = deadline_ms - now_ms`，要求 `hold_ms >= remaining_ms + APP_OPERATION_TEST_HOOK_MARGIN_MS`（草案取 10000 ms）；不满足时返回 `INVALID_ARG`，不武装；
- 检查通过后在 O 内用有界 `memcpy` 发布 `targeted/op_id/generation/until/probe`（不用 `snprintf`）；`out_generation/out_deadline_ms` 先在 O 内复制到局部变量，释放 O 后再写出；
- 日志在 O 外打印，包含 `op_id`、`generation`、`deadline_ms`、`remaining_ms`、`hold_ms`。

只接受 `GOT_IP` 的理由：GOT_IP 的 deadline 为 `now + 300000 ms`，在目标 operation 上稳定；若在 `ACCEPTED/CONNECTING` 武装，目标进入 GOT_IP 时 deadline 会被重写为 300 s，早先的 `until_ms` 可能提前超时。

arm 时的 stale 兜底保证：即使目标通过未覆盖的 terminal 路径结束，下一次 arm 也不会被旧 hook 阻塞 360 s；`diag.testhook clear` 仍是显式兜底。

### 2.3 pause 匹配与目标取消（在 `s_ops_mux` 内）

先做超时检查，再做目标匹配：

- 若 `!armed`，直接返回；
- 若 `targeted == false`，保持旧语义：任何 cancel/expire 路径都可消费并暂停；
- 若 `targeted == true` 且 `now_ms > until_ms`：清除 armed，释放 O 后记录 `timeout`，不暂停、不再进入匹配；
- 若 `targeted == true` 且未超时：
  - `trigger == EXPIRE`、目标 ID/generation 匹配，且 `op->state == APP_OP_GOT_IP`、`finish_registered`、`finish_target_state == APP_OP_EXPIRED`、`expiring`：命中，消费 hook；
  - `trigger == EXPIRE`、目标 ID/generation 匹配但上述条件不满足：清除并记录 `target_stale`；若 `finish_target_state == CANCELLED` 则记录 `target_cancelled`；不暂停；
  - `trigger == CANCEL`、目标 ID/generation 匹配：清除 hook，记录 `target_cancelled`，**不暂停**；
  - 目标 ID/generation 不匹配：扫描表中目标是否仍存在且有效；若目标已不存在、已 terminal、已非 GOT_IP，或 `finish_registered` 且 `finish_target_state != APP_OP_EXPIRED`，清除并记录 `target_stale`；若目标 `finish_registered` 且 `finish_target_state == APP_OP_EXPIRED`、`expiring`、state 仍为 GOT_IP，则视为 pending expire，不清理，记录 `skip`；否则记录 `skip`，**不消费**；
  - 其他不匹配：记录 `skip` 原因（id/gen/state/trigger），**不消费**。

匹配时的字段在 O 内复制到局部快照；释放 O 后只使用快照。cancel 路径不会消费 expire pause。

### 2.4 目标失效通知（R1 核心：按值通知）

仅靠 pause 不能覆盖生产 cancel。新增 test-only `app_operation_test_hook_notify_target_inactive(op_id, generation)`，在 operation 发布终态或完成 finish 后调用；它只做目标失效清除，不做暂停。

**身份捕获规则（消除指针复用竞态）**

- 必须在发布终态的同一个 O 临界区内，把 `operation_id + generation` 复制到局部变量；
- 或在 `request_finish` 登记 finish 的同一个 O 临界区内捕获（现有 `op_id`/`log_gen` 已在回调前捕获，可直接复用）；
- 解锁后只把 `op_id`/generation 的**值**传给 notify，禁止传 `app_operation_t *`；
- notify 取得 O 后按 ID/generation 查找，不依赖原 operation 指针；
- 新增捕获使用有界 `memcpy`，不在 O 内引入 `snprintf`；`request_finish` 既有的 `snprintf` 保持原样；
- 没有发布终态、也没有登记 finish 的路径不调用 notify。

**终态发布路径覆盖表**

| 路径 | 身份捕获点 | 通知安排 | 说明 |
|---|---|---|---|
| `request_finish` 有回调，DONE_OK | 登记 finish 的 O 内：`op_id`、`log_gen` | cleanup 返回后先 expire pause（仅 EXPIRED）；apply 成功后 O 外 notify 按值 | cancel/SUCCESS/FAILED 目标均覆盖；apply 失败不 notify |
| `request_finish` 有回调，DONE_FAIL | 同上 | 同上 | DONE_FAIL 也通知；最终态由 apply 决定 |
| `request_finish` 无回调 | 登记 finish 的 O 内：`op_id`、`log_gen` | apply 成功后 O 外 notify 按值 | 此路径 outcome 固定 DONE_OK；apply 失败返回 STALE，不 notify |
| `request_finish` PENDING | 同上 | 不 notify | 目标仍在 finish/expiring；当前源码未找到后续 resume 终态发布点；hook 占用到 timeout |
| `request_finish` STALE | 同上 | 不 notify | 未发布终态；目标身份可能已变，由 arm stale/pause/timeout 兜底 |
| `app_operation_cancel` 成功 | 入口 O 内捕获 generation；终态 O 内捕获 `op_id` | cancel pause 按 ID/gen 匹配清除；终态 O 外再 notify 按值 | pause 已清除时 notify 为 no-op |
| `app_operation_cancel` cleanup 失败 | 入口 O 内捕获 generation；失败终态 O 内捕获 `op_id` | 失败终态 O 外 notify 按值 | 最终态 FAILED/CLEANUP_FAILED；generation 不匹配时未发布终态，不 notify |
| `app_operation_set_state` terminal | 同一次 O 内设置 terminal 并捕获 `op_id`/generation | O 外 notify 按值 | 消除解锁到通知之间的槽位复用窗口 |
| worker run 失败直接置 FAILED | 同一次 O 内设置 FAILED 并捕获 `op_id`/generation | O 外 notify 按值 | 目标异常失败清除 |
| 槽位回收/复用 | 无终态发布 | 不 notify | 下一次 arm 找不到旧 ID/generation，清除 `target_stale`；否则 timeout |
| `app_operation_cleanup_complete` | 不发布终态 | 不 notify | 只更新 cleanup claim，不能当作终态通知点 |

`app_operation_finish_apply_locked()` 是 `request_finish` 内部 helper，不单独设置通知点；其终态发布由调用方按上表处理。

`request_finish` 的 notify 只在 apply 成功后发生；PENDING、STALE、apply 失败均不 notify，目标 hook 由后续 arm stale/pause/timeout 兜底。若 EXPIRED pause 已命中但 apply 随后失败，hook 已被消费且目标可能仍未终态；实验必须判失败，不能记为已清理。

**notify 行为**

- 若 hook 未 armed 或为旧全局 hook：不动作；
- 取得 O 后先比较传入 `op_id + generation` 是否等于目标 hook 的身份：
  - 不相等：不动作（其他 operation 的终态）；
  - 相等：在表中查找该 ID/generation：
    - 找到：清除目标 hook；若状态/`finish_target_state` 为 CANCELLED，记录 `target_cancelled`；否则记录 `target_terminal`；
    - 找不到：清除目标 hook，记录 `target_stale`（槽位已回收/复用）；
- 只使用 O 内快照记日志；解锁后不引用 operation 指针。
- cancel 请求伴随 `DONE_FAIL` 时，notify 仍按 `finish_target_state==CANCELLED` 记 `target_cancelled`；最终状态由 apply 决定，可能是 `FAILED/CLEANUP_FAILED`。

**PENDING/STALE 的清理责任**

- `request_finish` 返回 PENDING 时不发布终态，也不通知；当前 `main` 中未找到后续 resume 终态发布点（`app_operation_cleanup_complete()` 只更新 claim）；
- 因此目标 hook 会保留到 `until_ms` 超时，最长占用为 `hold_ms`（默认 310000 ms，上限 360000 ms）；这属于有界资源占用，不得记录为“目标已清除”；
- 实验遇到 PENDING/STALE 必须判失败并显式 `diag.testhook clear`，不得继续第二次请求；
- STALE 不保证目标已终态，只保证本次 finish 没有发布终态；由 arm stale/pause/timeout 兜底。

这样 cancel 不消费 expire hook，但会立即清除目标 hook，新目标到达 GOT_IP 后可马上重新 arm。

### 2.5 锁序与执行上下文

- 旧/新 arm、clear、pause、notify 的 hook 状态统一由 `s_ops_mux`（O）保护；O 是 spinlock；
- O 内只做字段读取/复制/状态位修改，不 `ESP_LOG`、不 `vTaskDelay`、不 `snprintf`、不调用 job/mqtt/wifi；
- pause/notify 在 O 外执行日志；pause 的 probe `set_state()` 在 O 外调用，`set_state` 内部重新取 O，此时已释放，无嵌套；
- arm/clear/pause/notify 均不调用 job 层，不形成 O→J；也不取 MQTT/Wi-Fi 锁；
- 调用方约定：任何调用方不得在持有 O 时调用 arm/clear/pause/notify；
- pause 可发生在 op_worker 或 expire 任务；两者都通过 `expiring`/`finish_registered` 避免重复 finish。

**锁序证明（新增路径）**

| 路径 | 获取的锁 | 结论 |
|---|---|---|
| arm/clear（BLE 命令任务） | O → 释放；无 J/M/W | O 是叶子锁 |
| pause（expire，经 `finish_fn` 返回后） | O → 释放 → probe `set_state` 再取 O → 延时 | 无 J/M/W；O 内无阻塞 |
| pause（cancel，经 cleanup 返回后） | O → 释放；若调用方持有 J，则仅 J→O；hook 不取 J | 与既有 J→O 顺序不冲突 |
| notify（finish/cancel/set_state/worker 返回后） | O → 释放；只传 ID/gen 值；若调用方持有 J，则仅 J→O；hook 不取 J/M/W | 与既有 J→O 顺序不冲突 |
| MQTT defer diag（cmd_net_job begin 路径） | 调用方持 J → M → 释放 M → O | defer 的 M 在 O 之前且已释放，无 M→O 嵌套 |

新增代码不形成 O→J、O→M、O→W；不把 O 带入 `ESP_LOG`/`vTaskDelay`；`app_operation_set_state` 只在 O 释放后调用，其内部重新取 O，不与外层 O 嵌套。

### 2.6 解锁后快照约束（R2）

pause/notify 的 O 内匹配必须把诊断所需字段复制到局部快照，释放 O 后只使用快照：

```c
typedef struct {
    bool matched;
    char op_id[APP_OPERATION_ID_LEN];
    uint32_t generation;
    app_op_state_t state;
    bool finish_registered;
    app_op_state_t finish_target_state;
    bool expiring;
    bool timed_out;
    const char *reason;
} app_operation_hook_snapshot_t;
```

强制约束：

- O 内复制 `op_id`、generation、state、finish target、trigger、是否超时、匹配结果；
- 释放 O 后，日志只能使用局部快照；
- 不得在解锁后再次读取 `op->id`、`op->generation`、`op->state`、`op->finish_*`；
- 不得通过解锁后的 `app_operation_valid(op)` 补做匹配判断；
- notify 不接收 `app_operation_t *`；终态路径在发布终态的同一 O 内捕获 ID/generation，解锁后按值调用；
- probe 只允许把 `op` 作为不透明句柄传给 `app_operation_set_state()`，由后者在 O 内重新校验；不得在调用前解引用 `op`。

这是 pause/notify 接口的强制实现约束，不是编码建议。

### 2.7 生命周期/竞争场景

| 场景 | 期望 |
|---|---|
| 目标 GOT_IP、未 finish，到期 | pause 命中并消费 |
| 目标未到 GOT_IP | arm 返回 `INVALID_STATE` |
| 错误 ID | arm 返回 `NOT_FOUND` |
| 目标已 `finish_registered`/`expiring` | arm 返回 `INVALID_STATE` |
| 已存在未超时的旧全局 hook | 目标 arm 返回 `INVALID_STATE`，旧 hook 保留 |
| 已存在已超时的旧全局 hook | arm 清除 timeout 后成功 |
| 已存在有效目标 hook | 目标 arm 返回 `INVALID_STATE`，不覆盖 |
| 已存在失效目标 hook | arm 清除 `target_stale` 后重新武装 |
| 生产 cancel（`request_finish(CANCELLED)`） | notify 清除 `target_cancelled`，不 pause；新目标可立即 arm |
| `app_operation_cancel` 的 cancel pause | 清除 `target_cancelled`，不 pause |
| cancel 后新目标到达 GOT_IP | 可立即 arm，不等待旧 hook timeout |
| 其他 operation expire | 目标仍有效则 skip、不消费；目标已失效则 `target_stale` |
| 目标 terminal 后槽位复用 | 下一次 arm 按 ID/generation 找不到目标，清除 `target_stale` |
| arm/clear 并发 | O 串行化，最后一次操作生效，无撕裂 |
| pause/notify 与 clear 并发 | O 内串行；命中前 clear 则 skip |
| 超时未命中 | pause 时自动清除并记录 timeout；再次 arm 前也检查并清除 |
| 超时后再次 arm | 成功，不等待外部 clear |
| `request_finish` PENDING | 不发布终态、不 notify；hook 保留到 timeout；不得记为已清除 |
| `request_finish` STALE | 不发布终态、不 notify；由 arm stale/pause/timeout 兜底 |
| hold_ms 小于 remaining+10 s | arm 返回 `INVALID_ARG`，不武装 |
| `app_operation_cleanup_complete` | 不发布终态，不触发 notify |
| op_worker 与 expire 任务同时到期 | 两者都在 O 内选目标；`expiring` 使第二个跳过；hook 只命中一次 |
| arm 后目标 GOT_IP deadline 被重写 | 设计禁止在非 GOT_IP 武装，避免该窗口 |

### 2.8 BLE 诊断入口

复用 `diag.testhook`（仅 `APP_C1_TEST_HOOKS`，仅 BLE 来源，非 v1 Contract）：

- `action=arm` 且带 `operation_id`：调用目标 arm；未提供 `hold_ms` 时默认 `310000`；响应返回 `armed`、`generation`、`deadline_ms`、`remaining_ms`、`hold_ms`；
- `action=arm` 不带 `operation_id`：保持旧全局 hook 的现有行为（`hold_ms` 取值 100–30000，非法值回落 8000）；
- 目标 arm 的 `hold_ms` 允许 100–360000；实验默认 `310000`（GOT_IP TTL 300 s + 10 s 余量），`360000` 只是上限，不是常规等待时间；
- arm 时计算 `remaining_ms = deadline_ms - now_ms`；若 `hold_ms < remaining_ms + 10000` 返回 `invalid_param`，不武装；成功响应必须回传 `remaining_ms`，供操作者判断余量；
- 目标不存在时返回 `not_found`；非 GOT_IP、已 finish 或已有有效 hook 时返回现有 test-only 错误名 `not_active`；参数非法返回 `invalid_param`；均不改变 hook；
- 若已有目标 hook 但目标已失效，arm 先清除 stale 再武装，响应仍为成功；
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

### 3.2 调用方式与 `s_begin_diag` 同步（R3）

- 生产入口 `app_operation_start_with_finish()` 签名不变，内部调用公共实现并传 `NULL` diag；
- test-only 入口 `app_operation_start_with_finish_diag(..., app_operation_start_diag_t *diag)` 传本次调用结构；
- 公共实现入口先校验 diag 并清零；所有返回路径写 `start_result`；
- `owner != INTERNAL` 时置 `defer_attempted=true`，在 `app_mqtt_defer_restart_begin()` 返回后立即写 `defer_result`；失败时 `start_result=defer_result` 并返回；成功时 `defer_started=true`；
- 进入 O 后置 `active_check_done=true`、写 `active_check_result`；存在 active operation 时 `start_result=ESP_ERR_INVALID_STATE`；
- no slot/worker 创建失败写 `ESP_ERR_NO_MEM`；成功写 `ESP_OK`；
- diag 结构由调用方栈上持有，**不跨调用共享**；INTERNAL owner 不记录 defer，`defer_attempted=false`。

`cmd_net_job` 的共享快照约束：

- 调用 `app_operation_start_with_finish_diag(..., &local_diag)` 时，`local_diag` 必须是 `app_cmd_net_job_begin_locked` 栈上的局部变量；
- 调用返回后，在**同一 J 临界区**内把本次结果复制到既有 `s_begin_diag`；
- 禁止把全局 `s_begin_diag` 的地址传给 operation 层；
- `s_begin_diag` 的 reset/write 都在 `app_cmd_net_job_begin_locked` 内、由调用方持 J 完成；
- `app_c15_busy_diag_begin_snapshot()` 当前实现不取锁，调用者必须持 J；现有调用点在 `cmd.c` 的 begin 失败分支内、仍持 J，并先复制到局部 `bd`，解锁后的日志只使用该局部副本；
- 建议在头文件注释或函数名上标注 `_locked` 约束，避免未来误用；改名本身不是本设计的强制项；
- 若未来新增不持 J 的读者，必须新增取 J 的快照接口，不得直接读 `s_begin_diag`，也不得在 O 内访问它；
- `app_c15_busy_diag_capture_locked()` 继续遵守“调用方持 J”，其内部取 O 形成既有 J→O，不允许 O→J。

### 3.3 诊断映射

`cmd_net_job` 在 begin 诊断中记录：

| 条件 | 结论 |
|---|---|
| `active_check_done && active_check_result` | BUSY 来自 active operation |
| `defer_attempted && !defer_started && defer_result==ESP_ERR_INVALID_STATE` | BUSY 来自 MQTT restart |
| `defer_attempted && !defer_started && defer_result==ESP_ERR_NO_MEM` | MQTT defer 定时器资源失败，不是 no_slot |
| `defer_attempted && !defer_started && defer_result!=ESP_OK` | 按 defer 错误记录，不映射为 busy |
| `defer_started && start_result==ESP_ERR_NO_MEM` | no_slot/worker 资源失败 |
| 其他 | 按 `start_result`/begin 枚举记录 |

生产路径、defer 顺序与错误处理不变；仅在 `APP_C15_BUSY_DIAG` 构建启用 diag 入口。

### 3.4 MQTT defer 锁序逐路径证明（R4）

`app_mqtt_defer_restart_begin()` 的返回路径：

| 路径 | M 锁状态 | 返回值 |
|---|---|---|
| `!s_mqtt.started` | 未取 M | `ESP_OK` |
| `app_mqtt_defer_timer_ensure()` 失败 | 未取 M | `ESP_ERR_NO_MEM` |
| `s_restart_active` | 取 M 后 unlock | `ESP_ERR_INVALID_STATE` |
| 正常 defer | 取 M、修改 depth、unlock | `ESP_OK` |
| `esp_timer_start_once` 失败 | 取 M、回退 depth/deferred、unlock | 定时器错误 |

因此成功返回时 M 已释放；失败返回时也先 unlock（或从未取 M）。调用方随后才进入 O，不存在 M→O 嵌套。

`app_mqtt_defer_restart_end_at()` 在 operation.c 的 busy/no_slot/no-callback/terminal 路径中均在 O 释放后调用；调用方可能仍持 J（如 cmd_net_job begin 路径），因此形成既有 J→M，但绝不在 O 内调用。其内部取 M、释放，再可能调度重启，不持有 O。

diag 结构只写调用方栈，不修改 `s_restart_defer_depth`、`s_restart_deferred`、`s_restart_pending` 或 defer 定时器；因此不改变现有 defer 生命周期。

## 4. Host 测试矩阵

| 编号 | 场景 | 期望 |
|---|---|---|
| H1 | GOT_IP 目标正确 id，expire 到期 | hit 一次，generation/deadline 与 arm 响应一致 |
| H2 | 错误 id | arm `NOT_FOUND` |
| H3 | 目标非 GOT_IP | arm `INVALID_STATE` |
| H4 | 目标已 finish/expiring | arm `INVALID_STATE` |
| H5 | 已有全局 hook 时目标 arm | `INVALID_STATE`，旧 hook 保留 |
| H6 | 生产 cancel（request_finish CANCELLED） | notify 清除 `target_cancelled`，不 pause |
| H7 | `app_operation_cancel` cancel pause | 清除 `target_cancelled`，不 pause |
| H8 | cancel 后新目标到达 GOT_IP | 立即 arm 成功，不等待旧 hook timeout |
| H9 | 其他 operation expire | 目标仍有效则 skip、不消费 |
| H10 | 目标 terminal 后槽位复用 | arm 清除 `target_stale` 并成功 |
| H11 | 目标经 `set_state(FAILED/CANCELLED)` 终态 | notify 清除 `target_terminal`/`target_cancelled` |
| H12 | arm 后 clear | 不 hit |
| H13 | 超时未命中（pause 路径） | timeout 清除 |
| H14 | 超时后再次 arm | arm 先清除 timeout，再成功武装新目标 |
| H15 | arm/clear/pause/notify 并发压力 | 无撕裂、无重复消费、无锁内日志 |
| H16 | op_worker 与 expire 任务竞争 | 只 finish 一次、只 hit 一次 |
| H17 | defer 返回 `ESP_ERR_INVALID_STATE` | diag 记录 MQTT 来源 |
| H18 | defer OK、active operation | diag 记录 operation 来源 |
| H19 | defer 返回 `ESP_ERR_NO_MEM` | 原错误返回，不映射 busy |
| H20 | INTERNAL owner | `defer_attempted=false` |
| H21 | 解锁后日志路径 | 仅使用局部快照；静态审查无 `op->` 解锁读取 |
| H22 | hold_ms 小于 remaining+10 s | arm `INVALID_ARG`，不武装 |
| H23 | `request_finish` PENDING | 不 notify；hook 保留到 timeout，测试不得记为已清除 |
| H24 | `request_finish` STALE | 不 notify；stale 失效路径清除 |
| H25 | 终态发布后、notify 前槽位复用 | notify 按旧 ID/gen 只清旧目标，不读取新 operation 字段 |
| H26 | `cleanup_complete` 调用 | 不发布终态、不触发 notify |
| H27 | `request_finish` apply 失败 | 不 notify；目标 hook 保留，由 stale/timeout 兜底 |

Host 测试需覆盖 `APP_C1_TEST_HOOKS`、`APP_C15_HOOK_TARGET`、`APP_C15_BUSY_DIAG` 的组合编译；测试构建单独定义，生产构建不定义。

## 5. 编译隔离

- 新增 `APP_C15_HOOK_TARGET`，默认 OFF；CMake 要求同时 `APP_C1_TEST_HOOKS=ON`，否则 FATAL_ERROR；
- `APP_C15_HOOK_TARGET` 控制目标 arm、notify 与 `diag.testhook` 的 `operation_id` 分支；
- `APP_C15_BUSY_DIAG` 控制 start diag 结构与 test-only 入口；
- 生产构建两者均 OFF；生产 ELF 用 `nm` 检查 `app_operation_test_hook_arm_target`、`app_operation_start_with_finish_diag` 符号为 0；
- `tests/operation_host/Makefile` 的 `DEFS` 增加 `-DAPP_C15_HOOK_TARGET=1`（仅测试构建）；`tests/cmd_net_job` 保持 `APP_C15_BUSY_DIAG=1`；
- 不新增 BLE 命令名，不改变 v1 Contract 或生产状态机；`diag.testhook` 属实验构建专用入口。

## 6. 验收条件

| 编号 | 验收条件 | 证据 |
|---|---|---|
| D1 | 目标 cancel、目标 terminal、槽位复用均能清除对应目标 hook | H6–H11、H14、H25；新目标 cancel 后立即 arm |
| D2 | 非目标 expire 不消费 hook | H9 |
| D3 | pause/notify 解锁后只使用局部快照，不再读取 operation 实时字段 | H21/H25 + 静态审查 |
| D4 | arm/clear/pause/notify 及旧全局 hook 均由 O 保护 | H15 + 静态审查 |
| D5 | `s_begin_diag` 读写同步约束明确 | §3.2；写/读均在 J 内 |
| D6 | MQTT defer 的锁获取、释放及错误返回路径有源码依据 | §3.4 |
| D7 | H1–H27 对应更新后的行为，含新目标 cancel 后立即 arm、PENDING/STALE、apply 失败 | Host 结果 |
| D8 | 生产构建仍不包含新增 test-only 符号 | `nm` 检查 |

D3/D5/D6/D8 属静态审查或构建验证，不以 Host 测试结果替代；Host 通过不证明设备行为。

## 7. 未决问题

1. `diag.testhook` 目标 arm 的响应是否再增加 `state` 字段（当前草案已含 `deadline_ms`/`remaining_ms`）；
2. Host 并发压力测试的线程模型与可重复性；
3. `APP_C15_HOOK_TARGET` 是否与 `APP_C15_BUSY_DIAG` 合并为单一实验宏，或继续保持独立以缩小组合面；
4. 实现阶段需复核 `main@5aaabea` 是否确实没有 PENDING finish 的外部 resume 路径；若未来新增，必须在该终态发布点补按值 notify，不能依赖 timeout 兜底。

## 8. 边界

- 本文只描述设计，不修改源码、不构建、不运行 Host/设备测试；
- 当前 hook 实现尚未满足目标关联、同步与目标失效清理要求；
- 设计通过并实现、Host 验证后，才重新评审设备实验；
- 不启动 P2；
- C15-2 保持 `BUSY_REJECTED / NOT_COVERED`，CMD-15 保持 `TEST_PENDING`。
