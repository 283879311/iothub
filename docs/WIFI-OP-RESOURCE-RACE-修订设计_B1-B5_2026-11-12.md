# WIFI-OP-RESOURCE-RACE · 修订设计（B1–B5 全量覆盖，2026-11-12）

> 评审输入：`wx-ble-provision/docs/WIFI-OP-RESOURCE-RACE-设计评审_2026-11-12.md` §5 裁决。
> 本文件**只设计、不含代码**；代码修改与烧录需单独批准。
> 证据：`docs/evidence/WIFI-OP-RESOURCE-RACE/c8e4-12-serial.log`（973 行，SHA256
> `3415217d…322f034f`，与客户端仓存档同一份）。

## 0. 对裁决的响应摘要

| 项 | 裁决 | 本设计的响应 |
| --- | --- | --- |
| B1 | P0：CANCEL_CLEANUP 成为 owner 枚举值；apply_cfg 为其内部子阶段 | §2：owner 六值；§3：**无 NONE 窗口的移交协议**（TRYX/AUTO/APPLY 释放时直接 handover 到 CLEANUP）；apply_cfg 全程在 CLEANUP 内，不再 claim APPLY |
| B2 | P0：claim_auto 检查完整仲裁态 | §4：claim 统一入口，检查 owner 六值；`cleanup_pending` 退出历史（不再作为第二套所有权） |
| B3 | P0：新增 CLEANUP_FAILED，失败不得转 NONE | §5：CLEANUP 内重试 3 次（1/2/4s）→ 仍失败转 **CLEANUP_FAILED（粘性）**；auto 不得抢占、新 op 返回既有 `cleanup_failed`；清除入口 = 显式重配（config.set → reboot） |
| B4 | P0：9 点覆盖表 | §6：逐点填写，含触发来源/上下文/现有检查/修订后归属/失败处理 |
| B5 | P1：caller 标签，调度时刻打印 | §7：`schedule_connect_task(delay, caller)` + 调度瞬间日志；覆盖 8 个调度点 + resume 调用者 |

**Q1.4 已用归档日志定案（★ 补交）**：199650429 那发 auto_connect 的调度源 =
**旧 op 的 `app_wifi_tryconnect_finish()` → `schedule_connect_task(400ms)`（wifi.c:3510）**。
推理链（唯一与时间线一致）：
1. `stale after wait` 199645179 证明旧 worker 的 `try_connect_ip` 已返回 ⇒ finish 在其前，
   400ms 后任务创建 ≈1996455xx；
2. 新 op 的 `wait_connect_task_idle` 自 199645189 起等待，199650429 任务清指针才返回
   （402ms 后即预扫描）——与日志 199650449 完全吻合；
3. 199645179–199650419 之间无任何 wifi 行 ⇒ 该任务在执行**静默成功的扫描**
   （`connect_best_saved_profile` 非 fast_path 路径，scan 成功不打印）；
4. 其余五个调度源当时条件均不成立（无 STA_START/无 APPLY/无断连事件/monitor 被
   tryconnect 活跃挡住）。
> B5 落地后此结论可由调度行 100% 证实；在此之前以"高置信推断"记档。

## 1. 概念模型（终态 ≠ 资源释放）

- **owner（资源所有权）**：任何时刻谁有权驱动 STA（scan/connect/disconnect/set_config/stop/start）。
- **operation 状态**：业务生命周期（accepted→…→terminal），**不承载**资源语义。
- **cleanup 完成**：CLEANUP 所有者完成 apply_cfg（成功→NONE；彻底失败→CLEANUP_FAILED）。
  释放原则（评审 §5 补充）：
  > owner 的释放必须发生在异步任务真正退出、或其后续 STA 操作已被可靠禁止之后；
  > 不能因为函数返回、operation 状态改变或 generation 过期就认定资源已释放。

本设计逐条落实：TRYX 在 `finish` 的驱动调用**全部完成后**才释放/移交；AUTO 在
`connect_profile_index` 返回（驱动调用已入队）后才释放/移交；CLEANUP 在
`apply_cfg`（含 `esp_wifi_stop/start` 与 A8 await）返回后才释放。

