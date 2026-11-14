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
| D | 新增 `app_wifi_handover_wake_failed()`；两处 release 调用 | 唤醒失败 → **`CLEANUP_FAILED` 明确终态**（带 op_id 日志）。**不**回退 NONE——避免"清理未跑、owner 显示空闲"的假恢复；CLEANUP_FAILED 为既有粘性语义（config.set+reboot 恢复） |
| E | `app_wifi_restart_task_fn()` | **兜底**：若 `owner==CLEANUP` 而 kind≠CLEANUP，强制改为 CLEANUP 并补 started_ms，打 `restart: kind corrected to CLEANUP` 警告——防止未来任何记账缺口再次粘滞 |

## 3. 构建与资源

| 项 | 值 |
| --- | --- |
| build | 0 error / 0 warning |
| **新镜像 build_sha** | **`249b3a72c`**（ELF） / BIN `2c010c571d6d` |
| 栈帧（`-fstack-usage`） | `owner_step_locked` 48B、`owner_step_ev` 80B、`handover_wake_failed` 96B、`restart_task_fn` 288B（任务栈 4096B，余量充足） |
| 主机测试 | `tests/wifi_owner` **59 passed / 0 failed**（纯状态机未改） |

## 4. 修复后回归矩阵（**设备侧执行**，需烧录后另行授权）

| # | 场景 | 通过判据 |
| --- | --- | --- |
| R1 | TRYX 持有期间收到 cleanup 请求，随后正常释放 | 日志 `TRYX -> CLEANUP`；kind=CLEANUP、started>0；随后 `cleanup complete`；owner → NONE |
| R2 | **同上但 BLE 断连触发取消**（本次现场故障复现路径） | 同上；**不得**出现 owner 停留 CLEANUP 且无 cleanup 终态 |
| R3 | restart 任务正常执行 | `cleanup complete` 或 `cleanup failed` 至少一条；owner 不停在 CLEANUP |
| R4 | restart task 创建/唤醒失败（注入） | 日志 `restart wake failed -> CLEANUP_FAILED`；owner=CLEANUP_FAILED（可诊断，非假 NONE） |
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
