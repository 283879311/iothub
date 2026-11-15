> 归档说明（2026-11-16）：早期六问分析已被 B1-B5 修订设计取代。 当前结论与导航见 [WIFI owner fencing 主文档](../../WIFI-OP-RESOURCE-RACE_主文档.md)。保留原始设计、diff 和验证陈述供审计。

---

# WIFI-OP-RESOURCE-RACE · 六问回答与最小改动设计（固件侧，2026-11-12）
> **状态**：ARCHIVED（已被修订设计 B1-B5 取代）（文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)）


> 对应：[WIFI-OP-RESOURCE-RACE-确认清单.md]（客户端仓 docs/）。
> 本轮**只回答与设计，不修改任何固件代码**；代码修改与烧录需单独批准。
> 固件基线：`1.3-2ef2a38`（build_sha `98f2465cb`）。行号以该提交为准。

## 0. 一句话结论

`cancel` 的 cleanup 只**调度**了恢复（`app_wifi_schedule_restart()` 入队即返回），
而 `try_connect_common` 的预扫描前唯一等待是 `wait_connect_task_idle()`——
它只等 **auto_connect 任务指针**，不等 **STA 状态**；auto_connect 任务把
"set_config+connect" 发起完就清指针返回，STA 仍处 `connecting`。
于是新 op 的预扫描撞上驱动状态机拒绝（`ESP_ERR_WIFI_STATE`），
而旧 op 的恢复配置因为要等 `OP_APPLY`（被新 tryconnect 持有）反而落在新 op
终态之后。两处同源：**"任务/终态结束"被当成了"STA 资源已释放"**。

## 1. 调用关系总图（已证实，源码直接引用）

```
                 ┌─ app_wifi_event_handler (STA_START)            wifi.c:1986
调度入口(6 个) ──┼─ app_wifi_event_handler (STA_DISCONNECTED+BLE)  wifi.c:2167 (250ms)
app_wifi_schedule_connect_task(delay)  wifi.c:1405
  │  拒绝条件: s_wifi_auto_connect_suspended / app_wifi_connect_blocked /
  │            s_wifi_connect_task != NULL / 非 STA 模式
  ↓
app_wifi_connect_task_fn()  wifi.c:1198
  │  延时后只二次检查 app_wifi_connect_blocked()   ← ★ 不查 tryconnect/op/STA 状态
  ↓
app_wifi_connect_best_saved_profile()  wifi.c:975   ← ★ 入口无 owner/op 检查
  ├─ fast_path: disconnect→set_config→connect      wifi.c:1035/1036/1050
  └─ profile  : app_wifi_connect_profile_index()   wifi.c:901
                disconnect→set_config→connect      wifi.c:939/942/960

                 ┌─ app_wifi_apply_start_released   wifi.c:1942
其它调度者 ──────┼─ monitor: runtime retry          wifi.c:1342
                 ├─ monitor: ble fast retry(120s)   wifi.c:1369
                 ├─ monitor: startup retry          wifi.c:1376
                 ├─ apply_start_probe               wifi.c:3111
                 └─ tryconnect finish(400ms)        wifi.c:3510  ← ★ 无条件 resume+schedule

tryconnect: app_wifi_try_connect_common()  wifi.c:3520
  entry_blocked → tryconnect_begin(claim OP_TRYX) → auto_connect_suspend()
  → wait_connect_task_idle(15s)  wifi.c:1412   ← ★ 只等任务指针,不等 STA 状态
  → app_wifi_perform_scan()  wifi.c:3448 → scan_lock(20s) → perform_scan_impl
     → esp_wifi_scan_start(block=false)  wifi.c:3426  ← ★ 无 STA 状态前置检查
  → 失败: vTaskDelay(500ms) 重试同一调用  wifi.c:3577-3581  ← ★ 纯时间退避

cleanup: cmd.c:460 app_cmd_net_async_cleanup()
  → 清 trial/job、netcheck_cancel、try_cancel(CONNECTING 时)
  → app_wifi_schedule_restart()  (cmd.c:497)  ← ★ 只入队即返回 true
  → operation.c:489/650 收到 true 后置终态(CANCELLED/EXPIRED)

wifi_restart 任务: wifi.c:3780
  vTaskDelay(250ms) → app_wifi_op_claim_apply(15s)  ← ★ 等新 tryconnect 释放 OP_TRYX
  → apply_cfg(TQ2026)  wifi.c:3804/2999
```

