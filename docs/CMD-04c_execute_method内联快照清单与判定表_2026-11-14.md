> **状态**：ACTIVE（CMD-04c 设计清单与判定表·第二轮修订；评审通过前不改代码）
> **状态**：ACTIVE（CMD-04c 设计清单与判定表·第二轮修订；评审通过前不改代码）


# CMD-04c · `app_cmd_execute_method` 内联两段快照清单与判定表（第二轮修订，2026-11-14）

> 基线：`a2c18ff`（两段自 v0 删除后无变更；04a/04b 不触及此文件区域）。
> **第二轮修订（评审）**：① `sta_password` 三副本清点与清理策略（栈副本防优化
> 擦除 / patch 堆副本删除前擦除 / 不夸大共享副本）；② 栈与锁耗时降级为估算，
> `-fstack-usage` 前后对照；③ B 段"使用范围"精确化；④ §1 前置守卫表述更正。

## 0. 前置守卫表述更正（评审指出）

初稿 §1 段 B 写"trial_ready 校验于 cancel 分支"——**不准确**。实际：
`network.apply` dispatcher 分支入口守卫 = MQTT 来源拒绝（guard_rejected）+
`app_cmd_net_trial_ready(trial_item->valuestring)` 校验（trial 无效即
trial_invalid）+ `app_config_snapshot_save()` 失败即 trial_clear。
trial_ready 校验在 **apply dispatcher 分支入口**，patch 构建之前——本段纯拷贝，
锁内无判定。

## 1. 两段现状（R8 注释原文与锁边界；行号基于 `a2c18ff`）

### 段 A · 同步 tryconnect 受理响应（wire 可见，cmd.c:2683-2696）

```c
resp = cJSON_CreateObject();
if (resp == NULL) {
    app_cmd_net_trial_clear();          /* OOM → trial 清理：现状语义，保留 */
    err = "out_of_memory";
} else {
    /* R8:响应字段在锁内拷出,防中途断连清理撕读。 */
    app_cmd_net_job_lock();
    cJSON_AddStringToObject(resp, "trial_id", s_net_trial.id);        /* 锁内 ×3 */
    cJSON_AddStringToObject(resp, "state", "got_ip");
    cJSON_AddStringToObject(resp, "ssid", s_net_trial.ssid);
    app_cmd_net_job_unlock();
}
```

- 锁内 cJSON ×3；锁内无判定（TRY_OK 后 trial 刚由本路径写入 cmd.c:333-336）；
- wire 可见（tryconnect RPC data）。

### 段 B · apply dispatcher 的 trial patch（内部参数，不外发，cmd.c:2740-2762）

```c
cJSON *patch = cJSON_CreateObject();
if (patch != NULL) {
    /* R8:trial 字段读点入锁(同步 v0 apply 与断连清理跨任务;...) */
    app_cmd_net_job_lock();
    cJSON_AddStringToObject(patch, "sta_ssid", s_net_trial.ssid);      /* 锁内 ×2 */
    cJSON_AddStringToObject(patch, "sta_password", s_net_trial.password);  /* 明文 */
    app_cmd_net_job_unlock();
    cJSON_AddStringToObject(patch, "net_mode", "sta");                 /* 锁外（不变） */
    apply_err = config_apply_patch(patch, &changed, &effect);          /* 同任务消费 */
    cJSON_Delete(patch);
    cJSON_Delete(changed);
}
```

- 锁内 cJSON ×2；锁内无判定（前置守卫在分支入口，见 §0）；
- patch 为 config_apply_patch 入参，同任务消费后删除——不上 wire。

## 2. 快照设计（两段均为"对象先建 → 锁内值拷贝 → 锁外构建"）

### 段 A 快照（栈局部，容量 53B）

```
root = CreateObject（失败 → OOM+trial_clear，同现状）
job_lock: trial_id[20] / trial_ssid[33] 值复制 → unlock
锁外: Add trial_id / state="got_ip"（常量）/ ssid
```

- 锁内零 cJSON；R8 防撕读保护保持；OOM → trial_clear 语义不变。

### 段 B 快照（栈局部，容量 98B；第三轮修正流程与失败语义）