## 2. owner 枚举与不变量

```
APP_WIFI_OP_NONE = 0
APP_WIFI_OP_AUTO            /* 后台自动连接（task_fn → best_saved_profile 全程） */
APP_WIFI_OP_TRYX            /* tryconnect 生命周期（begin → finish） */
APP_WIFI_OP_APPLY           /* boot / 显式 config apply（非 cleanup） */
APP_WIFI_OP_CLEANUP         /* cancel/expire 的恢复链（含 apply_cfg 子阶段） */
APP_WIFI_OP_CLEANUP_FAILED  /* 粘性失败态：禁止一切 STA 发起，直到显式重配 */
```

不变量：
1. 同一时刻只有一个 owner；
2. 只有 owner 可以调用 9 个 STA 驱动点（§6 逐点证明）；
3. CLEANUP/CLEANUP_FAILED 期间 **AUTO 不得 claim**（B2）；
4. CLEANUP_FAILED 只能由"显式重配（config.set → reboot）"清除（重启后回到 NONE）；
5. 任何 release 都是"交接或归零"，**不允许出现 NONE 中间态后再被别人抢走**（§3）。

## 3. 无窗口移交协议（B1 核心）

所有权统一由 `s_tryconnect_mux`（现有 lock）保护，新增两个原语：

```
cleanup_request():                       /* cmd.c cleanup 调用 */
  lock
  switch (owner):
    NONE      -> owner = CLEANUP;         notify(restart)
    TRYX/AUTO/APPLY -> s_cleanup_requested = true   /* 由持有者释放时移交 */
    CLEANUP   -> no-op
    CLEANUP_FAILED -> 返回 false（cleanup 已失败，operation 记 cleanup_failed）
  unlock

release_or_handover(from):               /* 所有 release 的唯一出口 */
  lock
  if (owner == from):
      if (s_cleanup_requested): owner = CLEANUP; s_cleanup_requested = false; notify(restart)
      else:                     owner = NONE
  unlock
```

持有者释放点（全部改为调用 `release_or_handover`）：
- `app_wifi_tryconnect_release()`（TRYX 唯一释放点，wifi.c:287）；
- `auto` 释放点（`app_wifi_connect_best_saved_profile` 的**单一出口**，含所有 return）；
- `app_wifi_op_release_apply()`（APPLY）；
- restart 任务：成功 → `owner=NONE`；彻底失败 → `owner=CLEANUP_FAILED`。

**为什么没有窗口**：cleanup 请求要么直接取得 CLEANUP（owner 空闲），要么把
`s_cleanup_requested` 置位（owner 忙），而**所有释放点在同一把锁内检查该位并直接
移交 CLEANUP**。不存在"先归零、再被别人 claim、再回来要"的中间态。

**新 op 等待**：`claim_owner(TRYX, 8000ms)` 轮询：
- `owner==NONE` → CAS 取得，继续；
- `owner==CLEANUP` → 继续等（restart 任务正在恢复，通常 <2s）；
- `owner==CLEANUP_FAILED` → **立即**返回 `cleanup_failed`（不等待、不绕过）；
- 超时 → 返回 `busy`（瞬态，客户端已按 1.5s×3 重试）。
**一个等待替代原 A3 的两个等待**（采纳 S2，8s 单值）。

## 4. claim 矩阵（B2）

| 发起者 | 允许的 owner 状态 | 等待 | 被拒结果 |
| --- | --- | --- | --- |
| TRYX（tryconnect begin） | NONE；CLEANUP（等） | ≤8s | `busy`；遇到 CLEANUP_FAILED → `cleanup_failed` |
| AUTO（best_saved_profile 入口） | NONE | 不等待（非阻塞） | 不触碰 STA；置 `fast_path_pending=true` 退出，由 monitor 1s 重试 |
| APPLY（boot / 显式 config） | NONE | ≤15s（沿用） | 放弃本轮，记 `APPLY_BUSY_*` |
| CLEANUP（restart 任务） | 由 request/handover 取得，不再 claim | — | 取得后全程持有 |