---

## Q1 auto_connect 有哪些触发源？

- **证据**：199650419 `wifi:state: run -> init (0x0)`；199650429
  `wifi: auto_connect: selected saved profile index=1 ssid=CMCC-502`；
  199656249 `STA down with BLE connected: … 120s formal-only monitor owns recovery`。
- **文件 / 函数**：`main/wifi.c` · `app_wifi_connect_best_saved_profile()`（打印点 :1185）
  ← 唯一调用者 `app_wifi_connect_task_fn()`（:1218）← 唯一创建者
  `app_wifi_schedule_connect_task()`（:1405）。
- **调用者（6 个调度源）**：`:1942 apply_start_released`、`:1986 STA_START`、
  `:2167 STA_DISCONNECTED(BLE 连接态, 250ms)`、`:3111 apply_start_probe`、
  `:3510 tryconnect finish(400ms)`、monitor 三支 `:1342/:1369/:1376`。
- **被调用者**：`app_wifi_connect_profile_index()` :901（profile 路径）或
  fast_path 分支（:1017-1050）；二者都 `esp_wifi_disconnect → set_config → connect`。
- **关键状态变量**：
  - `s_wifi_auto_connect_suspended`（写：`:1383 suspend / :1388 resume`，读：
    `schedule_connect_task :1408`、monitor `:1282/:1324/:1352`）——**只挡调度，不挡已建任务**；
  - `s_wifi_connect_task`（写：`xTaskCreate :1434`、task_fn 尾部置 NULL :1219；
    读：`wait_connect_task_idle :1412`、monitor）；
  - `s_wifi_fast_path_pending` / `s_wifi_formal_only_pending`（写：`:597/:621/:1214-1216`，
    读：`:1013/:1218`）；
  - `s_wifi_sta_connecting`（写：`connect_profile_index :958`、`fast_path :1045`、
    事件/超时清零；读：monitor `:1281/:1325/:1353`）。
- **已证实**：`s_wifi_auto_connect_suspended` 仅在 `schedule_connect_task` 入口检查；
  `app_wifi_connect_task_fn` 延时后**只**检查 `app_wifi_connect_blocked()`（离线门控），
  **不检查** tryconnect 活跃、`s_wifi_op_owner`、`s_wifi_sta_connecting`；
  `app_wifi_connect_best_saved_profile` 入口也无 owner/op 检查。
- **待确认（Q1.4）**：199650429 那一发具体由 6 个调度源中的哪一个创建。仅凭清单的
  4 行无法唯一定位（`auto_connect: selected` 只能证明"跑在 connect task 里"）。
  **判别方法**（用完整 973 行日志 grep，任取命中即定案）：
  `first schedule deferred to monitor` / `apply_start_released` / `STA_START` /
  `runtime retry: cycle=` / `ble fast retry: cycle=` / `apply_start_probe` /
  `tryconnect … finish` 附近 400ms 前的行。
  **结构性结论不依赖此项**（见"结论"）。
- **Q1.5（120s monitor vs event-chain retry）**：
  - event-chain retry = `STA_DISCONNECTED` 处理器里的直连/换 profile/退避链
    （`wifi.c:2136-2210`），BLE 连接态下被抑制（`:2126-2133`，打印
    "event-chain retry suppressed"）；
  - 120s monitor = `app_wifi_boot_monitor_task_fn()`（`wifi.c:1260-1380`，1s 节拍）的
    BLE 连接态分支（`:1352-1372`），常量 `APP_WIFI_BLE_FAST_RETRY_MS=120000`（:50），
    **只试正式 SSID（formal-only，不扫描 profile）**；归属 `main/wifi.c` 单模块。
