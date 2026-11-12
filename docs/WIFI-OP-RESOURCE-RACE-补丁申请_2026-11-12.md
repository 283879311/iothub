# WIFI-OP-RESOURCE-RACE · 补丁代码申请（2026-11-12）

> 依据：设计评审签收（C1–C3/D1 关闭；退避数值统一为 **0.5/1/2s**，TRYX 等待保持 12s）。
> 本申请**只请求代码修改授权**；烧录与设备操作按评审要求**单独申请**（§7）。
> 状态：**未开始改码**。设计文档：`docs/WIFI-OP-RESOURCE-RACE-修订设计_B1-B5_2026-11-12.md`
> （§14 为 D1 后唯一口径）。

## 1. diff 范围（文件 / 函数 / 行数）

**基线**：`1.3-2ef2a38`（build_sha `98f2465cb`）。改动集中在两个文件，无新增文件。

| 文件 | 函数/对象 | 预计净增行 |
| --- | --- | --- |
| wifi.c | owner 六值枚举 + `claim_owner()/release_or_handover()/cleanup_request()` 原语（含 waiter 位、同临界区 CAS） | +45 |
| wifi.c | `app_wifi_tryconnect_begin()`（改 claim + waiter 置位） | +10 |
| wifi.c | `app_wifi_tryconnect_release()`（改 release_or_handover） | +4 |
| wifi.c | `app_wifi_connect_best_saved_profile()`（AUTO claim/统一释放/PMF pending 消费） | +20 |
| wifi.c | `app_wifi_connect_task_fn()`（延时后复核 + caller 打印） | +10 |
| wifi.c | `app_wifi_event_handler()`：`retry_direct`、PMF 重试两处非阻塞 claim | +14 |
| wifi.c | `app_wifi_netcheck_on_result()`（非阻塞 claim + teardown pending） | +12 |
| wifi.c | `app_wifi_perform_scan_command()`（改为 claim AUTO，去掉 suspend 组合） | +8 |
| wifi.c | `app_wifi_perform_scan_impl()`（owner 断言/日志） | +4 |
| wifi.c | `app_wifi_restart_task_fn()`（CLEANUP/APPLY 分流、0.5/1/2s×3、FAILED） | +28 |
| wifi.c | `app_wifi_schedule_connect_task()` + 8 个调用点（caller 参数 + 调度日志） | +18 |
| wifi.c | monitor（cleanup 看门狗 12s/25s、teardown pending、PMF pending 消费节拍） | +20 |
| cmd.c | `app_cmd_net_async_cleanup()`（调 `app_wifi_cleanup_request()`，false→cleanup_failed） | +8 |
| **合计** | | **≈ +200 / −25（净 +175，±20%）** |

> 行数比初版预估（110–140）多约 40 行，全部来自 C1–C3：waiter 优先（含事件旁路三处）、
> cleanup 回退+看门狗、PMF/teardown 两个 pending 的接续。若需压缩，可把三个 pending
> 合并为一个位图（约省 10 行），但可读性下降，建议保留分列。

## 2. B1–B5 + C1–C3 实现点对照表

| 评审编号 | 函数（文件） | 行为 |
| --- | --- | --- |
| **B1** | `claim_owner/release_or_handover/cleanup_request`（wifi.c） | 六值 owner；cleanup 请求在 owner 忙时只置 `s_cleanup_requested`；**所有释放点同锁检查并直接移交 CLEANUP**，无 NONE 可抢占窗口；`apply_cfg` 在 CLEANUP 内执行，不再 claim APPLY |
| **B2** | `claim_owner(AUTO)`（wifi.c） | 唯一取得点检查完整 owner 六值；`cleanup_pending` 标志删除 |
| **B3** | `app_wifi_restart_task_fn`（wifi.c） | CLEANUP 内 `apply_cfg×3`（0.5/1/2s），**不跑 LKG/DHCP 链**；全失败 → `CLEANUP_FAILED` 粘性；进入/离开日志带 op_id |
| **B4** | 9 个驱动点（§6 表） | #1/#2/#7 随 AUTO/TRYX；#3/#4/#5 事件路径非阻塞 claim；#6 分流 CLEANUP/APPLY；#8/#9 在 TRYX 内 |
| **B5** | `app_wifi_schedule_connect_task(delay, caller)` | **调度瞬间**打印 scheduled/refused（含 reason）；8 个 caller + `op_owner` 迁移 + `resume by` |
| **C1** | `claim_owner(TRYX/AUTO)`（wifi.c） | `s_tryx_waiter` **先置位再轮询**；所有 AUTO 取得点（含事件旁路）与 `schedule_connect_task` 入口检查 waiter；TRYX 等待 12s |
| **C2** | `cleanup_request` + monitor | `schedule_restart()` 失败 → **回退 NONE** 并返回 false；monitor 看门狗 12s re-notify / 25s forced FAILED |
| **C3** | `best_saved_profile` + monitor | PMF：被拒置 pending，下一次 AUTO 以 `fast_path + pmf.required=true` 消费（30s 过期）；netcheck：被拒置 `teardown_pending+gen`，monitor ≤1s 执行，gen 变化清位，>10s 告警 |
| **R1** | `claim_owner` 实现 | waiter 判断与 owner CAS **同一临界区**（`s_tryconnect_mux`） |
| **R2** | 文档/日志 | TRYX waiter ≤12s 期间 `sta_disconnected_ble` 被拒，属已知代价（设计 §10） |