```
apply dispatcher 入口守卫（现状不变，非本段引入）：
  1. MQTT 来源 → guard_rejected
  2. busy / trial_id 校验 + app_cmd_net_trial_ready() → trial_invalid
  3. app_config_snapshot_save() 失败 → trial_clear + storage_failed

patch = cJSON_CreateObject()
  失败 → 保持现有失败处理：
           apply_err 仍为 ESP_FAIL（未进 config_apply_patch）
           snapshot_discard + trial_clear
           err = "save_failed"（统一失败处理映射）
  成功 →
    job_lock: 值复制 trial_ssid[33] / trial_password[65] → unlock
    锁外构建: sta_ssid / sta_password / net_mode="sta"（顺序不变）
    config_apply_patch(patch, ...)                成功/失败两路径汇合
    擦除点: patch 中 sta_password valuestring → secure_wipe（删除前）
    cJSON_Delete(patch / changed)
    栈局部 trial_password → secure_wipe
  成功路径继续: save_wifi_profile → schedule_restart（既有后续，不动）
  失败路径: snapshot_discard + trial_clear + save_failed（既有映射，不动）
```

- `net_mode` 保持原锁外构建（评审认可项）；
- 前置守卫不新增不削弱（§0 更正后的入口守卫 + snapshot_save 原位）；
- **patch 创建失败的现有失败处理（snapshot_discard + trial_clear +
  save_failed）不在快照重构中改变**——快照化只影响"成功进入复制后"的
  清理增补（擦除点），不触碰 OOM 分支的既有语义。

### 2.1 `sta_password` 三副本清点与清理策略（评审 P1 核心项）

**密码在 B 段的实际副本（全量清点，不做夸大）**：

| # | 副本 | 位置 | 生命周期归属 | 本次清理责任 |
| --- | --- | --- | --- | --- |
| 1 | 共享状态 `s_net_trial.password[65]` | cmd.c 静态结构 | 原有生命周期（trial 清理时由 `app_cmd_net_trial_clear` 处理，CMD-02 边界排除） | **不声称清除**——局部擦除不覆盖它，如实记录 |
| 2 | 栈副本 `trial_password[65]` | 本次新增（dispatcher 任务栈） | 本次快照引入 | **必须擦除**（防优化） |
| 3 | patch 堆副本 `sta_password.valuestring` | cJSON 节点（本次复制） | patch 删除即释放节点，但释放 ≠ 安全擦除 | **删除前显式擦除** |
| — | `config_apply_patch()` 内部配置副本（`s_next.sta_password`） | config 层 | 原有生命周期（配置提交本身），**不属于本次局部快照** | 不声称清除；由既有配置生命周期管理 |

**清理策略（逐副本）**：

1. **栈副本（#2）**：`config_apply_patch` 返回后（成败均然），以
   **volatile 指针逐字节写零**擦除（`volatile char *p = ...; while(len--) *p++ = 0;`）
   ——普通 `memset` 在优化构建中可能被编译器消除（清零后数据不再读取），
   volatile 写强制真实执行。封装为 `app_cmd_secure_wipe(char *buf, size_t len)`
   静态助手（cmd.c 内，两段共用）；
2. **patch 堆副本（#3）**：`config_apply_patch()` **返回后、`cJSON_Delete(patch)`
   之前**——取 `sta_password` 节点（`cJSON_GetObjectItemCaseSensitive`），对
   `valuestring` 执行 `app_cmd_secure_wipe(valuestring, strlen+1)`；
   **成功与失败两条路径共用同一擦除点**（汇合点在 config_apply_patch 返回处，
   无需分叉）；
3. **共享副本（#1）与 config 层副本**：不声称清除——属于 trial/config 既有
   生命周期（CMD-02 边界排除 / 配置提交本身），文档如实记录边界。

**实现形态**（设计，非代码；patch 创建失败走既有统一失败处理——
无堆副本、无新增擦除点，快照栈副本擦除在两路径各自保留）：

```
patch = CreateObject
  失败 → 既有失败处理（snapshot_discard + trial_clear + save_failed）
         + 栈局部 trial_password secure_wipe（本次新增的清理，覆盖失败路径）
  成功 →
    job_lock: 拷贝 trial_ssid/trial_password → 局部; unlock
    Add sta_ssid/sta_password/net_mode
    config_apply_patch()
    擦除点: patch 中 sta_password valuestring → secure_wipe（删除前）
    cJSON_Delete(patch/changed)
    栈局部 trial_password → secure_wipe
```