- **结论（对方案 A）**：AUTO_CONNECT 的执行入口只有一个
  （`app_wifi_connect_best_saved_profile`），**仲裁点就放在它入口 + task_fn 二次检查**；
  不必改 6 个调度源。`suspend` 只是调度门，需升级为"执行前复核 + 可取消"。

## Q2 pre-scan 从哪里进入？

- **证据**：199650449 `wifi: scan: start failed: ESP_ERR_WIFI_STATE`（预扫描 #1）；
  199650479 `pre-scan failed …, one retry in 500ms`；199650999
  `wifi:sta_scan: STA is connecting, scan are not allowed!` + 第二次失败 +
  `pre-scan failed twice, abort before connect`。
- **文件 / 函数**：`main/wifi.c` · `app_wifi_try_connect_common()` :3520 →
  `app_wifi_perform_scan()` :3448 → `app_wifi_perform_scan_impl()` :3397 →
  `esp_wifi_scan_start()` :3426。
- **调用者**：`app_cmd_net_async_run()`（cmd.c 异步 worker）→ `app_wifi_try_connect_ip()`
  → `app_wifi_try_connect_common()`。
- **被调用者**：`esp_wifi_scan_start(&cfg,false)`（驱动 libnet80211）。
- **关键状态变量**：`s_scan_lock`/`s_scan_holder`（R-SCAN-3 有界锁，只互斥**扫描之间**）；
  `s_wifi_sta_connecting`（预扫描前**未读**）；`s_wifi_scan_count`/`S_WIFI_SCAN_RECORDS`。
- **已证实**：
  1. 进入预扫描前**没有 STA 状态检查**——`perform_scan_impl` 直接调
     `esp_wifi_scan_start`；`ESP_ERR_WIFI_STATE` 来自驱动；
  2. "STA is connecting, scan are not allowed!" **是 ESP-IDF 驱动打印**，不是业务层
     （全仓无此字符串）；
  3. 失败重试 = `vTaskDelay(500ms)` 后**原样再调**（:3577-3581），无状态等待、
     无重试计数器对象（两次失败即 `APP_WIFI_TRY_SCAN_FAILED`，:3587-3592）；
  4. 预扫描**不拆链**——它不做 disconnect。199650419 的 `run -> init`
     来自 auto_connect 的 `esp_wifi_disconnect()`（`connect_profile_index :939`）。
     **缺陷文档 §3.1 把该行注为"新 op 为扫描拆链"需更正。**
- **待确认**：`state=7` 与 "connecting" 是否同一状态量——**否**。`state=7` 是
  operation 状态机枚举 `APP_OP_CANCELLED`（operation_state.h:5-15），
  与驱动 STA 状态无关；两者只是恰好在同一日志窗口出现。
- **结论（对方案 A）**：扫描入口的仲裁 = **预扫描前等 `!s_wifi_sta_connecting`**
  （有界）+ 扫描失败区分"状态拒绝"与"真扫描失败"；`wait_connect_task_idle`
  必须升级为"STA 静默"。若已有 ready 判据可复用，最小改动不需要新模块。

## Q3 restore_persistent 由谁驱动？apply_cfg 为什么延迟？

- **证据**：199641619 `rescue=restore_persistent … (cleanup without commit)`；
  199651009 新 op 终态；199651029 `apply_cfg: ssid='TQ2026'`；199651409
  `fast_path: ssid='TQ2026'`；199661159 `connect_profile: ssid='CMCC-502'`。