## 3. "不触碰"清单

1. operation 状态机（`operation.c` / `operation_state.*` 一行不改）；
2. 错误码契约（`contract/v1/*.json` 不改；复用既有 `cleanup_failed`/`busy`/`wifi_scan_failed`）；
3. 扫描重试策略（预扫描 500ms 重试一次、`wifi_scan_failed` 分类不变）；
4. `auto_connect` 开关与 120s BLE formal-only 恢复路径（AUTO 仅延迟，不取消不削弱）；
5. netcheck 判定与代次绑定逻辑；
6. NVS/配置格式（无迁移、无新 key）；
7. `esp_wifi_*` 调用参数（scan_time 0/0、PMF capable 等保持）。

## 4. 编译与自测（提测门槛）

- 构建：`idf.py -B build build` **0 error / 0 warning**（当前基线即为 0/0）；
- 主机自测全绿（现有 7 套）：
  `tests/cmd_policy`、`tests/cmd_error`、`tests/cmd_cfg_txn`、`tests/cmd_context`、
  `tests/netcheck`、`tests/token_store`、`tests/syntax_check.py`（34 文件）；
- 纯逻辑新增（若拆出可测函数）：owner 迁移表（NONE/TRYX/AUTO/APPLY/CLEANUP/FAILED
  的合法/非法迁移）加一组 host 用例，预计 +12 例；
- 提交前 `git diff --check`（无空白错误）。

## 5. RACE-01/02/03 复现步骤与串口判据（沿用评审 §8）

| 编号 | 复现步骤 | 通过判据（串口） |
| --- | --- | --- |
| **RACE-01** | ① 让设备处于无连接态（关闭上游 AP 或 `network.status` 确认 `sta_connected=false`），monitor runtime-retry 活跃；② 在此窗口内用 BLE 发 `network.tryconnect`（不存在 SSID 亦可） | 任一时刻只有一个主体发起 STA 操作：出现 `auto_connect: schedule refused ... reason=tryx_waiting` 或 `AUTO claim denied: TRYX waiter`；**不得**出现两段 `run->init → connect` 交叠 |
| **RACE-02** | T4 #12 `operation 语义` 单项运行（cancel 后立即新 tryconnect，目标 `__ble_selftest_absent__`） | 新 op 受理后出现 `claim_owner waiting owner=CLEANUP`；随后 `cleanup complete`；预扫描**真正启动并完成**（无 `ESP_ERR_WIFI_STATE`）；终态 **`failed/ap_not_found`**；不得以 `busy` 收尾、不得 `wifi_scan_failed` |
| **RACE-03** | ① 断开上游 AP 使 monitor 调度 AUTO；② 同时取消一个在飞 tryconnect（BLE `network.cancel`） | **CLEANUP 期间无任何 STA 发起操作**（scan/connect/disconnect/set_config 行一律无）；`cleanup complete` 之后首条 STA 操作来自谁都可以（不限定顺序） |
| 附加（可选） | 用现有 FI 构建强制 apply_cfg 连续失败（`APP_WIFI_FAULT_INJECT` 钩子） | `cleanup failed ... attempts=3` → `CLEANUP_FAILED`；随后 tryconnect 返回 `cleanup_failed`；AUTO 全程无 STA 行；`config.set + reboot` 后恢复 |

## 6. 回滚方案

- **单 commit**：全部改动一个提交（`fix(wifi): ...`），不拆分多提交，保证 `git revert <sha>` 一次回退；
- **无状态迁移**：不改 NVS、无新配置 key、无一次性迁移代码；revert 后旧固件读到的是原样配置；
- **运行时状态**：owner/CLEANUP 均为 RAM 状态，revert 后烧录即回到旧行为（重启清零）；
  `CLEANUP_FAILED` 不会跨重启/跨固件保留；
- **回滚验证**：revert 后重编、烧录到同设备，跑 T4 #12/#23 与 41 例最小集确认回到旧行为；
- **回滚触发条件**：RACE-02 不能稳定到 `ap_not_found`、或正常路径出现新的 `busy`/`cleanup_failed`、
  或 BLE 恢复路径退化（断 BLE 后 >12s 不恢复且非已知 R2 场景）。

## 7. 烧录与设备操作（单独授权）

本申请**不包含**烧录、重启、配置变更；需另行申请窗口，并接受两个前提：

1. 设备可能进入 `CLEANUP_FAILED`（Wi-Fi 离线，直到 `config.set + reboot` 或断电）；
2. 全程串口采集，且**不与验收并发**——保持与 973 行证据相同的采集条件。

建议窗口流程（供授权时参考）：
预检（串口在录、两台设备空闲）→ 烧录 → 冒烟（启动 netcheck ok）→ RACE-01/02/03
→ 回归（T4 #12/#23 + 41 例最小集）→ **R2 专项**（TRYX 结束后立即断 BLE，
判据：Wi-Fi 恢复 ≤12s+1s，串口看 BLE disconnect → 首个 connect/GOT_IP）→
收尾（配置与基线核对）。

## 8. 待批准的边界

- ✅ 请求批准：**代码修改**（§1 范围）+ 编译/主机自测；
- ❌ 未请求：竞态验收代码（RACE 场景脚本）、烧录、设备重启、配置变更——全部等单独授权。
