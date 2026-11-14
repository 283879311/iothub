# WIFI-OP-RESOURCE-RACE · fencing 差量实现记录（2026-11-13）
> **状态**：ACTIVE（fencing 差量待复审）（文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)）


> 评审方式：以 `artifacts/wifi_op_race_20261112/fencing_delta.patch` 的实际 diff 为准。
> 代码 commit：`0eb30ce47795cff5201388793f926cc4302b5dd8`（基线 `185e029`，单 commit 可 revert）。
> 伴随提交：`gitignore`（隔离测试二进制）并入同一 commit；本记录为独立 docs 提交。

## 1. 硬性审查条件逐条对照

| 评审条件 | 实现点 |
| --- | --- |
| watchdog >25s 只置 abort，不得写 FAILED/释放/转移 | `app_wifi_owner_monitor_tick`：>25s 分支删除 CLEANUP_FAIL 投递；同临界区（owner==CLEANUP 校验 + 置位）只置 `s_wifi_cleanup_abort`，日志 `abort requested (owner stays CLEANUP)` |
| restart 任务入口：abort 置位不得开始驱动操作 | `app_wifi_restart_run_cleanup` 入口安全点：`cleanup_abort_consume("cleanup_entry")` → 命中即 FAILED + return（apply_cfg 一次都不跑） |
| 失败退避后：重试前检查 abort | 循环内 `vTaskDelay(backoff)` 之后、`apply_cfg` 之前：`cleanup_abort_consume("cleanup_backoff")` |
| apply 成功：清 abort 与释放 owner 正确同步 | 成功路径单一临界区：`abort=false` + `CLEANUP_DONE`（owner→NONE）+ kind 复位一并完成；日志如实标注 `abort discarded: apply succeeded` |
| apply 失败：检查与 FAILED 转换间无抢占窗口 | `cleanup_abort_consume` = **同一临界区内**检查 abort + 投递 CLEANUP_FAIL；其他主体（TRYX/AUTO/APPLY claim）在 owner=CLEANUP 下本就被状态机拒绝 |
| active op 防误清 | `clear_active_operation` **按 id 匹配**才清；调用点：cleanup（cancel/expire）、worker 尾部 FAILED/CANCELLED、apply 成功提交；run 入口重新登记覆盖 |
| B5 task run 用调度时 gen | 任务参数结构 `app_wifi_auto_task_arg_t`（caller/op_id/gen/scheduled_ms）在 xTaskCreate 前填充；task_fn 只读参数，不重读 link_gen |
| BOOT_RESET | 生产固件 grep 证实无调用点 → **删除死日志分支**；事件保留（主机测试注入粘性清除语义 + 未来显式恢复通道），注释写明"运行期接线需单独评审" |

## 2. 关键竞态闭合证明（供交叉核对）

- **watchdog vs restart**：monitor 置 abort 与 restart 消费 abort 都在 `s_tryconnect_mux`
  内；restart 消费时校验 `owner==CLEANUP` 并**同锁**完成 FAILED 转换——不存在
  "monitor 改 owner 时任务仍在驱动内"（monitor 不改 owner）或"任务在飞时 owner 被
  改"（monitor 不改 owner）的双向窗口。
- **成功 vs abort**：成功路径的 `abort=false + CLEANUP_DONE` 单临界区——若 monitor
  在 apply 执行期间置了 abort，成功完成时一并丢弃并记日志；monitor 之后看到
  owner=NONE 不会再置 abort（其置位前提 owner==CLEANUP 不成立）。
- **abort 与新 op**：CLEANUP 期间 TRYX claim 被状态机拒绝（WAIT）；FAILED 期间
  DENY_FAILED 立即拒绝——新 op 不可能绕过。

## 3. 自测与构建（本 commit 树，2026-11-13）

- `idf.py -B build build`：**0 error / 0 warning**（ELF sha 前 9 位 `96df31696`）；
- 语法检查 **35/35**；
- 主机 8 套全绿：cmd_policy **50**（+test-build PASS）/ cmd_error PASS /
  cmd_cfg_txn **22** / cmd_context **85** / netcheck_policy PASS / token_store **58** /
  **wifi_owner 59**（新增 fencing 不变量 +19）；
- `git diff --check` 通过；提交内无构建产物（测试二进制经 .gitignore 隔离，
  曾误入暂存区，已 amend 移除——两次提交历史如实保留）。

## 4. 规模

| 文件 | 变更 |
| --- | --- |
| main/wifi.c | +170/−29（fencing + B5 + BOOT_RESET 说明） |
| main/cmd.c | +6（4 个集成点） |
| main/wifi.h | +4（两个 API） |
| tests/wifi_owner/test_wifi_owner.c | +53（+19 例） |
| .gitignore | +1 |
| **合计** | **+234/−29** |
