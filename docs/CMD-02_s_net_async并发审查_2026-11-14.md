> **状态**：ACTIVE（CMD-02 审查结论，待评审；本轮只审查未改代码）

# CMD-02 · `s_net_async` 并发审查（2026-11-14）

> 基线：`dc254d5`（含 49707a9/CMD-01，origin/main 与本地一致，无需另推）。
> 方法：四轮（字段清单 → 任务上下文 → 所有权分类 → 修复方案），全部结论带
> 文件:行号与锁边界证据；45 处 `s_net_async` 引用逐一映射到所在函数与任务上下文。

## 0. 基础事实

- **`s_net_job_lock` 是递归互斥**（`xSemaphoreTakeRecursive/GiveRecursive`，
  cmd.c:247-254；R8 评审注：apply 再入内层加锁需要递归语义）。
- **`clear_job` 双版本纪律成立**：直接版（要求调用方持锁）3 个调用点全部在锁内
  （cleanup:486、tryconnect op_start 失败:781、apply 收尾:1020/1050）；`_sync`
  版 2 个调用点均无锁（worker:582/:652）——配对无错位。
- **`s_ops_mux` 是 spinlock**（operation.c:49，portENTER_CRITICAL），短临界区；
  expire 任务在**退出临界区之后**才调 cleanup（operation.c:650，:328 EXIT 先于
  cleanup 调用）⇒ job_lock(mutex) 内调 operation API（进 s_ops_mux）与
  expire 路径（s_ops_mux → 退出 → job_lock）**无 AB-BA 死锁**（spinlock 不睡眠）。
- **受理段整体持锁**（初判虚惊已纠正）：`app_cmd_net_tryconnect_async` 的
  `app_cmd_net_job_lock()`（:745 附近）到 `app_cmd_net_job_unlock()`（:793）之间
  **无中途解锁**——memset/cfg_txn/active/owner/parent_ctx/strings/op_start/op/
  operation_id 全部在锁内。

## 1. 第一轮 · 字段读写清单

任务上下文代号：**D**=命令 dispatcher（ble_cmd 任务 / mqtt 任务，按来源）；
**W**=operation 异步 worker；**C**=cleanup（cancel/expire 调用方：NimBLE host 任务、
D、op_expire 任务）；**B**=BLE 断连回调（NimBLE host 任务）；**T**=TTL 过期任务；
**Q**=状态查询（D 同任务）。

| 字段 | 位置（行） | 操作 | 上下文 | 锁状态 |
| --- | --- | --- | --- | --- |
| `active` | :439 op_active | R | W（wifi monitor 经 API） | 🔒 锁内 |
| `active` | :709 tryconnect_async | R | D | ⚠️ 锁外（受理守卫，见 U1） |
| `active` | :757 受理 | W | D | 🔒 锁内 |
| `active` | :872 operation_status | R | Q | 🔒 锁内 |
| `active` | :2530/:2614/:2692 dispatcher | R | D | ⚠️ 锁外（bool 原子，见 O3） |
| `active` | :376 BLE 断连 | R | B | 🔒 锁内（快照后锁外 cancel——R3 评审修复） |
| `owner` | :377 断连 / :758 受理 | R / W | B / D | 🔒 均锁内 |
| `parent_ctx` | :759 受理 / :1003 apply 再入 | W / R | D | 🔒 均锁内（R1-P0） |
| `idem_key` | :712 幂等分支 / :766 受理 | R / W | D | ⚠️ 读锁外（撕裂窗口，见 U1 附带）；写锁内 |
| `ssid`/`password` | :760/:762 受理 | W | D | 🔒 锁内（写）；撕裂读见 U1 附带 |
| `operation_id` | :787 受理 / :797 响应 / :873 status | W / R / R | D / D / Q | 🔒 写锁内；:797 读锁外（单写者已消失，O2）；:873 锁内 |
| `op` | :378 断连 / :710 幂等 / :786 受理 | R / R / W | B / D / D | 🔒 断连与受理锁内；:710 锁外（静态槽位句柄不失效，O4） |
| `cfg_txn/cfg_txn_valid` | :754-755 受理 / :263 clear_job end | W / W(+end) | D / 持锁者 | 🔒 锁内 |
| `netcheck_gen` | :598 worker / :480 cleanup | W / R | W / C | 🔒 均锁内 |
| `st_*` ×4 | :611-614 worker / :876-879 status / :958 apply gate | W / R / R | W / Q / D | 🔒 写锁内、status 锁内；apply gate ⚠️ 锁外（生命周期论证，见 O1） |
| 整体 `memset` | :753 受理（锁内）/ :266 clear_job（调用方持锁） | W | D / W·C·B·T | 🔒 |
| 别名指针 `&s_net_async`（作为 run_fn/cleanup 的 ctx） | :773/:778/:786 | 传递 | D→W/C | 见 §3 生命周期（静态存储，指针恒有效） |
| `s_net_async_apply_internal` | :105 / :994/:1012 / :2614 | W/R/R | D | ⚠️ 锁外（暴露面=无，见 O5） |