## 3. 判定表（两段锁内均无判定——自动满足"判定完整迁移"）

| # | 段 | 现锁内判定 | 说明 |
| --- | --- | --- | --- |
| 1 | A | 无 | TRY_OK 后 trial 由本路径写入，纯拷贝 |
| 2 | B | 无 | 前置守卫全在分支入口（§0 更正后的表述），锁内纯拷贝 |

快照化不增不减判定；周边守卫（TRY_OK / guard_rejected / trial 校验 /
snapshot_save）全部留在锁外原位。

## 4. 资源预算（约束 4 修订：容量与估算分离）

| 项 | 值 | 性质 |
| --- | --- | --- |
| 段 A 局部缓冲容量 | 53B（trial_id[20]+ssid[33]） | 算术值 |
| 段 B 局部缓冲容量 | 98B（trial_ssid[33]+password[65]） | 算术值 |
| **净栈增量** | **待验证估算**——A/B 同函数不同路径，编译器可复用栈槽，不可按容量相减 | 实施时 `-fstack-usage` 输出 04b/04c 两版 `app_cmd_execute_method` 栈帧对照；烧录窗口按 04a 口径逐任务 HWM（ble_cmd） |
| job 锁内耗时 | 现状 2-3 次 cJSON 分配 ≈ 150-250µs → 纯拷贝 ≈ 5µs | **估算**，烧录窗口实测确认，不作为验收通过条件 |

## 5. wire 语义与兼容（约束 3 修订：使用范围精确化）

- **段 A**：RPC data 三字段名/值/顺序/类型不变；OOM → trial_clear 不变；
  状态恒 "got_ip"；
- **段 B**：非 wire。`config_apply_patch()` 的行为精确化（评审要求）：
  - 内部**读取** patch 字段并**提交配置到配置结构**；
  - **不保留 patch 指针**（返回后 patch 可安全删除/擦除）；
  - `changed` 返回**变更字段信息**，**不包含密码值**；
  - ⇒ patch 中的密码副本必须在函数返回后擦除，**不能只依赖 `cJSON_Delete`**。
  以上四条列入实施阶段代码审查的明确检查项；
- 协议兼容：字段省略条件（OOM/对象创建失败分支）与权限行为不变。

## 6. 实施与验收

- **实现完成 + 第一轮评审修正**（`7f6354e` + `7f86720`，均可独立 revert；
  修正 patch sha 前 16 位 `06fcfb3cde66857b`）：两段各 ~15 行改动（Add 移锁外 +
  局部拷贝 + `app_cmd_secure_wipe` 助手 + patch 堆副本擦除）；
  **`7f86720` 修正**：恢复 B 段 `sta_ssid`/`sta_password` 锁外构建——第一轮
  重构遗漏导致 config_apply_patch 收到无候选凭据的空 patch（P0）；
- **`-fstack-usage` 实测已完成**：`app_cmd_execute_method` 栈帧
  **04b=912B / 04c=912B（零增长）**——快照栈局部被编译器与既有局部复用抵消；
- 待烧录窗口验收：段 A wire 逐字段一致（补抓 sync tryconnect 受理响应
  golden）；
- 其余验收（锁内 grep 零 cJSON / build 0/0 / 主机 8 套 / 密码三副本生命周期
  代码审）已在本 commit 树完成。

## 7. 修订记录

| 轮次 | 变更 |
| --- | --- |
| 初稿（6ba07b9） | 两段纯拷贝设计；sta_password 生命周期表（不完整） |
| 第二轮（85407bd） | ① 三副本清点表；② 栈副本 volatile 擦除 + patch 堆副本删除前擦除；③ 使用范围四条精确化；④ §0 前置守卫表述更正；⑤ 资源估算降级 |
| 第三轮（6f6b5c2） | §2 段 B 流程与失败语义修正——patch 创建在 trial_ready/snapshot_save 之后；patch 创建失败走既有统一失败处理，不在快照重构中改变 |
| **第四轮（代码归档）** | `7f86720`：恢复 B 段 sta_ssid/sta_password 锁外构建，修复 patch 缺失候选凭据的 **P0 回归**（第一轮重构遗漏）。**审查经验**：锁内零 cJSON 只证明锁边界合规，锁外 JSON/patch 字段完整性必须单独验收——两项已分开验收 |
