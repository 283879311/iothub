# WIFI-OP-RESOURCE-RACE · 四路径调用关系图（2026-11-12）

> 配套：[修订设计 B1–B5](WIFI-OP-RESOURCE-RACE-修订设计_B1-B5_2026-11-12.md)。
> 标注约定：`(n)` = B4 覆盖表第 n 个 STA 驱动调用点；`★` = owner 取得/释放/交接；
> `▣` = 异步任务真正退出的位置。行号基于固件 `1.3-2ef2a38`。

## 1. owner 状态机（总图）

```
                    claim_owner(TRYX,12000ms)
        ┌───────────────────────────────────────► TRYX ◄── P1
        │                                            │ release_or_handover
        │        claim_owner(AUTO,0)                 │
        ├───────────────────────────────────────► AUTO ◄── P3
        │                                            │ release_or_handover
        │        claim_owner(APPLY,15s)              │
        ├───────────────────────────────────────► APPLY ◄─ P4
        │                                            │ release_or_handover
   ┌────┴────┐                                       │
   │  NONE   │◄──────────────────────────────────────┘
   └────┬────┘
        │ cleanup_request()（owner==NONE 时直接取得）
        │ 或 任一持有者 release 时发现 s_cleanup_requested → 直接 handover
        ▼
   ┌─────────┐   apply_cfg×3 全失败        ┌──────────────────┐
   │ CLEANUP │────────────────────────────►│ CLEANUP_FAILED   │
   │  (P2)   │◄──── 重试（0.5/1/2s）        │  （粘性，禁止一切 │
   └────┬────┘                              │   STA 发起）      │
        │ apply 成功                         └────────┬─────────┘
        ▼                                            │ 仅 config.set+reboot
      NONE                                           ▼
                                                  NONE（重启后）
```

**关键不变量**：`TRYX/AUTO/APPLY → NONE` 的路径被"释放即交接"取代——释放点在同一
把锁内检查 `s_cleanup_requested`，为真则直接转 `CLEANUP`，**不产生可被抢占的 NONE 窗口**。

---

## 2. P1 · OP_ACTIVE（tryconnect）

```
[cmd worker 任务]（operation run_fn 动态创建）
  app_cmd_net_async_run()
    └─ app_wifi_try_connect_ip()
        └─ app_wifi_try_connect_common()  :3520
             ★ claim_owner(TRYX, 12000ms)           owner: NONE → TRYX
               │  （日志带 op=<active_op> gen=<link_gen>，B5 第三轮字段）
               │  - owner==CLEANUP     → 轮询等待（restart 任务在恢复）
               │  - owner==CLEANUP_FAILED → 立即 failed/cleanup_failed
               │  - 超时 → failed/busy
             app_wifi_auto_connect_suspend()         （仅挡新调度）
             wait_connect_task_idle(15000)           （只等任务指针；本设计保留）
             ┌─ 预扫描 app_wifi_perform_scan() → perform_scan_impl (7) :3426
             │    fail ×2（500ms 间隔）→ wifi_scan_failed
             ├─ esp_wifi_disconnect() (9) :3661
             ├─ esp_wifi_set_config(candidate) (9) :3679
             ├─ esp_wifi_connect() (9) :3688
             └─ defer 窗口（选 B）→ 等判决/取消
             app_wifi_tryconnect_finish()  :3481
               ├─ esp_wifi_disconnect() (8) :3498
               ├─ esp_wifi_set_config(old_cfg) (8) :3501
               ★ release_or_handover(TRYX)  :287
                    ├─ s_cleanup_requested ? → CLEANUP（交接，通知 restart）
                    └─ 否则 → NONE
             ▣ run_fn 返回 → worker 任务自删
```

要点：
- `(7)(8)(9)` 全部在 TRYX 持有期内；
- **释放发生在 disconnect/restore set_config 完成之后**（评审释放原则），
  candidate 连接已在 finish 内被 disconnect 收口；
