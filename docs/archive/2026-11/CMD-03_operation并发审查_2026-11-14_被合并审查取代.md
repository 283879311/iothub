> 归档说明：该独立审查已被 [CMD-02/03 并发静态审查](../../CMD-02_03_并发静态审查_2026-11-15.md) 合并覆盖。保留原文供审计，不再作为当前独立结论维护。

---

> **状态**：ACTIVE（CMD-03 审查结论，待评审；本轮只审查未改代码）

# CMD-03 · operation.c 状态机/worker 生命周期/cleanup 交互审查（2026-11-14）

> 基线：`dc254d5`（与 CMD-02 同基线）。operation.c 1,127 行 / `operation_state.{h,c}`
> 纯逻辑 / `s_ops_mux` spinlock / 两任务（op_worker、op_expire，均懒创建、不退出）。
> 目标：**逐条验证 CMD-02 传递的两条硬约束**，并完成状态机与生命周期审查。

## 0. CMD-02 两条硬约束的验证结论（先给结果）

| 约束 | 结论 |
| --- | --- |
| **CMD-03-A**：worker 对 `job->active` 的锁外读受 operation stale 守卫保护，且生命周期前提成立 | **确认成立**（§2，含 entry-read 竞态的完整时序分析） |
| **CMD-03-B**：`run_fn` 非零返回时"非活动 op 不覆盖"的守卫保持 | **确认成立**（`expiring` 闸 + `app_op_state_is_active` 双守卫，run_pending 尾部） |

## 1. 结构基础

- **槽位数组**：`s_ops[APP_OPERATION_MAX_SLOTS=4]`，`s_ops_mux` spinlock 保护；
  槽位含 `generation`（复用 +1 且跨 memset 保留，R4）、`running`（R5：run_fn 在飞
  禁止本槽回收/复用）、`expiring`（R3-T1/T2：cleanup 在飞闸）。
- **两任务**：`op_worker`（通知驱动 + 1s 兜底节拍；reclaim → expire_one →
  run_pending）与 `op_expire`（3072B 小栈，1s 节拍只跑 expire_one——DEF-MQTT-002
  解耦：run_fn 阻塞不再停摆 TTL）。**两任务均不退出**（for 循环），满足"owner
  释放在异步任务真正退出或后续操作被可靠禁止之后"的前置。
- **纯逻辑转移表**（operation_state.c）：`app_op_transition_allowed` 逐态白名单
  （ACCEPTED→CONNECTING/CANCELLED/FAILED/EXPIRED；CONNECTING→GOT_IP/…；
  GOT_IP→COMMITTING/…；COMMITTING→SUCCESS/FAILED；terminal→仅 FREE=槽位回收）。

## 2. CMD-03-A · worker `job->active` 锁外读的生命周期闭环

### 2.1 run_fn 入口读只会发生在"合法领取"之后

`app_operation_start`：槽位选择（跳过 used 槽，R5 的 running 槽必 used）→
`memset(slot)+generation+1` → state=ACCEPTED → `ensure_worker()` →
`xTaskNotifyGive(s_worker)`。worker 醒来 → `run_pending()`：**只在
`state==ACCEPTED && !expiring` 时领取**，领取即 `state=CONNECTING; running=true`
（同一 s_ops_mux 临界区），然后才调 `run_fn(op, ctx)`。

⇒ run_fn 入口读 `job->active` 时，必然处于"本 op 刚被本 worker 领取"的状态；
**cancel 若在领取前发生**（state=CANCELLED），run_pending 不会领取
（非 ACCEPTED）→ **run_fn 根本不执行，入口读不存在**。

### 2.2 领取后 cancel 的三个时序，逐一时序闭合

| 时序 | 过程 | 结局 |
| --- | --- | --- |
| a) run_fn 内（阻塞于 try_connect_ip） | cancel：cleanup_fn（job memset，active=false）→ finalize 置 CANCELLED | worker 返回后：cmd.c stale 守卫（generation+CONNECTING）→ **no job touch**；run_pending 尾部 expiring=false + state 非活动 → 不覆盖 FAILED |
| b) cancel 的 cleanup_fn 尚在执行（expiring=true）时 run_fn 返回 | run_pending 尾部 `op->expiring` → **skip_for_cleanup**（R4：终态由 cleanup 独占裁决，run_fn 不改 FAILED——否则"对外 FAILED、cleanup 仍在执行"且 finalize 落空） | ✓ |
| c) run_fn 已经返回、`running=false` 已置、cancel 才开始 | 正常 cancel 路径（op 非 running） | ✓ |