`cleanup_pending` 标志**删除**（B2）：pending 语义并入 owner=CLEANUP。

## 5. CLEANUP 内部时序与失败（B3）

```
restart 任务（单实例，wifi_restart）
  wait_owner(CLEANUP)（由 request/handover 保证，8s 上限）
  ├─ 尝试 1: apply_cfg() （全程持 CLEANUP；含 stop/mode/set_config/start + A8 await）
  │    ├─ 成功 → owner=NONE；日志 cleanup complete op=<id> result=ok
  │    └─ 失败 → 1s
  ├─ 尝试 2: apply_cfg() → 失败 → 2s
  ├─ 尝试 3: apply_cfg() → 失败 → 4s
  └─ 仍失败 → owner=CLEANUP_FAILED（粘性）
       日志 cleanup failed op=<id> stage=<stage> err=<n>（含 op_id 与阶段）
```

- `apply_cfg` 的既有恢复链（LKG→DHCP→离线）在每次尝试内部保持不变；
- 三次尝试全失败才进入 CLEANUP_FAILED，避免瞬态（DHCP 慢）误判；
- CLEANUP_FAILED 语义：
  - TRYX → `cleanup_failed`（既有 wire 码，op_terminal 类，**不复用
    `wifi_config_offline`**）；客户端按"设备清理失败，需检查/重配"处理；
  - AUTO → **不发起任何 STA 操作**（连记录日志后退出），monitor 重试也被拒；
  - 清除：`config.set`（任一域，`effect=reboot`）→ 重启后 owner 归 NONE；
    或物理重启。无其它自动清除路径（B3 要求）。

> 代价如实说明：进入 CLEANUP_FAILED 后设备 Wi-Fi 保持离线直到显式重配。这是
> "失败不得假装释放"的必然取舍；三次重试把误判概率压到最低。

## 6. B4 · 9 个 STA 驱动调用点覆盖表

| # | 调用点（wifi.c） | 触发来源 | 执行上下文 | 现有 owner 检查 | 修订后归属 | 失败处理 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | `connect_profile_index` :939/942/960 | auto task → best_saved → profile 分支 | `wifi_auto_connect` 任务 | 无（仅 `connect_inflight` 计数 + `apply_hold`） | **AUTO**（由 #2 入口统一 claim；本函数为内部子阶段，不再单独 claim） | claim 失败不进入；#2 置 fast_path_pending 退出，monitor 重试 |
| 2 | `connect_best_saved_profile` :1035/1036/1050（fast_path） | auto task（唯一执行入口） | 同上 | 无 | **AUTO 的唯一 claim 点 + 唯一 release 点**（所有 return 走 release_or_handover） | claim 失败：不触碰 STA，置 pending 退出 |
| 3 | `retry_with_pmf_required` :1520/1526 | `event_handler` reason=210（每次启动仅一次） | `wifi` 事件任务 | 无 | **AUTO（非阻塞 claim）** | claim 失败：跳过本次 PMF 重试，记 `pmf_retry deferred: owner=%d`，交 monitor |
| 4 | `event_handler retry_direct` :2157 | `STA_DISCONNECTED` 直连重试（`sta_retries<3`，BLE 未连接） | `wifi` 事件任务 | 仅 `app_wifi_connect_begin("retry_direct")`（计数，不互斥） | **AUTO（非阻塞 claim）** | claim 失败：本拍不直连，保留重试计数与退避，monitor 接续 |
| 5 | `netcheck_on_result` :2352（disconnect） | netcheck 判决 `fail_internet`（观察者回调） | `netcheck` 任务 | 仅 `link_gen` 匹配（事件归属，不是所有权） | **AUTO（非阻塞 claim，仅覆盖本次 disconnect）**；gen 不匹配仍直接丢弃 | claim 失败：跳过拆链（记日志），判决已归档不受影响；monitor/退避接续换 profile |
| 6 | `apply_cfg` :2999（含 stop/mode/start） | boot（`source=boot`）、restart（`runtime_restart`）、恢复链（LKG/DHCP） | boot 任务 / `wifi_restart` 任务 | 调用者持 `APPLY`（boot/显式）；cleanup 路径**无独立 claim** | **boot/显式 = APPLY；cleanup 恢复 = 在 CLEANUP 内执行（B1）** | 每次尝试内部走既有 LKG→DHCP→离线；CLEANUP 内 3 次失败 → CLEANUP_FAILED |
| 7 | `perform_scan_impl` :3426 | ① tryconnect 预扫描（#9 内）② auto profile 扫描（#2 内）③ `network.scan` 命令 | worker / cmd 任务 | 无 STA 状态检查（仅 20s 扫描锁） | **随调用者：① TRYX ② AUTO ③ 命令扫描先非阻塞 claim AUTO** | ① 500ms 重试一次后 `wifi_scan_failed`（保留）② 记 scan_failed 并让 monitor 重试 ③ 忙则回 `busy`（客户端 1s 重试） |
| 8 | `tryconnect_finish` :3498/3501 | tryconnect 所有退出路径 | worker / cmd 任务 | TRYX 持有中 | **TRYX 内**；disconnect/set_config 完成后走 `release_or_handover(TRYX)`（有 cleanup 请求→移交 CLEANUP） | restore 失败：记日志，仍按协议释放/移交 |
| 9 | `try_connect_common` :3661/3679/3688 | tryconnect 主体（拆旧链/设候选/连候选） | worker / cmd 任务 | TRYX 持有中 | **TRYX 内** | 各阶段既有分类；取消/失败统一经 #8 finish 释放 |