- **文件 / 函数**：`main/cmd.c` · `app_cmd_net_async_cleanup()` :460（打印 :497）
  → `app_wifi_schedule_restart()` `wifi.c:3829` → `wifi_restart` 任务
  `app_wifi_restart_task_fn()` `wifi.c:3780` → `app_wifi_op_claim_apply(15000)`
  `wifi.c:2597` → `app_wifi_apply_cfg()` `wifi.c:2999`。
- **调用者**：operation 的 `cleanup_fn`——由 `app_operation_cancel()`
  （operation.c:489）与 `app_operation_expire_one()`（operation.c:650）**同步调用**；
  另有 cmd worker 的 rollback 分支（cmd.c:621）直接调用。
- **被调用者**：`app_wifi_apply_cfg` → `esp_wifi_set_config`（:2999）→ 后续 connect
  （apply 完成后由 STA_START/调度链接手）。
- **关键状态变量**：`s_wifi.restart_task`（单实例，`schedule_restart` 创建/通知）；
  `s_wifi_op_owner`（`APP_WIFI_OP_APPLY` 由 `claim_apply` 取得，`release_apply` 释放）；
  `s_wifi_apply_hold` / `s_wifi_apply_await_start`（危险段门控）。
- **已证实**：
  1. cleanup **不等待**任何异步任务退出；`schedule_restart()` 入队即 return true，
     operation 随即置终态——**"cleanup 成功"只表示"恢复已排队"**；
  2. restart 任务醒来后 `vTaskDelay(250ms)` + `claim_apply(15s)`；期间若新 tryconnect
     持有 `OP_TRYX`，只能等。新 op 在 199651009 释放 → apply 在 199651029 落地
     （间隔 20ms 正是该等待的结果，**不是巧合，是 OP_APPLY 串行化的必然**）；
  3. 两份配置源：`apply_cfg` 用 `app_wifi_build_cfg_from_config()` ← `s_config`
     （NVS 正式配置，当前=TQ2026）；`auto_connect` profile 路径用
     `s_wifi_profiles[]`（NVS 已存 profile 列表，index=1=CMCC-502）；
     fast_path 用 `s_config.sta_ssid/password`（=TQ2026）。
- **待确认**：旧配置是否会覆盖新配置——**结构上被 OP_APPLY 串行化挡住**（restart
  在新 tryconnect 运行期间拿不到 APPLY）；但"旧恢复晚于新 op 终态"这一时序是确定的，
  若新 op 是 **success**（COMMITTING→SUCCESS），cleanup 侧 `state != COMMITTING` 判定
  为假 → 不调度 restore，因此不会覆盖已提交配置。**残余风险**：新 op 在 got_ip 前
  被 cancel/expire 时，旧 restore 会在其后落地——这正是 RACE-02 的场景。
- **结论（对方案 A）**：CANCEL_CLEANUP 的完成条件 = **`apply_cfg` 返回**（成功或
  放弃），不是 cleanup_fn 返回；必须在 `schedule_restart` 与 restart 任务之间加
  "pending → complete" 的两点标志，并让新 op 等待它。

## Q4 旧 job 的真正退出条件是什么？

- **证据**：199645179 `cmd: async tryconnect op=op-5737… stale after wait (gen=15
  state=7), no job touch`；199645189 `operation: run op-1273… -> connecting`；
  199650439 `tryconnect: old disconnect consumed reason=8(ASSOC_LEAVE)`。
- **文件 / 函数**：`main/cmd.c` · `app_cmd_net_async_run()`（worker 任务体，
  stale 判定 :563-567）；worker 由 `app_operation` 动态创建（operation.c 的
  worker 创建路径），run_fn 返回后任务自删。
- **调用者**：operation 框架（`app_operation_start` 的 run_fn）。
- **被调用者**：`app_wifi_try_connect_ip()` → `app_wifi_try_connect_common()`。
- **关键状态变量**：`st1.state`（7=`APP_OP_CANCELLED`）、`st1.generation`、
  `s_net_async`（job，cleanup 已 `app_cmd_net_async_clear_job()`）、`s_net_trial`。
