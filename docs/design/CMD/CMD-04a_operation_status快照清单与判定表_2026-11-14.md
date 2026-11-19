> **文档状态：PENDING（待实施设计）
> 材料性质：PENDING DESIGN（待实施设计依据）**
> 本文定义 operation_status 快照字段、分支与验收要求；是否实施及当前状态只见 [CMD 当前任务台账](../../status/CMD任务台账.md)。不得将设计清单描述为已实现或已验收。

# CMD-04a · `app_cmd_net_operation_status` 快照清单与分支判定表（2026-11-14）

> 基线：**`dc254d5`**（评审唯一基线；49707a9/dc254d5 已在 origin/main，e1d2058 仅文档变更不影响 cmd.c——改造前对照以 dc254d5 的函数行为为准）。评审约束逐条对应：
> 值快照（约束 1）/ 判定完整迁移（约束 2）/ 协议语义（约束 3）/ 栈堆预算（约束 4）/ skew 边界（约束 5）。

## 1. 快照结构（值快照，零悬空指针）

```c
typedef struct {
    bool op_found;                       /* find/get_status 成功且 st.id 与请求 ID 二次比对一致 */
    app_op_state_t state;
    app_operation_error_t error;
    char operation_id[APP_OPERATION_ID_LEN];   /* 20B,来自 status.id */
    app_operation_owner_t owner;         /* MQTT/HTTP owner 校验用 */
    int64_t terminal_ms;                 /* finished_uptime_ms 用 */
    bool have_job_snapshot;              /* job 快照命中(active 且 operation_id 匹配) */
    bool st_wifi_connected;
    bool st_ip_acquired;
    bool st_internet_verified;
    bool st_mqtt_connected;
    bool trial_match;                    /* s_net_trial 归属本 op */
    char trial_id[20];                   /* 与 app_cmd_net_trial_t.id 同宽 */
    char trial_ssid[33];                 /* 与 app_cmd_net_trial_t.ssid 同宽 */
} app_cmd_net_op_status_snap_t;
```

- **sizeof = 112B（目标编译器 xtensa esp32 实测，探针法 2026-11-14）**；
  代码内置 `_Static_assert(sizeof <= 160)` 守卫防字段膨胀；
- **全部值为拷贝**：operation_id/trial_id/trial_ssid 按字节复制，不含指向
  `s_net_async`/`s_net_trial`/`s_config`/runtime 内部成员的任何指针（约束 1）；
- 无全局快照对象（约束：不新增全局状态）；`stage_snap_get` 为既有独立锁 API，
  锁外调用（自有 s_stage_snap_mux，不与 job 锁嵌套）。

## 2. 唯一锁内临界区（捕获段，零 cJSON）

```
app_cmd_net_job_lock();
  op = find(operation_id); get_status(op,&st)          → op_found/state/error/
                                                         operation_id/owner/terminal_ms
  have_job_snapshot = active && job.operation_id==status_id
  have_job_snapshot ? 拷贝 st_*×4                        （job 快照域）
  trial_match = trial.valid && trial.operation_id==status_id
  trial_match ? 拷贝 trial_id/ssid                      （trial 域）
app_cmd_net_job_unlock();
```

锁内耗时 = 1 次 find（s_ops_mux spinlock）+ 1 次 get_status（s_ops_mux 快照）
+ ~12 字段拷贝——**零 cJSON、零字符串格式化**（约束 4：锁内复制耗时以实测确认，
设计预期 <10µs）。注意：find 与 get_status 是**两次独立的 s_ops_mux 临界区**
（各自进出，非同一快照）——两者之间的槽位复用由 §4 的 P1 身份复核封闭。

## 3. 分支判定表（锁内判定 → 锁外等价判定，全部基于单次快照）