补充说明（不能以"上层已检查"代替逐点证明）：
- #3/#4/#5 是**事件上下文**，claim 一律非阻塞（事件任务不得阻塞），被拒即让位并留日志；
- #6 的 boot 路径与 cleanup 路径**互斥**：boot 只发生在启动期（无 operation），
  cleanup 只在运行期；两者不会同时存在；
- #7 的三种调用者分别继承调用者的 owner，**不存在"无 owner 的扫描"**；
- 9 点之外，`esp_wifi_stop/set_mode/start` 只出现在 #6 内部，随 #6 的 owner 生效。

## 7. B5 · 可观测性（调度时刻打印）

接口变更：`app_wifi_schedule_connect_task(TickType_t delay_ticks, const char *caller)`。

调度瞬间（无论是否创建任务成功）打印：

```
auto_connect: scheduled by <caller> delay=<ms> owner=<name>
auto_connect: schedule refused by <caller> delay=<ms> reason=<suspended|offline|task_busy|mode>
```

caller 取值（覆盖评审要求的五类）：
| caller | 位置 |
| --- | --- |
| `apply_start_released` | wifi.c:1942 |
| `sta_start` | wifi.c:1986 |
| `sta_disconnected_ble` | wifi.c:2167 |
| `apply_start_probe` | wifi.c:3111 |
| `tryconnect_finish` | wifi.c:3510（**Q1.4 元凶，必须可追溯**） |
| `monitor_runtime_retry` | wifi.c:1342 |
| `monitor_ble_fast` | wifi.c:1369 |
| `monitor_startup` | wifi.c:1376 |

另外三行（可选但建议同批）：
- `auto_connect: resume by <caller>`（`app_wifi_auto_connect_resume` 加 caller）；
- `op_owner: <from> -> <to> by <caller>`（claim/release/handover 统一日志）；
- `cleanup complete/failed op=<id> result=<ok|failed> attempts=<n>`。

## 8. 验收场景（采纳 S1/S3）

| 编号 | 操作 | 通过判据（串口） |
| --- | --- | --- |
| WIFI-RACE-01 | auto_connect 与 tryconnect 同时触发 | 任一时刻只有一个主体发起 STA 操作：`auto_connect: schedule refused ... reason=owner_busy` 或 `AUTO claim denied owner=TRYX`；不得出现两段 disconnect→connect 交叠 |
| WIFI-RACE-02 | cancel 后立即新 tryconnect | 新 op 受理后 `claim_owner(TRYX) waiting owner=CLEANUP`；`cleanup complete` 后预扫描**真正启动并完成**；终态 = **`failed/ap_not_found`**（目标不存在），**不得以 `busy` 收尾**、不得 `wifi_scan_failed`（S3） |
| WIFI-RACE-03 | 恢复配置与 auto_connect 同时 | **CLEANUP 持有期间无任何 STA 发起操作**（scan/connect/disconnect/set_config 一律无）；`cleanup complete` 之后首条 STA 操作来自谁都可以（S1，不限定顺序） |
| 附加 | CLEANUP 失败 | `cleanup failed ... attempts=3` → owner=CLEANUP_FAILED；随后 tryconnect 返回 `cleanup_failed`；auto 全程无 STA 操作；`config.set + reboot` 后恢复 |