## 2. 第二轮 · 竞态场景逐项裁决

| 场景 | 裁决 | 依据 |
| --- | --- | --- |
| 1. worker 写 vs cleanup 清零 | **安全** | 两者对 job 的全部读写都在 job 锁内（worker st_*/gen :586-614 锁内；cleanup nc_gen 捕获+clear_job :480-486 锁内）——串行化成立 |
| 2. dispatcher 读 vs BLE 断连 | **安全** | 断连在锁内快照 owner/op（:376-378，R3 评审修复），cancel 在锁外（避免自锁）；cancel→cleanup 与 dispatcher 的命令处理跨任务，但 job 全访问锁内串行 |
| 3. apply 再入 vs operation cleanup | **安全（有意持锁长段）** | `set_state(COMMITTING)` 是与 cancel/expire 的同步点；COMMITTING 后 BLE 断连回调直接 return（:923-927）、expire 跳过非活动 op；apply 在锁内完成内层 execute+clear_job（R2-P0 设计：cleanup 在 job 锁上等待） |
| 4. 状态查询 vs job 生命周期结束 | **安全** | operation_status 全程持锁（:813 lock → :903 unlock），have_snapshot/st_* 与 memset 串行；job 清零后 have_snapshot=false → 回落 live（+阶段快照缓存兜底） |
| 5. 旧 operation cleanup vs 新 operation | **安全（三机制协同）** | 单例 job + 受理 active 守卫（旧 op 非活动才放行）+ worker stale 守卫（generation+CONNECTING，不匹配即 no job touch）+ `clear_active_operation` 按 id 匹配。旧 worker 不可能在"新 job 写入后"触碰 job——它唯一能到 rollback cleanup 的前提是自身仍 CONNECTING 且槽位未复用，而那时新 op 尚未受理（active=true 未清） |
| 6. 双源并发受理（BLE+MQTT 同时 tryconnect） | **当前不可达** | `network.tryconnect` MQTT 源 guard_rejected（cmd.c:2517 区域）；HTTP 无通道；SERIAL 未接线；TOUCH 不产生网络命令；BLE 命令单任务串行 ⇒ U1 为**结构性缺口（潜在）**，非可触发竞态 |

## 3. 第三轮 · 逐字段所有权分类

| 字段 | 分类 | 依据与边界 |
| --- | --- | --- |
| `netcheck_gen` | **LOCKED** | 写（worker :598）读（cleanup :480）均锁内 |
| `st_*` ×4 | **LOCKED**（写）/ **OWNER+生命周期**（apply gate 读） | 写锁内；apply gate :958 锁外读靠三前提：①worker 在 got_ip 交付后已返回自删（st_* 写者消失）②active=true 挡新受理（memset 不会发生）③apply 仅 BLE 来源且 BLE 命令单任务串行（cancel/expire 不并发到达）。**脆弱面**：MQTT apply 放开即破（CMD-06 迁移时的硬约束） |
| 受理初始化段（active/owner/parent_ctx/ssid/password/idem_key/op/operation_id/cfg_txn） | **LOCKED** | :745→:793 单临界区（含 operation_start） |
| `active`（守卫读） | **SNAPSHOT-原子** | bool 对齐读原子；dispatcher/tryconnect 守卫锁外读 + 后续锁内复核（trial_busy、operation_start busy）或守卫兜底（worker stale） |
| `op`/`idem_key`（幂等分支读） | **UNSAFE-潜在**（U1 组成部分） | 锁外读与锁内写（受理）并发——当前不可达（场景 6），第二入口放开即激活 |
| `parent_ctx` | **LOCKED** | 写（受理 :759）读（apply :1003）均锁内，且以 COMMITTING 同步点隔离 |
| `cfg_txn` | **LOCKED**（受 U1 影响） | 写/end 锁内；双受理覆盖时第一个事务泄漏（不可达场景，随 U1 修复） |
| `s_net_async_apply_internal` | **OWNER（D 单任务）** | 写/读均在 BLE apply 的 dispatcher 任务内（内层再入同任务）；MQTT apply 被 guard 挡；暴露面=无 |
| 别名指针 `&s_net_async`（ctx） | **SNAPSHOT-生命周期** | 静态存储指针恒有效；数据生命周期由 stale 守卫 + active 守卫 + 幂等 cleanup 保证（场景 5） |

