> **状态**：ACTIVE（CMD-04c 设计清单与判定表；评审通过前不改代码）
> **状态**：ACTIVE（CMD-04c 设计清单与判定表；评审通过前不改代码）


# CMD-04c · `app_cmd_execute_method` 内联两段快照清单与判定表（2026-11-14）

> 基线：`22d29c8`（execute_method 两段自 v0 删除后无变更；04a/04b 不触及此文件区域）。
> 范围（CMD-04 方案 §3.3 批准项）：**A = network.tryconnect 同步受理响应**
> （cmd.c:2689-2696）、**B = network.apply dispatcher 的 trial patch**
> （cmd.c:2745-2760，含 `sta_password` 明文）。**不纳入**：apply_async COMMITTING
> 段（R2-P0，登记不动）、`s_net_trial` 全面重构（CMD-02 边界排除）。

## 1. 两段现状（带 R8 注释原文与锁边界）

### 段 A · 同步 tryconnect 受理响应（wire 可见）

```c
/* cmd.c:2683-2696，network.tryconnect dispatcher → app_wifi_try_connect(15s 阻塞)
   返回 APP_WIFI_TRY_OK 之后 */
resp = cJSON_CreateObject();
if (resp == NULL) {
    app_cmd_net_trial_clear();          /* OOM → trial 清理：现状语义，保留 */
    err = "out_of_memory";
} else {
    /* R8:响应字段在锁内拷出,防中途断连清理撕读。 */
    app_cmd_net_job_lock();
    cJSON_AddStringToObject(resp, "trial_id", s_net_trial.id);        /* 锁内 Add ×3 */
    cJSON_AddStringToObject(resp, "state", "got_ip");
    cJSON_AddStringToObject(resp, "ssid", s_net_trial.ssid);
    app_cmd_net_job_unlock();
}
```

- **锁内 cJSON ×3**；锁内**无判定**（TRY_OK 后 trial 刚由本路径写入 :333-336，
  纯拷贝段）；
- **wire 可见**：此 resp 是 tryconnect RPC 的 data——字段名/类型/顺序必须逐字不变。

### 段 B · network.apply dispatcher 的 trial patch（内部参数，不外发）

```c
/* cmd.c:2740-2760，network.apply dispatcher（同步 v1 apply 路径） */
cJSON *patch = cJSON_CreateObject();
if (patch != NULL) {
    /* R8:trial 字段读点入锁(同步 v0 apply 与断连清理跨任务;...) */
    app_cmd_net_job_lock();
    cJSON_AddStringToObject(patch, "sta_ssid", s_net_trial.ssid);      /* 锁内 Add ×2 */
    cJSON_AddStringToObject(patch, "sta_password", s_net_trial.password);  /* ← 明文 */
    app_cmd_net_job_unlock();
    cJSON_AddStringToObject(patch, "net_mode", "sta");                 /* 锁外（不变） */
    ...
    apply_err = config_apply_patch(patch, &changed, &effect);          /* 同任务立即消费 */
    cJSON_Delete(patch);
    cJSON_Delete(changed);
}
```

- **锁内 cJSON ×2**（sta_ssid + **sta_password 明文**）；锁内**无判定**（前置守卫
  在分支入口：MQTT guard_rejected、trial_ready 校验于 cancel 分支、snapshot_save
  失败即 trial_clear——本段纯拷贝）；
- patch 为 **config_apply_patch 的入参**（内部消费后即 `cJSON_Delete`），
  **不上 wire**。

## 2. 快照设计（值拷贝；两段均为"对象先建 → 锁内拷贝 → 锁外构建"）

### 段 A 快照（栈局部，≈53B）

```
锁内拷贝: trial_id[20] / trial_ssid[33]（s_net_trial.id/ssid 按值复制）
锁外构建: trial_id / state="got_ip"（常量）/ ssid —— 三字段、顺序、值不变
```

- 无判定迁移（现锁内无判定）；R8 防撕读保护**保持**（拷贝仍在锁内）；
- OOM 路径（resp 失败 → trial_clear）不变。

### 段 B 快照（栈局部，≈98B）

