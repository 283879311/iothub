# WIFI-OP-RESOURCE-RACE · fencing `0eb30ce` 最终复审材料（2026-11-14）

> **状态**：ACTIVE（阶段 B 提交物；签收后同步 docs/README.md 索引）
> 差量复核依据：以代码提交 `0eb30ce47795cff5201388793f926cc4302b5dd8` 相对父提交 `ebba25c` 的 Git diff 为准；申请中提及的 `artifacts/cmd04_flash_20261114/fencing_review.patch` 当前不在仓库跟踪树内，不能作为可点击证据链接。
> （417 行，`git diff ebba25c 0eb30ce` 原样导出）。本文给出基线澄清、逐 hunk
> ↔ 复审点映射、主机测试实测对应。**签收人请以 patch 文件为准逐行核对。**

## 1. 差量范围与基线澄清

| 项 | 值 |
| --- | --- |
| 提交 | `0eb30ce`（"fix(wifi): C2-fencing——watchdog 只请求中止不夺权…"） |
| 实际父提交 | **`ebba25c`**（不是提交信息所写的 185e029） |
| 185e029→ebba25c | **仅 3 个 docs 提交**（设计文档第三/四轮修订，b23b5ed/5c13c78/ebba25c）——代码口径上"基线 185e029"成立 |
| 纯代码差量 | 5 文件 +205/−29：wifi.c +170/−xx、cmd.c +6、wifi.h +4、test_wifi_owner.c +53、.gitignore +1 |
| 旧工作版 patch | `artifacts/wifi_op_race_20261112/fencing_delta.patch`（458 行）为**提交前工作版，已过时**，与本包内容不一致——以本包 git diff 为准 |

## 2. 逐 hunk ↔ 复审点映射（锁 / generation / owner / cleanup / worker stale 守卫）

### 2.1 C2-fencing：watchdog 只请求中止、不夺权（patch 行 154-179，wifi.c monitor_tick）

- `>25s` 分支**删除** `step_ev(CLEANUP_FAIL)` 强制转换；改为同临界区检查
  `owner==CLEANUP && !abort → s_wifi_cleanup_abort=true`，**owner 保持 CLEANUP**；
  日志 `cleanup watchdog: abort requested (owner stays CLEANUP) op=%s elapsed=`。
- `>12s` nudge 分支不变。锁：`app_wifi_tryconnect_state_lock()` 临界区内完成
  检查+置位，`abort_requested` 仅控制日志。

### 2.2 C2-fencing：原子"检查 abort + FAILED 转换"（patch 行 103-123，新增 `app_wifi_owner_cleanup_abort_consume`）

- 同一临界区内：`abort && owner==CLEANUP` → `step_locked(CLEANUP_FAIL)` +
  捕获 op_id + 清 started_ms/kind；锁外打日志。**检查与转换间无抢占窗口**
  （评审条件原文对应）。

### 2.3 C2-fencing：restart 任务三个安全点（patch 行 277-331，`app_wifi_restart_run_cleanup`）

1. **入口**（patch 行 281-284）：在 `build_cfg` 与任何驱动操作**之前** consume
   abort → abort 已置位则直接 return（不开始任何驱动操作）；
2. **每次失败退避后**（patch 行 290-293）：attempt>0 的 backoff delay 之后
   consume（apply_cfg 已返回的边界）；
3. **成功路径**（patch 行 300-318）：清 abort 与 `step_locked(CLEANUP_DONE)`
   **同一临界区**（关闭"清 abort 后、释放前"被 monitor 再置位的窗口）；
   abort 已置位但 apply 成功 → 正常 complete 并如实标注
   `watchdog abort discarded: apply succeeded`；
4. **失败路径**（patch 行 323-328）：先 consume（abort→FAILED），consume 返回
   false 才走既有 `step_ev(CLEANUP_FAIL)`（避免双重转换）。

### 2.4 B5：任务参数结构携带调度时捕获值（patch 行 183-193 + 233-276）

- 新增 `app_wifi_auto_task_arg_t{delay_ticks, caller, op_id[24], gen,
  scheduled_ms}`，静态单实例 `s_wifi_auto_task_arg`；
- `schedule_connect_task`：调度瞬间打印 `auto_connect: scheduled caller=...
  op=%s gen=%u`（op 来自活动登记、gen 来自 `app_wifi_link_generation()`，
  **均为调度时刻值**）；
- `connect_task_fn`：**不重读** op/gen——task run 行
  `auto_connect: task run caller= op= gen= delay_actual=ms` 全部来自任务参数
  （patch 行 216-219 注释明确"执行时可能已变化，不重新读取"）；
- refused 日志带 `op=%s gen=%u`（patch 行 223-232）。
- **单实例竞态评估（自查）**：参数写入与任务读取间的覆写风险沿用既有单飞
  栅栏（`s_wifi_connect_task` 存活期间调度被拒——机制在 hunk 上下文，未改动）；
  本差量未放宽该栅栏。

### 2.5 B5：活动 operation 登记 + 按 id 匹配清除（worker stale 守卫）

- wifi.c：`app_wifi_set_active_operation` / `app_wifi_clear_active_operation`
  （patch 行 132-149）——**clear 按 id 匹配**（`strcmp` 相同才清），旧终态
  不得误清新 op（评审条件原文对应）；锁内读写。