## 4. 第四轮 · 发现与修复方案

### 4.1 确定安全（保留现状，附同步依据）

§2 场景 1-5 全部；§3 全部 LOCKED 项；`clear_job` 双版本纪律；锁序
`job_lock → s_ops_mux(spinlock)` 单向无反转。**本轮未发现可触发的数据竞争。**

### 4.2 UNSAFE（潜在，结构性缺口 U1）

**内容**：受理守卫（:709 active 读、:710-712 幂等分支读 op/idem_key）在 job 锁外，
与锁内受理初始化非原子。**触发条件**：未来任何第二个 tryconnect 入口
（MQTT 放开 / SERIAL 接线 / HTTP 回归）。**后果**：双受理 → 第二个 memset 覆盖
第一个 job → 第一个 worker 读 active=false 返回 → 双 op FAILED + **第一个
`cfg_txn` 泄漏**（begin 未 end，占配置事务槽）。
**修复（方案 A，防御式 ~10 行）**：把 :709-:724 的守卫（active/幂等分支）移入
`app_cmd_net_job_lock()` 临界区，与 memset/op_start 同锁——幂等分支内的
`operation_get_status`（s_ops_mux spinlock 短临界区）锁内调用安全（锁序与
operation_start 一致）。**当前不要求立即修**（不可达），登记为"第二入口放开的
前置条件"。

### 4.3 需要补充证据：无

源码 + 调用链已闭合（45 处引用全部定位到函数/任务/锁状态）；无需新增日志或
测试——唯一值得加的是**双源受理并发回归**（待 MQTT tryconnect 放开时随入口
同批补测，当前无触发路径）。

### 4.4 方案 A/B 结论

**采纳方案 A**：维持现有递归锁机制，仅登记 U1 为前置条件；**不引入快照 API（方案 B）**。
理由：本轮审查显示除"不可达的受理缺口"外，全部访问点要么 LOCKED、要么有清晰的
生命周期论证；方案 B 的抽象成本（parent_ctx/op/cfg_txn 的生命周期进快照的语义
设计）当前无对应收益。**重评时机**：CMD-04（status 快照改造）与 CMD-06（网络事务
迁移 `cmd_net_txn.c`）落地时——迁移会改变 job 的任务上下文分布，届时按新版图
重评 B。

## 5. 验收对照（评审清单）

| 验收项 | 结果 |
| --- | --- |
| 字段清单 | 45 处引用 + memset/sizeof/别名指针/ctx 传递全覆盖（§1） |
| 锁保护 | 每访问点标注锁状态（§1 表），递归锁语义确认（R8） |
| 任务上下文 | D/W/C/B/T/Q 全部归属（§1 代号） |
| 生命周期 | 场景 5 三机制协同闭环（单例 job + active 守卫 + stale 守卫 + 按 id 清除） |
| generation | 槽位复用 generation+1 ⇒ 旧 worker 守卫必不等，无误操作路径 |
| operation | 句柄静态槽位不失效；stale/COMMITTING/cancel 守卫逐点核验 |
| 修复边界 | U1 一项（防御式，登记为第二入口前置条件），其余零修改 |
| 回归方案 | 当前拓扑无可触发路径；MQTT tryconnect 放开时随入口补双源并发回归 |

## 6. 给 CMD-06/CMD-03 的两条硬约束（迁移时不可丢失）

1. **gate v3 锁外读的三前提**（worker 已退 / active 挡新受理 / apply 仅 BLE 串行）
   ——CMD-06 把事务迁 `cmd_net_txn.c` 时若放开 MQTT apply 或改多任务分发，
   `st_internet_verified` 的读必须入锁或改快照 API；
2. **`job->active` 锁外读依赖 operation 框架 stale 守卫**（非活动 op 不覆盖
   run_fn 结果）——CMD-03 审查 operation.c 时必须确认该守卫保持。

## 7. 与既有文档的一致性

- cmd.c:82-84 的 R2-P0 注释（"受理写入、apply 再入读取、worker/cleanup 的终态
  memset 全部由 s_net_job_lock 串行"）与实测一致——**但"受理写入"的守卫读
  （:709）在锁外**，该注释表述的是写入侧；建议后续在注释补一句"受理守卫读在
  锁外，单入口前提下安全"（代码注释变更，随 U1 修复或 CMD-06 一并）。