## 9. 改动清单（预计 ~110–140 行；比上版多 20–40 行，换来 B1–B5 全覆盖）

| 文件 | 函数/对象 | 改动 |
| --- | --- | --- |
| wifi.c | `app_wifi_op_owner_t` + claim/release 原语 | 六值枚举；`claim_owner(who, wait_ms)`；`release_or_handover(from)`；`cleanup_request()` |
| wifi.c | `app_wifi_tryconnect_begin` | 改为 `claim_owner(TRYX, 8s)`；CLEANUP_FAILED → 立即失败 |
| wifi.c | `app_wifi_tryconnect_release` | 改为 `release_or_handover(TRYX)` |
| wifi.c | `app_wifi_connect_best_saved_profile` | 入口 claim AUTO（非阻塞，检查六值）；所有出口 release_or_handover |
| wifi.c | `app_wifi_connect_task_fn` | 延时后复核 owner==NONE；不满足则置 pending + 记 `schedule refused` |
| wifi.c | `app_wifi_event_handler`（retry_direct / PMF） | 各自非阻塞 claim AUTO，被拒让位 |
| wifi.c | `app_wifi_netcheck_on_result` | disconnect 前非阻塞 claim AUTO，被拒跳过 |
| wifi.c | `app_wifi_perform_scan_command` | 改为 claim AUTO（非阻塞），删除 suspend 组合 |
| wifi.c | `app_wifi_perform_scan_impl` | 断言调用者 owner ∈ {TRYX, AUTO}（debug 构建断言，release 记日志） |
| wifi.c | `app_wifi_restart_task_fn` | 区分 CLEANUP/APPLY 请求；CLEANUP 三次重试；成功 NONE / 失败 CLEANUP_FAILED；清理日志 |
| wifi.c | `app_wifi_schedule_connect_task` | 加 caller 参数 + 调度瞬间日志（B5） |
| wifi.c | monitor | 删除 `cleanup_pending` 相关；CLEANUP_FAILED 下不调度；可选 CLEANUP 卡死告警 |
| cmd.c | `app_cmd_net_async_cleanup` | 调 `app_wifi_cleanup_request()`；返回值语义：false → operation `cleanup_failed` |

**不做**：不改 operation 状态机、不改错误码契约（复用既有 `cleanup_failed`/
`busy`/`wifi_scan_failed` 语义）、不取消 auto_connect、不加新任务/新模块、
不改 120s BLE 恢复路径（AUTO 被拒只延迟）。

## 10. 遗留风险与开放问题

1. **CLEANUP 持有时长**：apply_cfg 含 `esp_wifi_stop/start` + A8 await（实测
   数百 ms~2s）；若 DHCP 慢，三次重试最坏 ~10s 内 owner=CLEANUP，期间新 op 得
   `busy`——可接受（客户端瞬态重试），但需在 RACE-02 判定里允许"先 busy 后成功"的
   轨迹（以最终 ap_not_found 为准）。
2. **AUTO 被拒后的恢复延迟**：monitor 1s 节拍 → 最坏 1s 额外延迟，BLE 恢复路径语义不变。
3. **CLEANUP_FAILED 的可运维性**：仅显式重配可清除；现场需文档说明（建议写入
   故障排查手册，非本轮）。
4. **#6 的 APPLY 与 CLEANUP 并存**：由"boot 期无 op / 运行期 cleanup"的时间互斥保证；
   若未来新增运行期显式 apply 命令，需重新评审（当前无此命令）。