- cmd.c 四个接线点（patch 行 10-47）：
  1. `net_async_cleanup`（终态清理）：`clear_active_operation(operation_id(op))`
     在 job_lock 之前（patch 行 18-19）；
  2. `net_async_run` 入口：`set_active_operation`（patch 行 27-28）；
  3. run 失败/取消分支：`stage_snap_put(全false)` 后
     `clear_active_operation`（patch 行 36）；
  4. apply 成功路径：`clear_active_operation(status.id)`（patch 行 44）。
- **自查发现（已知限制，log-only）**：run 入口 `set` 后若 `get_status` 失败
  早退（patch 行 29-31 上下文），该路径**不清 active 登记**——残留 id 会出现在
  后续 scheduled/task run 日志的 op 字段，直至下一 op set 覆盖。仅影响日志
  字段，不影响 owner/cleanup/generation 任何判定。是否需要补一个早退清理
  行，请评审裁决（+2 行）。

### 2.6 杂项

- `owner_step_ev` 删除 BOOT_RESET 死日志分支；事件保留（生产无调用点，grep
  证实——注释 patch 行 93-95），用于主机注入与未来显式恢复通道，接线需单独
  评审；
- `.gitignore` 仅新增测试产物 `tests/wifi_owner/wifi_owner_test`；
- wifi.h 导出 set/clear 两个新 API（patch 行 340-343）。

## 3. 主机测试 ↔ 代码对应（实测 2026-11-14 19:47）

`make -C tests/wifi_owner run` → **`wifi_owner: 59 passed, 0 failed`**。

| 测试函数 | 检查数 | 覆盖 |
| --- | --- | --- |
| test_claim_tryx / cleanup_failed_sticky / waiter_priority / cleanup_handover / release_and_terminal | 40（既有） | 状态机纯逻辑：TRYX 互斥/单飞、CLEANUP_FAILED 粘性、C1 waiter 优先、B1 释放移交、无窗口 |
| **test_cleanup_fencing_invariants（新增）** | **9** | CLEANUP 态下 7 种事件全部不改 owner（watchdog 在状态机层面无夺权路径）+ DONE→NONE + FAIL→FAILED |
| **test_cleanup_failed_invariants（新增）** | **10** | CLEANUP_FAILED 态下 9 种事件全部拒绝/不变 + 仅 BOOT_RESET 清除 |

新增 19 检与 §2.1/§2.3 的 fencing 语义一一对应（此前汇报中"19 检"即指新增
不变量项；总检查数为 59，特此更正）。

## 4. 自查清单汇总

| # | 发现 | 评级 |
| --- | --- | --- |
| 1 | run 入口早退路径残留 active_op_id（log-only） | 提请裁决——补丁提案见 §5（补 2 行或接受现状） |

## 5. 自查项 1 裁决与修正记录（2026-11-14 终审）

**评审正式结论**（逐项）：C2 watchdog 不夺 CLEANUP owner / abort 检查与
FAILED 转换原子性 / restart cleanup 安全点 / 成功路径清 abort 与释放同锁 /
B5 调度捕获 caller-op-gen / active operation 按 ID 匹配清除 / BOOT_RESET 死
日志清理——**全部通过**；run 入口早退残留**裁决 (b) 采纳补丁**；主机 59/0
为申报方实测（评审未本地复跑），且其性质是状态机不变量验证，**不能代替真机
并发验收**（RACE-01/02/03 仍在烧录窗口执行）。

**修正已合入**（本提案原样采纳）+ 构建证据：

| 项 | 值 |
| --- | --- |
| 新镜像 build_sha | ELF `ae8a3abb5` / BIN `d25828d58`（0 error/0 warning） |
| `execute_method` | **912B（与 04c 锚点不变）** |
| `net_async_run` | 修正前 464B = 修正后 **464B（零栈增长）** |
| 其余逐函数首录 | `build_network_status` 496B / `net_operation_status` 320B / `net_async_cleanup` 48B |
| 主机测试（修后复跑） | **59 passed / 0 failed** |

**合并烧录镜像必须包含本修正**；旧 build_sha（abc097643）作废，烧录窗口
申请的目标镜像 SHA 将按新值更新（等 S6/资源项复核后一并重提）。

现状（patch 行 27-31，wifi.c 无涉）：

```c
app_wifi_set_active_operation(app_operation_id(op));
if (!app_operation_get_status(op, &st0)) {
    return ESP_ERR_INVALID_ARG;          /* ← 此路径不清登记，id 残留 */
}
```

补丁提案（+2 行，clear 与 set 对偶，按 id 匹配语义一致）：

```c
app_wifi_set_active_operation(app_operation_id(op));
if (!app_operation_get_status(op, &st0)) {
    app_wifi_clear_active_operation(app_operation_id(op));
    return ESP_ERR_INVALID_ARG;
}
```

影响面：仅该早退路径的日志 op 字段；无 owner/cleanup/generation 行为变化。
**评审可选**：(a) 接受现状（log-only，下一 op set 覆盖）；(b) 采纳提案
（+2 行，build_sha 变化需重新出镜像与 -fstack-usage 对照）。任一裁决均
不阻塞其余 hunks 的签收。
| 2 | `s_wifi_auto_task_arg` 单实例竞态沿用既有单飞栅栏，本差量未放宽 | 既有机制，无新增风险 |
| 3 | 提交信息基线（185e029）与实际父提交（ebba25c）不一致（中间仅 docs） | 已澄清，无代码影响 |
| 4 | 主机测试计数此前表述为"19 检"，实为新增 19/总数 59 | 已更正 |
