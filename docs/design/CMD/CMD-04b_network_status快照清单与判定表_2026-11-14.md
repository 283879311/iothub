> **文档状态：PENDING（待实施设计）**
> 材料性质：PENDING DESIGN（待实施设计依据）
> 本文定义 network_status 分域快照与验收要求；是否实施及当前状态只见 [CMD 当前任务台账](../../status/CMD任务台账.md)。不得将设计清单描述为已实现或已验收。

# CMD-04b · `app_cmd_build_network_status` 快照清单与判定表（2026-11-14）

> 基线：`e1d2058`（network_status 自 fencing 后无变更）。范围：四段锁内构建
> （runtime/config/mqtt/job-trial）分域快照化 + 构建移锁外；nc_tcp、rssi 段已合规保持现状。

## 0. 目标编译器探针实测（kaboom 法，真实头文件 wifi.h/mqtt.h/esp_wifi.h）

| 类型 | sizeof（xtensa esp32 实测） |
| --- | --- |
| `app_cmd_network_status_snap_t`（本轮快照） | **312B** |
| `wifi_ap_record_t`（rssi 段局部） | **92B** |
| `app_net_cfg_state_t`（cfg_state 字段类型） | 4B（enum） |

## 1. 快照结构（分域值快照，零悬空指针；字段宽度来自真实头文件）

```c
typedef struct {
    /* runtime 域（一次 app_wifi_runtime_lock，含 cfg_state 收编） */
    bool sta_connected;
    bool sta_has_ip;
    char sta_ip[16];               /* runtime.h:95 */
    char last_disconnect[32];      /* runtime.h:98 */
    char connected_ssid[33];       /* runtime.h:102 */
    int connected_profile;         /* runtime.h:103 */
    bool internet_verified;        /* = s_wifi.net_verified */
    char net_verify_stage[12];     /* runtime.h:119 */
    app_net_cfg_state_t cfg_state; /* enum 4B（收编项，见 §4-skew） */
    /* config 域（一次 app_config_lock） */
    char sta_ssid[33];             /* config.h:44 */
    bool password_set;
    uint8_t net_mode;
    bool persistent_use_static;    /* config.h:60(uint8_t)→bool */
    char persistent_static_ip[16]; /* config.h:61 */
    /* mqtt 域（一次 app_mqtt_lock） */
    bool mqtt_connected;
    char mqtt_last_error[96];      /* mqtt_runtime_t.last_error */
    /* trial 域（一次 job_lock，条件捕获——见 §3 触发条件） */
    bool trial_ready;
    char trial_id[20];             /* app_cmd_net_trial_t.id 同宽 */
    char trial_ssid[33];
} app_cmd_network_status_snap_t;
```

- **sizeof = 312B（目标编译器实测，§0 探针）**；实现将内置
  `_Static_assert(sizeof <= 400)` 守卫；
- 调用任务：ble_cmd（8192B）/ mqtt 事件任务（esp-mqtt 内建栈）；
  **净栈增量待测**（§4）——不作为设计结论；
- **全部值为拷贝**，零指向 runtime/config/mqtt/trial 内部成员的指针。

## 2. 四段 → 四域映射（现锁内构建 → 快照域）

| 现锁段（行号） | 现锁内构建 | 快照域 |
| --- | --- | --- |
| runtime (:1121-1136) | 8 次 Add（sta_connected/has_ip/sta_ip/last_disconnect/connected_ssid/connected_profile/internet_verified/net_verify_stage） | runtime 域 8 字段 |
| config (:1175-1183) | 5 次 Add（sta_ssid/password_set/mode/persistent_use_static/persistent_static_ip） | config 域 5 字段 |
| mqtt (:1187-1190) | 2 次 Add（mqtt_connected/mqtt_last_error） | mqtt 域 2 字段 |
| job-trial (:1195-1203) | ready 判定 + 条件 3 次 Add（trial_id/state="got_ip"/ssid）+ else（state="idle"） | trial 域（ready/id/ssid；idle 分支保留） |

不变段：nc_tcp（netcheck API，自有锁）、rssi 段（已合规模式）。

## 3. 实施流程与 trial_ready 触发条件（评审项 4，分阶段构建版）

**触发条件逐字保持**：`trial_ready`（含过期清理副作用——`app_cmd_net_trial_ready`
内部拿递归 job_lock 并对过期 trial 调 `app_cmd_net_trial_clear()`）**只在
root 与 trial JSON 对象都创建成功时执行**。

**第三轮修正（评审指出 `cJSON_AddObjectToObject` 是"创建+插入"合一）**：
初版流程把 trial 对象的 AddObject 放在其他字段构建之前——对象在创建瞬间即
插入 root，导致 trial 成为**第一个**顶层键，"trial 仍在最后"不成立。修订为
**分阶段构建**：先完成全部非 trial 字段的构建，最后才创建 trial 对象：

