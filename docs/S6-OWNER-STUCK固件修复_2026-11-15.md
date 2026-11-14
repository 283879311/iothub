# S6-OWNER-STUCK · 固件修复（2026-11-15）

> **状态**：ACTIVE（**已改码并构建，未烧录**；烧录与复测均需单独授权）
> 立项依据：[第五轮执行记录与 owner 粘滞缺陷](S6第五轮执行记录与owner粘滞缺陷_2026-11-15.md)（`6a2046c`）

## 1. 缺陷（dc254d5 实测 + 源码印证）

`app_wifi_tryconnect_release()` 直接调用 `app_wifi_owner_step_locked(RELEASE_TRYX)`，
绕过 `app_wifi_owner_step_ev()` 里的 handover 记账：

| 字段 | 应有值 | 实际值 |
| --- | --- | --- |
| owner | CLEANUP（状态机已迁移） | CLEANUP ✓ |
| `s_wifi_restart_kind` | `APP_WIFI_RESTART_CLEANUP` | **APPLY**（默认值） |
| `s_wifi_cleanup_started_ms` | 当前时刻（看门狗依赖） | **0** |

后果：restart 任务按 APPLY 分支运行（不产生 cleanup 终态）；monitor 看门狗条件
`owner==CLEANUP && started>0` 永不成立 → **owner 永久粘滞 CLEANUP**，
之后所有 STA 操作（tryconnect/connect/apply）全部 `owner wait_timeout`。
现场实测：自第一轮 `8122661ms` 起 3.5 小时零转移、`cleanup complete/failed/watchdog`
各 0 次，三轮 `connect_failed` 同因。

## 2. 补丁（`main/wifi.c`，5 处；纯状态机 `wifi_op_owner.c` 零改动）

| # | 位置 | 改动 |
| --- | --- | --- |
| A | 新增 `app_wifi_cleanup_handover_account_locked()` | handover 记账（kind=CLEANUP、started_ms）与终态清理（离开 CLEANUP 后复位 kind/started）**收进锁内统一出口** |
| B | `app_wifi_owner_step_locked()` | 状态迁移后调用 A —— **所有调用者**（step_ev / tryconnect_release / abort_consume）自动获得记账 |
| C | `app_wifi_owner_step_ev()` | 删除原重复记账（单一来源） |
| D | 新增 `app_wifi_cleanup_wake_failed()`；**三处**调用（两个 release + `cleanup_request` 的 NOTIFY 路径） | 唤醒失败 → **`CLEANUP_FAILED` 明确终态**（带 op_id、owner 锁内快照）。**不**回退 NONE——避免"清理未跑、owner 显示空闲"的假恢复；CLEANUP_FAILED 为既有粘性语义（config.set+reboot 恢复） |
| D2 | `app_wifi_owner_cleanup_request()` NOTIFY 路径 | **删除旧的 `CLEANUP_DONE`→NONE 回退**（P1：那条路径等于"清理没跑却显示空闲"），改与 HANDOVER 同口径 → `CLEANUP_FAILED` 并返回 false |
| F | `abort_consume()` | 删除重复的 started/kind 清理（统一出口已负责，单一来源） |
| G | `app_wifi_test_hook()` + `app_wifi_restart_wake()` | 新增**一次性注入** `restart_wake_fail`（仅 `APP_WIFI_FAULT_INJECT` 构建）：`restart_wake()` 首部消耗计数并返回 false，供 R4 复现 |
| E | `app_wifi_restart_task_fn()` | **兜底**：若 `owner==CLEANUP` 而 kind≠CLEANUP，强制改为 CLEANUP 并补 started_ms，打 `restart: kind corrected to CLEANUP` 警告——防止未来任何记账缺口再次粘滞 |

## 3. 构建与资源