- 旧 op 的 finish 会 `schedule_connect_task(400ms, "tryconnect_finish")`（B5 标签）——
  这是 Q1.4 的唯一候选调度源（**定案待 B5 日志闭环**，见修订设计 §0/§13）；
  在新协议下，该任务若在 CLEANUP/OP_ACTIVE 期间醒来会 `schedule refused` 或
  claim 被拒，不会驱动 STA。

**释放后零 STA 证明（第三轮评审问题 3，分段闭合版——覆盖释放点之后的全部栈展开）**：

```
release_or_handover(TRYX) 返回
  ↓ 段 1｜tryconnect_finish 剩余语句（wifi.c:3510-3512）
      auto_connect_resume("tryconnect_finish")     —— 置 suspended=false + 日志，非驱动
      schedule_connect_task(400ms, ...)            —— 仅创建任务，非驱动调用
      return final_result
  ↓ 段 2｜try_connect_common 剩余语句
      所有 finish 调用点均为 `return app_wifi_tryconnect_finish(...)` 尾语句
      ⇒ 剩余语句为空（wifi.c:4019/4024/4028/4039/4048/4077/4090/4118 逐一核实）
  ↓ 段 3｜try_connect_ip 剩余语句
      `return app_wifi_try_connect_common(...)` 尾语句 ⇒ 空
  ↓ 段 4｜app_cmd_net_async_run 剩余语句（cmd.c）
      stale 分支 return / OK 分支（读快照 + set_state）/
      回滚分支（cleanup + set_state）/ 尾部 clear_job + 终态
      ⇒ 无状态变更类驱动调用
```

- `esp_wifi_*` 状态变更类调用（disconnect/connect/set_config/scan_start/stop）
  在 cmd.c 全文件 **0 处**（唯一驱动交互 = 只读查询 `esp_wifi_sta_get_ap_info`，
  无副作用，不需要所有权）；
- 段 1 的 `schedule_connect_task` 创建的任务在新协议下受仲裁：CLEANUP/OP_ACTIVE
  期间醒来会 `schedule refused` 或 claim 被拒——旧 worker 释放后，"恢复动作"
  在 CLEANUP 所有权下由 restart 任务执行；
- 不变量兜底：release 后旧 worker 不持 TRYX，wifi.c 内任何状态变更类 STA 操作
  都需要 owner。
（`app_wifi_try_cancel()` 只置取消标志；`app_netcheck_cancel` 只取消验证任务。）

## 3. P2 · CANCEL_CLEANUP（cancel/expire 恢复链）

```
[expire 任务 或 cmd 任务] app_operation_cancel() :457 / expire_one() :619
  └─ cleanup_fn = app_cmd_net_async_cleanup()  cmd.c:460
       ├─ 清 trial/job、app_netcheck_cancel(gen)
       ├─ state==CONNECTING → app_wifi_try_cancel()   （让 P1 尽快走到 finish）
       ★ app_wifi_cleanup_request()
            ├─ owner==NONE            → CLEANUP + notify
            ├─ owner==TRYX/AUTO/APPLY → s_cleanup_requested=true（等释放移交）
            └─ owner==CLEANUP_FAILED  → false → operation failed/cleanup_failed
       └─ return true → operation 终态（CANCELLED/EXPIRED，terminal_ms 记录）
          （终态 ≠ 资源释放：owner 此时可能已是 CLEANUP，也可能仍在等移交）

[wifi_restart 任务] :3780（单实例，长期存活）
  xTaskNotifyWait → vTaskDelay(250ms)
  ★ wait_owner(CLEANUP)               （由 request 或 P1/P3/P4 的 release 交接而来）
  看门狗 fencing（第三轮）：
    abort 只由 monitor 置位（CLEANUP >25s），**不改变 owner**；
    owner→CLEANUP_FAILED 的转换只发生在本任务的"安全点"
    （= apply_cfg() 已返回、无在飞驱动调用的边界：入口处 / 每次失败退避后）
    ——每处检查 s_wifi_cleanup_abort
  循环最多 3 次：
    [安全点] abort? → ★ CLEANUP_FAIL（自转换）→ owner=CLEANUP_FAILED
                      日志 cleanup aborted by watchdog op=<id> → 结束
    apply_cfg() (6) :2999   ← 全程持 CLEANUP
        esp_wifi_stop() → set_mode() → esp_wifi_set_config() (6) → esp_wifi_start()
        → STA_START 事件 → handler 释放 hold/开窗口 + schedule("sta_start")
        → A8 await 完成 → 返回
    成功 → ★ owner=NONE；日志 cleanup complete op=<id> attempts=<n>
            （abort 已置位也清除：资源已合法释放）
    失败 → 0.5s/1s/2s 后重试（不跑 LKG/DHCP 链；回到上方安全点检查）
  3 次全失败且无 abort → ★ owner=CLEANUP_FAILED
              日志 cleanup failed op=<id> stage=<stage> attempts=3
  ▣ 任务不退出（继续等下一次通知）；owner 已释放/转失败态
  安全论证：CLEANUP_FAILED 转换只出现在"apply_cfg 已返回"的边界；
  转换前 owner=CLEANUP，其他主体本被拒——不存在"旧任务仍能操作 STA 时
  owner 被改"的窗口；apply_cfg 卡死（理论）时 owner 保持 CLEANUP，无 unsafe 交接。
```