| # | 现锁内判定（dc254d5 行号） | 快照字段 | 锁外判定 | 响应/语义 |
| --- | --- | --- | --- | --- |
| 1 | find 失败/get_status 失败/**身份复核失败**（:815 + **P1**：get_status 后复核 `st.id == 请求 id`——find 与 get_status 之间槽位可能被回收复用） | `op_found=false`（含复核失败） | 同 | `not_found`，NULL |
| 2 | MQTT owner 校验（:820-824） | `ctx->source`（调用方参数）+ `snap.owner` | 同 | `not_found`，NULL |
| 3 | HTTP owner 校验（:826-830） | 同上 | 同 | `not_found`，NULL |
| 4 | finished_uptime_ms 条件（terminal && terminal_ms>0） | `snap.state` + `snap.terminal_ms` | 同 | 加法字段 |
| 5 | trial 块（trial.valid && operation_id 匹配） | `snap.trial_match` + `trial_id/ssid` | 同 | trial_id/ssid 字段 |
| 6 | stages source 三分支（have_job_snapshot / stage_snap 命中 / live） | `have_job_snapshot` + st_* + `stage_snap_get(id)`（自有锁）+ live 捕获（runtime/mqtt，**移出 job 锁**——现状嵌套于 job 锁内，本改造顺带消除嵌套） | 同 | stages/stages_source |
| 7 | OOM（CreateObject 失败） | — | 同 | `out_of_memory`，NULL |

**判定完整迁移确认（约束 2）**：现锁内判定共 7 类（上表）——owner 校验、
have_snapshot、trial 匹配、operation 状态关联（state/error/terminal_ms 同一次
get_status 快照）、stages 回落、OOM、not_found——**全部基于同一次锁内捕获**；
锁外不再回读 `s_net_async`/`s_net_trial`/runtime 原始成员。

## 4. 协议语义保持（约束 3）——分支矩阵与 golden

wire 逐字段一致（含字段省略/类型/条件出现）：

| 分支 | golden（改版前实测，`artifacts/wifi_op_race_20261112/golden_pre_04a.json`，sha 前 16 位 `15401a735dc9d829`） |
| --- | --- |
| success 终态（op-36A936F9E7909901） | ✅ 已抓 |
| failed 终态 ap_not_found（op-12730EEC7F1F64AA） | ✅ 已抓 |
| network.status（trial 块 + live 字段） | ✅ 已抓 |
| owner 不匹配（MQTT 查 BLE op → not_found） | 烧录窗口随批补抓 |
| accepted/connecting 在飞（trial 块出现） | 烧录窗口随批补造 |
| expired（300s TTL） | 烧录窗口随批补造（静置 5 分钟） |

**generation/owner 关联**（约束 2）：`find(status_id)` 与 `get_status` 是**两次
独立的 s_ops_mux 临界区**——两者之间槽位可能被其他任务的 operation_start 回收
复用（满表覆盖/600s 到期），`get_status` 读到的是**新 operation** 的状态。
P1 身份复核（`get_status` 返回的 `st.id` 与请求 ID 二次比对）识别并拒绝这种情况：
不一致即判 `not_found`。快照携带 owner 供校验，**不携带 generation**
（wire 无此字段；关联性由 find-by-id + P1 二次比对保证）。

## 5. 栈/堆预算（约束 4）

**证据口径（P2 第二轮评审修正）**：`app_diag_stack_snapshot()` 的 low 列表只
记录**已找到且 HWM < 2048B** 的任务——"不在列表"可能是 HWM ≥ 2048B，**也可能是
任务未被找到**。因此"不在 low 列表"**不能单独证明** >2KB 余量；`+36B` 是静态
估算值，不替代实测。基线记录降级为"间接证据"，**逐任务 HWM 实测列入烧录窗口
必做项**：

| 调用任务 | 烧录窗口必须记录（改造前/后同设备、同固件配置、同查询路径） |
| --- | --- |
| `ble_cmd`（8192B，BLE operation.get/operation.get 调用方） | ① 任务确实存在（被诊断器找到）② 实际任务名 ③ HWM 字节数 |
| MQTT 事件任务（esp-mqtt 内建，MQTT 查询调用方） | 同上：存在性/实际任务名/HWM 字节数 |

判定：改造后同任务 HWM 相对基线的下降量 ≤ 快照净增量（估算 +36B）且任务
仍 >2KB 余量 ⇒ 栈预算验收通过；否则退回评审。
全局 IDLE1=976B 与本改造无关（快照不在 IDLE 栈上分配），仅作存档。堆：改造
前后 job 锁内均无堆分配；cJSON 分配总量不变（同字段集）。

## 6. 失败路径与资源释放（约束 3）

- CreateObject 失败 → `out_of_memory`、NULL——与现状一致（快照为栈对象，
  无需释放）；
- 子 Add 失败（cJSON 返回 NULL）：**保持现状不检查**（部分响应返回）——
  现状同行为，不因快照化改变；
- 无敏感字段（04a 无 password；04c 的 sta_password 生命周期在 04c 清单单独定义）。

## 7. skew 边界（约束 5）

- runtime 域 8 字段：单锁一致 ✓；
- trial 域 2 字段 + 匹配判定：单锁一致 ✓；
- job 快照 st_*×4：单锁一致 ✓；
- **跨域**（如 state=success 与 stages live 值）：现状即非原子、客户端已容忍；
  快照化不劣化。不可能组合的防护：`have_job_snapshot` 与 `trial_match` 同锁
  判定——不会出现"stages 来自新 op 快照、state 来自旧 op 终态"的混搭
  （二者判定同一次锁内捕获，且 operation_id 匹配锚定同一 op）。