```
锁内拷贝: trial_ssid[33] / trial_password[65]（s_net_trial.ssid/password 按值复制）
锁外构建: sta_ssid / sta_password / net_mode="sta" —— 字段名、值、顺序不变
```

- 无判定迁移；R8 防撕读保护保持；
- **`sta_password` 生命周期（评审约束 3）**：

| 阶段 | 规则 |
| --- | --- |
| 授权 | 分支入口既有守卫不变（MQTT → guard_rejected；权限由 cmd_policy 承载）——本段前置条件**不新增不削弱** |
| 复制 | 锁内 `s_net_trial.password` → 局部 `trial_password[65]`（同任务栈，不落堆） |
| 使用 | 仅写入 patch 字段 → `config_apply_patch` 同任务立即消费 → `cJSON_Delete(patch)`（明文随节点释放） |
| 清理 | `config_apply_patch` 返回后（成败均然）**显式 `memset(trial_password, 0, sizeof)`** 再离开作用域——明文不在栈上残留（评审"失败清理"要求） |
| 不变量 | 不写日志/不入 resp/不出任务栈拷贝之外（评审"使用范围"要求） |

## 3. 判定表（锁内判定 → 锁外等价判定）

| # | 段 | 现锁内判定 | 快照字段 | 说明 |
| --- | --- | --- | --- | --- |
| 1 | A | 无（TRY_OK 后纯拷贝） | trial_id/ssid | trial 由本路径 TRY_OK 前写入，R8 锁保护 |
| 2 | B | 无（纯拷贝） | trial_ssid/password | 前置守卫全在分支入口（guard/policy），锁内无判定 |

**判定完整迁移确认（约束 2）**：两段锁内**本就无判定**——快照化不增不减；
周边守卫（TRY_OK、guard_rejected、snapshot_save）全部留在锁外原位。

## 4. 资源预算（约束 4）

| 项 | 值 | 说明 |
| --- | --- | --- |
| 段 A 快照 | 53B（trial_id[20]+ssid[33]） | ble_cmd 任务（8192B）——若同步路径也来自 BLE |
| 段 B 快照 | 98B（trial_ssid[33]+password[65]） | 同任务（apply 与 tryconnect 不同时在栈） |
| 锁内耗时 | 现状 2-3 次 cJSON 分配 ≈ 150-250µs → 纯拷贝 ≈ 5µs | job 锁为递归互斥，apply 再入段不受影响（不同段） |
| `_Static_assert` | 不适用（栈局部 char 数组，宽度即声明值） | — |

净栈增量 ≈ +100B（两段不同路径，不叠加）——ble_cmd 8192B 占比 ~1.2%；
烧录窗口按 04a 口径对照 ble_cmd 任务 HWM。

## 5. wire 语义与兼容（约束 3）

- **段 A**：RPC data 三字段（trial_id/state/ssid）名/值/顺序/类型不变；
  OOM → trial_clear 行为不变；状态恒 "got_ip"（TRY_OK 前提不变）；
- **段 B**：非 wire（patch 为内部入参）；字段名/值/顺序不变；
  `sta_password` 明文的暴露面**不扩大**（原就在 patch 内）——快照化只改变
  "锁内分配 → 锁外分配"的位置与增加消费后清零；
- **协议兼容**：字段省略条件（OOM 分支）与权限行为不变。

## 6. 实施与验收（评审通过后）

- 实现：两段各 ~10 行改动（Add 移锁外 + 局部拷贝 + memset 清理）；
- 验收：
  - 段 A wire 逐字段一致（tryconnect 成功路径 golden：`golden_pre_04a.json`
    不含此段——**烧录窗口补抓 sync tryconnect 受理响应**）；
  - 段 B 无 wire（内部）——以代码审 + `config_apply_patch` 前后 patch 内容
    日志（如现有 debug 开）对照；
  - 锁内 grep 零 cJSON（job 锁两段区间）+ build 0/0 + 主机 8 套；
  - `sta_password` 生命周期：代码审（拷贝→消费→memset 三点在案）。
- 分期：04c 单独 commit（基线 = 04b 签收版），可独立 revert。