要点：
- `apply_cfg` 的 `stop/start` 会触发 `STA_START`→`schedule_connect_task("sta_start")`；
  该任务在 CLEANUP 期间 claim AUTO 被拒 → 置 pending 退出，**cleanup complete 后
  由 monitor 1s 内重试**（RACE-03：CLEANUP 期间零 STA 发起操作）；
- 无 "cleanup 释放 → apply 再 claim" 的窗口（B1 验收点 1）：apply 不 claim APPLY，
  外层的 CLEANUP 一直持有到 apply 返回。

## 4. P3 · AUTO_CONNECT（后台自动连接）

```
[wifi_auto_connect 任务]（schedule_connect_task 创建，一次性）
  vTaskDelay(delay)                     ← 调度时刻已打印 caller（B5）
  复核：owner==NONE && !suspended && !offline
  ★ claim_owner(AUTO, 0)  （非阻塞）
      deny → ESP_LOGW("AUTO claim denied owner=%s by=%s")
             s_wifi_fast_path_pending=true；▣ 任务退出（monitor 1s 重试）
  app_wifi_connect_best_saved_profile() :975   ← AUTO 唯一入口/唯一释放点
      ├─ fast_path 分支：
      │    esp_wifi_disconnect() (2) :1035
      │    esp_wifi_set_config() (2) :1036
      │    esp_wifi_connect()  (2) :1050
      └─ profile 分支：
           app_wifi_perform_scan() (7) → perform_scan_impl :3426
           app_wifi_connect_profile_index() :901
             esp_wifi_disconnect() (1) :939
             esp_wifi_set_config() (1) :942
             esp_wifi_connect()  (1) :960
  ★ release_or_handover(AUTO)（所有 return 统一）
      ├─ s_cleanup_requested → CLEANUP（交接）
      └─ 否则 → NONE
  ▣ 任务自删

事件旁路（wifi 事件任务 / netcheck 任务，均非阻塞 claim AUTO）：
  · reason=210 → app_wifi_retry_with_pmf_required() (3) :1520/1526
  · STA_DISCONNECTED 直连重试 → esp_wifi_connect() (4) :2157
  · netcheck fail_internet → esp_wifi_disconnect() (5) :2352
      被拒：让位 + 日志，退避/monitor 接续（不削弱恢复）
```

要点：
- 事件任务**永不退出**，因此事件路径的 claim 必须非阻塞（否则会卡死事件链）；
- `(1)(2)(7)` 在 AUTO 持有期内；`(3)(4)(5)` 各自短持有 AUTO（仅覆盖一次驱动调用）。

## 5. P4 · BOOT / 显式 APPLY