| 项 | 值 |
| --- | --- |
| build | 0 error / 0 warning |
| **功能镜像 build_sha** | **`acde8863e`**（ELF，`APP_WIFI_FAULT_INJECT=OFF`，0 error / 0 warning） |
| **FI 镜像 build_sha** | **`3dcf4f7db84e`**（ELF，`-DAPP_WIFI_FAULT_INJECT=ON`，R4 专用；构建期有 4 条 **uart.c 既有** unused-function 警告，与本次改动无关——功能镜像 0 警告） |
| 栈帧（`-fstack-usage`） | `owner_step_locked` 48B、`owner_step_ev` 80B、`handover_wake_failed` 96B、`restart_task_fn` 288B（任务栈 4096B，余量充足） |
| 主机测试 | `tests/wifi_owner` **59 passed / 0 failed**（纯状态机未改） |

## 4. 修复后回归矩阵（**设备侧执行**，需烧录后另行授权）

| # | 场景 | 通过判据 |
| --- | --- | --- |
| R1 | TRYX 持有期间收到 cleanup 请求，随后正常释放 | 日志 `TRYX -> CLEANUP`；kind=CLEANUP、started>0；随后 `cleanup complete`；owner → NONE |
| R2 | **同上但 BLE 断连触发取消**（本次现场故障复现路径） | 同上；**不得**出现 owner 停留 CLEANUP 且无 cleanup 终态 |
| R3 | restart 任务正常执行 | `cleanup complete` 或 `cleanup failed` 至少一条；owner 不停在 CLEANUP |
| R4a | **FI 镜像**：TRYX 持有期间请求 cleanup，释放时唤醒失败<br>步骤：BLE `diag.m1hook {action:"restart_wake_fail", count:1}` → 发起 tryconnect → 中途 `network.cancel`（使 release 时 `cleanup_requested=true`） | 日志 `hook: restart_wake forced fail` + `cleanup wake failed -> CLEANUP_FAILED op=… by tryconnect_release`；owner=CLEANUP_FAILED（可诊断，**非**假 NONE） |
| R4b | **FI 镜像**：owner=NONE 时直接请求 cleanup 且唤醒失败<br>步骤：BLE `diag.m1hook {action:"restart_wake_fail", count:1}` → 触发一次 cleanup 请求（cancel 一个已 ready 的 trial 或由 op 终态 cleanup 发起） | 日志 `cleanup wake failed -> CLEANUP_FAILED op=… by cleanup_request`；`app_wifi_cleanup_request` 返回 false；owner=CLEANUP_FAILED，**不得**回 NONE |
| R5 | 无 cleanup 请求时 TRYX 正常释放 | owner → NONE；**不**误启动 cleanup（无 cleanup 日志） |
| R6 | monitor 看门狗超时 | 12s 重通知 / 25s abort（`cleanup aborted by watchdog`）可观测——不再因 started=0 失效 |

> R2 为直接复现路径，必须是**生产侧集成测试**（真机 + BLE 断连），不能只靠纯状态机单测替代。

## 5. 未纳入本补丁（单独评审）

`network.status` 增加只读 `wifi_owner` 字段：属可观测性增强，不改变正确性；
按评审意见与核心修复分离，另行提审。

## 6. 下一步（各自独立授权）

1. 本补丁代码复审；
2. **一次性烧录授权**（新 `build_sha=249b3a72c`；第五轮授权不覆盖烧录）；
3. 烧录后重建 uptime / 网络 / owner 基线，执行 §4 回归；
4. 回归通过后，再申请 expired + op-V 的复测窗口。

## 7. 复审 P1/P2 处理（第二轮）

| 编号 | 审查意见 | 处理 |
| --- | --- | --- |
| **P1** | `cleanup_request` 的 NOTIFY 唤醒失败仍走 `CLEANUP_DONE`→NONE（清理未执行却释放 owner） | 删除该回退，统一调用 `app_wifi_cleanup_wake_failed("cleanup_request")` → `CLEANUP_FAILED`；函数返回 false，调用方按失败处理 |
| **P1** | R4 缺可重复注入手段 | 新增一次性 `restart_wake_fail` 注入（仅 FI 构建），R4a/R4b 两条路径均给出具体执行步骤 |
| P2 | `else` 分支锁外读 `s_wifi_owner.owner` | 改为**锁内快照** `owner_snap` 后再打日志 |
| P2 | `abort_consume` 重复清理 started/kind | 删除重复赋值，统一由 `step_locked` 出口处理 |
