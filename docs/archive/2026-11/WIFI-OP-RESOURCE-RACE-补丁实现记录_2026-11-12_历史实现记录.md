> 归档说明（2026-11-16）：提交前实现记录；其镜像/测试计数不代表最终 fencing 差量。 当前结论与导航见 [WIFI owner fencing 主文档](../../WIFI-OP-RESOURCE-RACE_主文档.md)。保留原始设计、diff 和验证陈述供审计。

---

# WIFI-OP-RESOURCE-RACE · 补丁实现记录（提审用，2026-11-12）
> **状态**：ACTIVE（185e029 实现记录）（文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)）


> 状态：**代码已实现、编译零告警、主机自测全绿；未提交、未烧录、未动设备。**
> 评审方式：以 `artifacts/wifi_op_race_20261112/wifi_op_race.patch` 的实际 diff 为准
> （不以此记录代替 diff）。

## 1. 实际 diff 范围（相对申请预估 +175 行）

| 文件 | 变更 | 说明 |
| --- | --- | --- |
| `main/wifi_op_owner.h`（新） | +76 | 六值 owner + 事件/动作 + 纯迁移声明 |
| `main/wifi_op_owner.c`（新） | +193 | 纯状态机实现（含名字/一致性辅助） |
| `tests/wifi_owner/*`（新） | +188 | Makefile + 40 例（要求 ≥12，覆盖 C1/B1/B3/B5 相关迁移） |
| `main/wifi.c` | +550 / −63 | owner 原语、claim/release、AUTO 包装、事件旁路、扫描、restart 分流、看门狗、B5 日志 |
| `main/wifi.h` | +7 | 新结果码 `APP_WIFI_TRY_CLEANUP_FAILED` + 三个新 API |
| `main/cmd.c` | +22 / −6 | cleanup 走 cleanup_request；scan owner-busy → conflict |
| `main/CMakeLists.txt` | +1 | 加入 `wifi_op_owner.c` |
| **合计** | **≈ +1037 / −69** | 比申请预估多约 4 倍，主要来自纯逻辑模块+测试（+457）与注释 |

**超预估的如实说明**：申请里 +175 是"wifi.c/cmd.c 净增量"的口头估算；实际把
P1 要求的纯状态机与 40 例测试、B5 全量日志、C1–C3 接续全部算进来后为 ~1037 行。
若评审要求压缩：可把 owner 名字/动作名辅助与部分注释移出（约 −80），测试不能压
（P1 必交）。

## 2. 四项前置的落实

| 前置 | 落实 |
| --- | --- |
| **P1** 纯逻辑 + ≥12 例 | `wifi_op_owner.{h,c}`（照 operation_state 先例，无 ESP-IDF 依赖）；`tests/wifi_owner` **40 例**：waiter 拒绝 AUTO、requested 时 handover（TRYX/AUTO/APPLY 三路）、CLEANUP_FAILED 粘性、非法/陈旧 release 不改状态、boot reset 清除、重复 cleanup 幂等 |
| **P2** 日志限流 | `app_wifi_log_should_emit()`：同 caller+reason 键 2s 内只打一次；`owner denied` 与 `schedule refused` 共用；`resume by <caller>` 也带调用者 |
| **P3** scan owner-busy 区分 | `app_wifi_perform_scan_command_ex(&owner_busy)`；cmd.c 映射：owner 忙 → **`conflict`**（瞬时 1s，契约既有），**不占用 3s 扫描节流窗**；节流拒绝仍 `busy` |
| **P4** R2 进真机清单 | 已写入补丁申请 §7 窗口流程：TRYX 结束后立即断 BLE，判据 Wi-Fi 恢复 ≤12s+1s |

## 3. 关键实现点（对应 B1–B5/C1–C3）

- **B1**：`release_or_handover` 语义由纯状态机实现——释放时若 `cleanup_requested`
  直接 `→ CLEANUP`（同临界区），无 NONE 可抢占窗口；`apply_cfg` 全程在 CLEANUP 内，
  不再 claim APPLY；
- **B2**：唯一取得点 `app_wifi_owner_step_ev`；`cleanup_pending` 未引入；
- **B3**：`app_wifi_restart_run_cleanup` = apply_cfg×3（0.5/1/2s），不跑 LKG/DHCP；
  全失败 → `CLEANUP_FAILED`（粘性）；进入/离开各一条带 op_id 日志；
  `config.set+reboot` 后 BOOT_RESET 清除并打印；
- **B4**：9 点归属见设计 §6；事件三路（PMF/retry_direct/netcheck）均非阻塞 claim AUTO，
  被拒让位并留 pending；
- **B5**：`schedule_connect_task(delay, caller)` 8 个调用点全部带 caller，**调度瞬间**
  打印；task_fn 二次复核打印 refused；owner 迁移 + resume 均带 caller；
- **C1**：`s_tryx_waiter` 先置位再轮询；AUTO 取得（含事件旁路）与 schedule 入口都检查；
- **C2**：`cleanup_request` 在 wake 失败时回退 NONE 并返回 false；monitor 12s re-notify /
  25s 强制 FAILED；
- **C3**：PMF pending 由下一次 AUTO 以 fast_path+required=true 消费；netcheck teardown
  pending 由 monitor 每拍执行、gen 变化失效、>10s 告警。

## 4. 自测与构建（当前树）

- `python3 tests/syntax_check.py`：**35 文件 35 通过**；
- 主机套件（8 个二进制全绿）：
  cmd_policy **50**（+ test-build 变体 PASS）/ cmd_error PASS / cmd_cfg_txn **22** /
  cmd_context **85** / netcheck_policy PASS / token_store **58** / **wifi_owner 40**；
- `idf.py -B build build`：**0 error / 0 warning**；ELF sha 前 9 位 `3be4b8199`。

（完整输出以终端记录为准；本记录不替代实测。）

## 5. 未做 / 边界

- 未提交（工作树改动 + 未跟踪新文件）；
- 未烧录、未重启、未改配置；
- 竞态验收脚本（RACE-01/02/03 自动化）未编写，等单独授权。


## 6. 评审复核 M1–M3 处理（2026-11-12 第二轮）

| 编号 | 处理 |
| --- | --- |
| **M1（必改）** `s_wifi_pmf_force_required` 泄漏 | 改为**进入 `app_wifi_connect_best_saved_profile_impl` 即取用并清零**（局部 `pmf_force`），删除 set_config 成功分支里的清除。任何分支（begin 失败/set_config 失败/connect 失败/回退扫描）都不会残留到下一次无关 fast_path。 |
| **M2（二选一）** 扫描 owner 护栏 | **选择补上**：`app_wifi_perform_scan_impl` 入口快照 owner，非 TRYX/AUTO 打 `scan without owner: owner=%s (guardrail)`（只告警不改行为，避免误伤既有三路径）。申请 §1 的 +4 行已兑现。 |
| **M3（记录）** 日志锁外读 | **顺手修掉**：`app_wifi_owner_log_transition` 改为由调用方在锁内快照 `waiter/requested` 后传入；schedule 日志的 `owner=` 改用锁内快照值（不再硬编码 NONE）。 |

复核后复跑：build **0 error / 0 warning**（ELF `3be4b8199`）、语法 **35/35**、
8 个主机套件全绿（cmd_policy 50 / cmd_error PASS / cmd_cfg_txn 22 / cmd_context 85 /
netcheck_policy PASS / token_store 58 / **wifi_owner 40**）、`git diff --check` 通过。