- **已证实**：
  1. `state=7` = `APP_OP_CANCELLED`（operation_state.h 枚举顺序）；
  2. "no job touch" = 该路径**不读写** job/trial（早退），因为 cleanup 已清 job，
     写回会造成代次污染；
  3. 199645179 时旧 worker 的 `try_connect_ip` **已返回**（stale 判定在其后），
     即旧 tryconnect 生命周期已 finish（release OP_TRYX + resume + schedule 400ms）；
  4. `old disconnect consumed` 晚 5.2s：那是旧链断开事件被 tryconnect 归属助手消费
     （wifi.c:2063-2065 / on_disconnected），与 job 退出无直接因果，只是同一窗口的
     事件顺序。
- **待确认**：stale 之后 job 是否仍持有资源——**否**（job 已由 cleanup memset；
  仅 worker 栈在返回前短暂存在）。真正残留的是 finish 的**副作用**：
  `auto_connect_resume()` + `schedule_connect_task(400ms)`（wifi.c:3509-3510）。
- **结论（对方案 A）**："新 op 不得越过 CANCEL_CLEANUP" 不应等 job 退出
  （job 早已释放），而应等 **cleanup_pending 标志**；job 退出条件保持现状即可。

## Q5 是否存在统一 STA 状态机或资源仲裁机制？

- **证据**：199650419-199650999 三行（auto_connect 与 tryconnect 同窗竞争）；
  另 `probe first schedule deferred to monitor (gen=35)` /
  `sta_start_evt gen=35 ignored (probe completed this gen)`（A8 的 gen 机制）。
- **文件 / 函数**：无统一状态机；现有三套局部机制：
  1. `s_wifi_op_owner`（`APP_WIFI_OP_NONE/APPLY/TRYX`，wifi.c:90-94）：
     `tryconnect_begin :268` claim TRYX、`tryconnect_release :289` 释放、
     `op_claim_apply :2597` / `op_release_apply :2613` 管 APPLY；
  2. `s_wifi.connect_inflight` 计数（`app_wifi_connect_begin :2462` /
     `connect_end :2492`，上限 `APP_WIFI_CONNECT_INFLIGHT_MAX`）——**计数不是互斥**；
  3. gen 机制：`s_wifi.link_gen`（netcheck 绑定）、`s_wifi_apply_start_gen`（A8 等
     STA_START）、tryconnect phase/result 状态机。
- **直接调用驱动 STA 的函数（全部在 wifi.c，共 9 处）**：
  `connect_profile_index`（:939/942/960）、`connect_best_saved_profile`（:1035/1036/1050）、
  `retry_with_pmf_required`（:1520/1526）、`event_handler` retry_direct（:2157）、
  `netcheck_on_result`（:2352 disconnect）、`apply_cfg`（:2999）、
  `perform_scan_impl`（:3426）、`tryconnect_finish`（:3498/3501）、
  `try_connect_common`（:3661/3679/3688）。
- **已证实**：
  - gen 只解决**事件归属/过期丢弃**，不具备操作权交接与清理确认——与清单边界
    提醒一致；
  - `s_wifi_op_owner` 覆盖 **TRYX vs APPLY**，**不覆盖 auto_connect**；
  - auto_connect 路径只用 `connect_inflight` 计数（允许并发，仅限上限），无所有权。
- **待确认**：是否需要新模块——**不需要**。最小方案是把 `s_wifi_op_owner` 扩展成
  三值（NONE/TRYX/APPLY/AUTO）并让 auto_connect 走同一 claim 原语；`connect_inflight`
  保留作诊断计数。
- **结论（对方案 A）**：复用 `s_wifi_op_owner`（增加 AUTO 值 + claim/release 原语），
  在 `app_wifi_connect_best_saved_profile` 入口 claim；违反者返回
  `ESP_ERR_INVALID_STATE` 并**延迟重试**（置 fast_path_pending，monitor 兜底）。