### 2.3 generation 防串（CMD-02 场景 5 的状态机侧证据）

槽位复用：`oldest_terminal` 回收/空槽 → `memset(slot)` **前** `generation+1`（R4，
operation.c:360-375）⇒ 旧 worker 局部保存的 generation 与复用后必不等。
旧 worker 的 cmd.c stale 检查（`st1.generation != generation → no job touch`）
与 `app_operation_cancel`/`expire_one` 的 finalize（`op->generation == generation`
才写终态）双向使用同一守卫 ⇒ **旧任务不可能写坏新 op**。

## 3. CMD-03-B · run_fn 返回值的双守卫（expiring 闸 + active 判定）

`run_pending` 尾部（operation.c:589-612）：

```
err = run_fn(op, ctx);
running = false;                    ← 此后本槽才可回收/复用（R5）
if (err != ESP_OK):
    锁内：expiring → skip_for_cleanup；否则 active → FAILED/INTERNAL
    锁外：日志（R7：日志禁止入 spinlock——首测 abort 已修）
```

- `expiring` 闸由 `set_state` 的同一把 s_ops_mux 原子裁决（:431-437）：
  **"cleanup 已启动"与"进入 COMMITTING"不可能同时成立**——输掉的 apply 在读
  job 数据之前就被挡下（R2-P0/第三轮 fencing 依赖的状态机侧保证）；
- 双守卫顺序（expiring 先、active 后）保证 cleanup 独占终态裁决。

## 4. 其余审查项

### 4.1 set_state（:410-455）

- `expiring` 闸（§3）+ 转移白名单（非法迁移一律 INVALID_STATE）；
- GOT_IP 交付时重置 `deadline_ms = now + got_ip_ttl_ms`；COMMITTING 清
  deadline（不可取消/不过期）；terminal 记 `terminal_ms`；
- `defer_restart_end_at` 挂钩仅终态触发。

### 4.2 cancel / expire_one 的对称性

两者同一模式：`expiring=true`（闸）+ 锁内取 cleanup_fn/ctx/generation →
**退出 s_ops_mux 后**调 cleanup_fn（锁外，无 spinlock 串联）→ finalize
（generation + active 双守卫；cleanup 失败 → FAILED/cleanup_failed，
`expiring=false`）→ `defer_restart_end_at`。cancel 另有
`app_op_state_can_cancel` 前置与 `expiring` 重入拒绝。

### 4.3 槽位生命周期

- 回收：仅 terminal 且超 600s 保留期（`reclaim_terminal_locked`）；
- 槽压力：满表时回收最老终态（覆盖而非 no_slot）；`running=true` 的槽
  **不可回收**（R5）——run_fn 在飞期间 ctx（`&s_net_async`）不会被 memset；
- `get_status`：s_ops_mux 内整份快照拷贝（调用方拿副本）。

### 4.4 锁序

`s_ops_mux`(spinlock) 从不跨越 ESP_LOG（R7 修复已核实 :601-612）与
cleanup_fn（:487 EXIT → :489 cleanup）；`job_lock`(mutex) → `s_ops_mux`
单向（operation_start/set_state/get_status）；反向（expire 持 s_ops_mux 等
job_lock）不存在（cleanup 在 EXIT 后）。**无死锁路径。**

## 5. 发现

### 5.1 确定安全（保留现状）

§0-§4 全部；CMD-02 两条硬约束成立。**operation.c 本体未发现可触发数据竞争
或生命周期漏洞。**

### 5.2 非阻断建议（两项，P2）

1. **`operation_state.c` 转移表无主机单测**：它是纯逻辑（照 wifi_op_owner 先例
   可 30 行建测），当前仅被 wifi_owner 间接覆盖。建议补 `tests/operation_state`
   （转移白名单 + can_cancel + terminal 判定，~15 例）——CMD-02 P1 精神的延续；
2. **worker 串行队列延迟**：单 worker 下第二个 ACCEPTED op 的
   accepted→connecting 延迟最坏 ≤1s（run_pending 每循环无条件重扫，非仅通知
   驱动）。当前业务（单 op 模型 + busy 守卫）不受影响；若未来放开并发 op
   需重新评审本节全部结论。

### 5.3 需要补充证据：无

## 6. 结论

CMD-02 传递的两条硬约束**全部验证成立**；operation.c 状态机、worker 生命周期、
cancel/expire/worker 三方交互**闭环**。CMD-02 的审查结论（含 U1 前置条件、
gate v3 三前提）在状态机层面获得直接支撑。CMD-03 签收建议：按本文档通过，
CMD-06 迁移时按 §5.2-2 的提示重评。