```
root = cJSON_CreateObject();            失败 → OOM return（trial_ready 不执行 ✓ 同现状）

[capture runtime 域]                     （一次 runtime_lock，9 字段含 cfg_state）
[nc_tcp 统计]                            （API，现状不变）
[rssi 段]                                （现状不变：connected 快照 + 锁外 get_ap_info）
[capture config 域]                      （一次 config_lock）
[capture mqtt 域]                        （一次 mqtt_lock）

[构建 runtime/nc_tcp/rssi/config/mqtt 字段]   ← 全部在锁外，键序与现状一致

trial = cJSON_AddObjectToObject(root, "trial");
                                         失败 → trial 域跳过（trial_ready 不执行 ✓ 同现状）

[trial != NULL → 一次 job_lock：trial_ready 判定 + 条件拷贝 trial_id/ssid → unlock]

[构建 trial 子字段（锁外）]：
    ready → trial_id / state="got_ip" / ssid
    else  → state="idle"
```

逐条满足：
- `trial_ready` 只在 root 与 trial 对象**均创建成功后**执行（清理副作用触发
  条件不变）；
- `trial` 仍是**最后一个顶层键**（键序与现状一致，golden 对照无需排序）；
- job/runtime/config/mqtt 锁内零 cJSON；
- trial 的 ready 判定与 id/ssid 拷贝在**同一次 job_lock** 内。

## 4. 一致性与 skew（评审项 2 修订）

- **域内一致**：runtime 9 字段（含 cfg_state）/ config 5 / mqtt 2 / trial 条件域
  各单锁一次拷贝 ✓；
- **cfg_state 收编的 skew 变化（如实写明）**：现状采样顺序 =
  `runtime(8 字段) → config → runtime(cfg_state 第二次加锁) → mqtt`；
  收编后 = `runtime(9 字段含 cfg_state) → config → mqtt`。
  ⇒ **cfg_state 与 config 域（persistent_use_static/persistent_static_ip）
  从"后于 config 采样"变为"先于 config 采样"**——两者可能来自不同时间点，
  不再与现状同序。评估：cfg_state 与持久静态配置在业务上无关联约束
  （cfg_state 是运行期配置状态机，persistent_use_static 是持久规划），
  客户端不交叉断言两者时序 ⇒ **接受此 skew 变化**，本节即为其存档；
- **其余跨域 skew 与现状相同**（捕获顺序 runtime→config→mqtt 不变）；
- 不可能组合防护：trial 域 ready/id/ssid 同锁捕获。

## 5. 栈预算（评审项 3 修订：删除 +250B 结论）

- 快照结构 **312B**（§0 实测）；
- **净栈增量：待测**——设计不给出结论。实现时以目标编译器
  `-fstack-usage` 输出改造前后函数栈帧对照；烧录窗口按 CMD-04a 口径
  逐任务记录 HWM（ble_cmd / mqtt 事件任务，改造前后同设备同查询路径）；
- 原"现 rssi 段局部可抵消"的论证**撤回**（局部变量能否复用栈槽取决于
  编译器栈帧分配，不可由 sizeof 相减证明）。

## 6. 实施与验收（评审通过后）

- 实现：`app_cmd_build_network_status` 改为
  **capture → 非 trial 字段构建 → trial 对象 → trial 域条件捕获 → trial 字段构建**
  的分阶段流程（§3，trial 对象最后创建以保持键序）；快照结构 `sizeof` 实测
  （目标编译器探针）+ `_Static_assert(sizeof <= 400)`；
  快照 312B + `_Static_assert(sizeof <= 400)`；
- **验收（评审补充口径全纳入）**：
  - wire 逐字段一致 + **分支覆盖**：正常/缺字段/终态/过期/取消/owner 不匹配/
    trial 不匹配（golden 基线 + 烧录窗口补造分支）；
  - 锁内扫描：job/runtime/config/mqtt 锁内 grep 零 cJSON（扫描器对嵌套锁段
    有漏计，**以逐段源码核对为准**）；
  - 资源回归：build 0/0 + 主机 8 套 + **栈水位（-fstack-usage 对照 + 烧录窗口
    HWM）+ 堆余量/largest**；
  - 并发回归：RACE-01/02/03 + T4，**重点确认 status 查询期间 BLE cancel
    不再被长时间阻塞**；
  - 协议兼容：字段/类型/缺省/省略条件/权限行为不变。
- 烧录窗口：与 fencing/04a 合并（RACE 回归 + golden diff + HWM 对照同批）。