## Q6 terminal、cleanup、release 三者在哪里耦合？

- **证据**：199651009 终态 → 199651029 apply_cfg（20ms）；199650999
  `abort before connect`（在终态之前）。
- **文件 / 函数**：
  - 终态写入：`operation.c` · `app_operation_cancel()` :514（CANCELLED）、
    `app_operation_expire_one()` :663-668（EXPIRED/FAILED）、worker 内
    `app_operation_set_state()`（FAILED/SUCCESS/GOT_IP）。
  - cleanup：`operation.c:489`（cancel）、`:650`（expire）**同步调用** `cleanup_fn`；
    `cleanup_fn = app_cmd_net_async_cleanup`（cmd.c:460，注册于 cmd.c:659）。
  - release：`app_wifi_op_release_apply()`（wifi.c:2613）、
    `app_wifi_tryconnect_release()`（wifi.c:287）、`app_cmd_net_*_clear_job()`。
- **关键状态变量**：`op->state/error/terminal_ms/expiring`（operation.c，s_ops_mux 保护）、
  `s_net_async.active`、`s_wifi_op_owner`、`s_wifi.restart_task`。
- **已证实**：
  1. 三者是**"同步两步 + 异步第三步"**：cleanup_fn 同步返回 → 终态同步置位 →
     真正的配置恢复异步（restart 任务）；**终态被当成"资源已释放"使用**——
     这正是缺陷核心；
  2. release 幂等：`release_apply`/`release` 均有 owner 值判断，重复调用无害；
     但**不能由新 op 触发**（新 op 只能 claim，不能 release 别人的）。
- **待确认**：cleanup 失败路径——`app_cmd_net_async_cleanup` 只有 `schedule_restart`
  失败才返回 false（→ operation 置 FAILED/cleanup_failed）；apply 阶段失败由
  `app_wifi_recover_from_apply_failure` 走 LKG/DHCP 救援链，仍持 APPLY。
- **结论（对方案 A）**：最小插入点 = `app_cmd_net_async_cleanup`（置 pending）+
  `app_wifi_restart_task_fn`（清 pending）+ `app_wifi_connect_best_saved_profile`
  与 `app_wifi_try_connect_common`（读 pending/owner）。"终态 ≠ 资源释放"用
  `s_wifi_cleanup_pending` 这个独立标志表达，不新增 operation 状态。

---

# 最小改动设计（方案 A 落地草案，不含代码）

## A1 资源所有权

| 主体 | 取得方式 | 持有区间 | 释放点 |
| --- | --- | --- | --- |
| `TRYX`（tryconnect） | `app_wifi_tryconnect_begin()` CAS：`owner==NONE` | 预扫描开始 → finish | `app_wifi_tryconnect_release()` |
| `APPLY`（restart/apply） | `app_wifi_op_claim_apply()` 等 `owner==NONE` | apply 开始 → 结束/放弃 | `app_wifi_op_release_apply()` |
| `AUTO`（auto_connect，**新增**） | `app_wifi_op_claim_auto()` CAS：`owner==NONE` | 进入 `connect_best_saved_profile` → 返回 | `release_auto()`（函数出口统一） |

规则：三者互斥；`owner != NONE` 时任何其它主体**不得**调用
`esp_wifi_disconnect/set_config/connect` 发起新的 STA 操作。

## A2 状态与非法转换

- `IDLE`：owner=NONE 且 `!cleanup_pending`；
- `OP_ACTIVE`：owner=TRYX；
- `CANCEL_CLEANUP`：`cleanup_pending=true`（owner 可为 NONE——恢复在 restart 任务里等 APPLY）；
- `AUTO_CONNECT`：owner=AUTO。

非法转换处理：