```
[boot 任务] app_wifi_boot_apply_or_recover()
  ★ claim_owner(APPLY, 15000)
  apply_cfg() (6) :3378
      stop → mode → set_config → start → STA_START → A8 await
  ★ release_or_handover(APPLY) → NONE（或 CLEANUP，若此刻有 cleanup 请求——运行期不存在）
  ▣ boot 流程继续

运行期：当前无显式 apply 命令；config.set 走 effect=reboot（重启后重新 boot）。
CLEANUP 的 apply 不在此路径（见 P2）。
```

---

## 6. 9 个驱动点 × 路径归属总表

| # | 驱动点 | 所属路径 | owner 持有区间 | 异步退出/交接 |
| --- | --- | --- | --- | --- |
| 1 | `connect_profile_index` :939/942/960 | P3 | AUTO（入口 #2 claim） | 随 #2 的 release_or_handover |
| 2 | `connect_best_saved_profile` :1035/1036/1050 | P3 | AUTO（claim + release 同点） | 函数返回即释放/交接 |
| 3 | `retry_with_pmf_required` :1520/1526 | P3 旁路 | AUTO（单次驱动，短持有） | 调用后立即释放 |
| 4 | `event_handler retry_direct` :2157 | P3 旁路 | AUTO（单次驱动，短持有） | 同上 |
| 5 | `netcheck_on_result` :2352 | P3 旁路 | AUTO（仅 disconnect，短持有） | 同上 |
| 6 | `apply_cfg` :2999 | P2（cleanup）/ P4（boot） | CLEANUP 全程 / APPLY 全程 | P2：任务不退出但 owner 释放；P4：boot 流程 |
| 7 | `perform_scan_impl` :3426 | P1（预扫描）/ P3（profile 扫描）/ 命令扫描 | 随调用者（TRYX/AUTO/AUTO） | 随调用者释放 |
| 8 | `tryconnect_finish` :3498/3501 | P1 | TRYX | release_or_handover（finish 内） |
| 9 | `try_connect_common` :3661/3679/3688 | P1 | TRYX | 经 #8 统一释放 |

## 7. 取消场景的交接时序（RACE-02 关键路径）

```
t0  [旧 op]  P1 持 TRYX，候选在飞
t1  [cancel] cleanup_request(): owner==TRYX → s_cleanup_requested=true
t2  [旧 op]  try_cancel → try_connect_common 提前走 finish
t3  [旧 op]  finish: disconnect + restore set_config 完成
t4  [旧 op]  release_or_handover(TRYX): requested → owner=CLEANUP + notify
             ▣ worker 返回自删（owner 已不由它持有）
t5  [新 op]  claim_owner(TRYX,12000ms): owner==CLEANUP → 等待（日志 waiting owner=CLEANUP）
t6  [restart] wait_owner(CLEANUP) 成功 → apply_cfg(6)（全程 CLEANUP）
t7  [restart] 成功 → owner=NONE，日志 cleanup complete
t8  [新 op]  claim 成功 → 预扫描(7) 真正启动并完成 → 终态 ap_not_found
```
无 `NONE` 可抢占窗口；无"cleanup 释放后 apply 再 claim"；新 op 只等一个 owner。

## 8. 与评审三验收点的对应

| 评审要求 | 图中证据位置 |
| --- | --- |
| B1：apply_cfg 全程不释放外层所有权 | §3：`apply_cfg(6)` 在 `wait_owner(CLEANUP)` 之后、`owner=NONE` 之前，中间无 release；§7 t6-t7 |
| B4：9 点逐点有 owner | §6 表（每点一行）+ §2/§3/§4/§5 各路径图中 `(n)` 标注 |
| B5：日志在调度时刻 + 可关联 | §4 顶部 `vTaskDelay(delay)` 前已打印 caller（调度瞬间）；§3 `sta_start` 同样带标签；scheduled/task run 两行均带 op/gen（第三轮字段），caller+op+gen 配对关联 |