| 尝试 | 处理 |
| --- | --- |
| AUTO 在 OP_ACTIVE/CANCEL_CLEANUP 时启动 | **拒绝执行**：不触碰 STA，置 `fast_path_pending=true` 后返回；monitor 在条件解除后重试（不削弱恢复） |
| TRYX 在 `cleanup_pending` 时进入预扫描 | **有界等待**（`wait_cleanup_complete(ms)`，建议 5s）；超时返回 `APP_WIFI_TRY_CONFIG_OFFLINE`（客户端按瞬态重试，契约已有该码） |
| TRYX 在 STA connecting 时预扫描 | **有界等待 `!s_wifi_sta_connecting`**（建议 8s，100ms 步进）；超时返回 `APP_WIFI_TRY_BUSY`（不再伪装 `wifi_scan_failed`） |
| APPLY 在 OP_ACTIVE 时 | 保持现状：`claim_apply(15s)` 等待（已是正确行为） |

## A3 取消/清理时序（谁等谁）

```
cancel/expire
  → cleanup_fn: 清 trial/job、try_cancel、schedule_restart、**置 cleanup_pending=true**
  → 终态置位（CANCELLED/EXPIRED）
restart 任务: 250ms → claim APPLY → apply_cfg → **所有出口清 pending**（含 drain 放弃）
  → 记日志 `cleanup complete (op=…, result=ok|abandoned)`
新 tryconnect: entry → begin(TRYX) → wait_connect_task_idle(15s)  ← 保留
  → **wait_sta_quiescent(8s)**   ← 新增（STA 非 connecting 且 inflight==0）
  → **wait_cleanup_complete(5s)** ← 新增（pending 清除）
  → 预扫描
```

超时策略：`wait_cleanup_complete` 超时**不清 pending**（防止假释放），返回
`wifi_config_offline` 让客户端重试；`cleanup_pending` 另设 20s 硬上限看门狗
（monitor 1s 节拍），超限清 pending 并
`ESP_LOGE("cleanup_pending stuck >20s, cleared")`——宁可记录异常也不永久卡死。

## A4 失败回滚

1. `apply_cfg` 失败 → 走既有 `recover_from_apply_failure`（LKG→DHCP→离线），
   **仍在 APPLY 所有权内**，结束后清 pending；
2. `claim_apply` 15s 超时（APPLY_BUSY_TRYX）→ 放弃本轮、**清 pending**、
   记 `cleanup abandoned`；此时新 op 已结束，monitor 会在 1s 内重新调度恢复；
3. `wait_cleanup_complete` 超时 → 新 op 以 `wifi_config_offline` 失败（可重试），
   **不提交任何配置**；
4. AUTO claim 失败 → 不执行、置 pending 标志，monitor 重试（恢复能力不变）。

## A5 三个竞态验收场景（串口判定）

| 编号 | 操作 | 通过判据（串口） |
| --- | --- | --- |
| WIFI-RACE-01 | auto_connect 与 tryconnect 同时触发 | 任一时刻只有一个主体发起 `esp_wifi_connect`：出现 `auto_connect deferred: op active (owner=TRYX)` **或** tryconnect 返回 busy；**不得**出现两段 init→connect 交叠 |
| WIFI-RACE-02 | cancel 后立即新 tryconnect | 新 op 受理后出现 `op gate: waiting cleanup (pending)`；随后 `cleanup complete` → 预扫描**真正启动**（`scan: done`），终态 = `failed/ap_not_found`（目标不存在时），**不得** `wifi_scan_failed` |
| WIFI-RACE-03 | 恢复配置 + auto_connect 同时 | 先 `apply_cfg`（含 `cleanup complete`），之后才出现 auto_connect 的 `esp_wifi_connect`；顺序颠倒即 FAIL |

保留原 #12 判定：两次预扫描必须**真正开始并完成**，错误码来自实际发现/连接结果。

## A6 改动清单（预计 ~70-90 行，均在 wifi.c + cmd.c 各一处）

| 文件 | 函数 | 改动 |
| --- | --- | --- |
| wifi.c | `app_wifi_op_owner_t` / claim/release 原语 | 增 `APP_WIFI_OP_AUTO` + `claim_auto/release_auto`（复用同一临界区） |
| wifi.c | `app_wifi_connect_best_saved_profile` | 入口 claim AUTO；失败即返回 `ESP_ERR_INVALID_STATE`（不碰 STA） |
| wifi.c | `app_wifi_connect_task_fn` | 延时后复核 `owner==NONE && !cleanup_pending && !tryconnect_active`；不满足则置 fast_path_pending 退出 |
| wifi.c | `app_wifi_try_connect_common` | 预扫描前加 `wait_sta_quiescent(8s)` + `wait_cleanup_complete(5s)`；新增失败码映射 BUSY/CONFIG_OFFLINE |
| wifi.c | `app_wifi_restart_task_fn` | 所有出口清 `s_wifi_cleanup_pending` + 打 `cleanup complete` |
| wifi.c | monitor | 新增 `cleanup_pending` 20s 硬上限看门狗 |
| cmd.c | `app_cmd_net_async_cleanup` | `schedule_restart` 成功后置 `s_wifi_cleanup_pending=true`（新 API `app_wifi_cleanup_begin()`） |

**不做**：不改 operation 状态机、不改错误码契约、不改扫描重试策略、
不取消 auto_connect、不引入新任务/新模块。BLE 断连后的 120s 恢复路径不变
（AUTO 被拒时只延迟，不退出）。

## A7 需要客户端侧配合确认的两点

1. **`wifi_config_offline` 的新语义**：从"entry gate 拒绝"扩展为"cleanup 未完成"，
   两者都是瞬态、都应 1.5s×3 重试（契约 errors.json 已定义 transient，无需改契约）；
2. **`wifi_scan_failed` 预期变化**：修复后 RACE-02 场景不再产生该码；
   #12 的判据保持"预扫描真正完成 + 结果来自真实发现"。

## A8 补充建议（观测，可选同批）

`app_wifi_schedule_connect_task(delay, const char *caller)` 增加 caller 标签，
task_fn 打印 `auto_connect: scheduled by <caller>`——本次 Q1.4 无法唯一定位
调度源，加这一行后同问题 10 秒定案（~10 行）。

---

## 附：本文引用的关键源码位置

| 符号 | 位置 |
| --- | --- |
| `s_wifi_op_owner` / enum | wifi.c:90-94 |
| `app_wifi_tryconnect_begin/release` | wifi.c:260-300 |
| `app_wifi_connect_profile_index` | wifi.c:901 |
| `app_wifi_connect_best_saved_profile` | wifi.c:975（打印 :1185） |
| `app_wifi_connect_task_fn` | wifi.c:1198 |
| monitor（120s / runtime retry） | wifi.c:1260-1380（常量 :50） |
| `app_wifi_schedule_connect_task` | wifi.c:1405 |
| `app_wifi_wait_connect_task_idle` | wifi.c:1412 |
| `app_wifi_op_claim_apply/release` | wifi.c:2597/2613 |
| `app_wifi_apply_cfg` | wifi.c:2999 |
| `app_wifi_perform_scan_impl` | wifi.c:3397（scan_start :3426） |
| `app_wifi_tryconnect_finish` | wifi.c:3481-3515 |
| `app_wifi_try_connect_common` | wifi.c:3520（预扫描 :3569-3595） |
| `app_wifi_restart_task_fn` | wifi.c:3780 |
| `app_wifi_schedule_restart` | wifi.c:3829 |
| `app_cmd_net_async_cleanup` | cmd.c:460（rescue :497，worker stale :563） |
| `app_operation_cancel` / `expire_one` | operation.c:457 / 619（cleanup 调用 :489/:650） |
| `app_op_state_t`（state=7） | operation_state.h:5-15 |
