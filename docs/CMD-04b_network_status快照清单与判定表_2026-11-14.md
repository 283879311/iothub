> **状态**：ACTIVE（CMD-04b 字段快照清单与判定表；评审通过前不改代码）
> **状态**：ACTIVE（CMD-04b 设计清单与判定表；评审通过前不改代码）


# CMD-04b · `app_cmd_build_network_status` 快照清单与判定表（2026-11-14）

> 基线：`e1d2058`（network_status 自 fencing 后无变更；CMD-04a 已签收）。
> 范围：四段锁内构建（runtime/config/mqtt/job-trial）分域快照化，构建移到锁外。
> rssi 段与 nc_tcp 段**已合规**（锁内只快照/无锁 API），保持现状。
> 字段宽度全部来自真实头文件（runtime.h:95/98/102/103/119、config.h:44/60/61、
> mqtt_runtime_t.last_error[96]），非估算。

## 1. 快照结构（分域值快照，零悬空指针）

```c
typedef struct {
    /* runtime 域（一次 app_wifi_runtime_lock 拷贝，含 cfg_state 收编） */
    bool sta_connected;
    bool sta_has_ip;
    char sta_ip[16];               /* runtime.h:95 */
    char last_disconnect[32];      /* runtime.h:98 */
    char connected_ssid[33];       /* runtime.h:102 */
    int connected_profile;         /* runtime.h:103 */
    bool internet_verified;        /* = s_wifi.net_verified */
    char net_verify_stage[12];     /* runtime.h:119 */
    app_net_cfg_state_t cfg_state; /* 收编 getter 直读(见 §3-6) */
    /* config 域（一次 app_config_lock 拷贝） */
    char sta_ssid[33];             /* config.h:44 */
    bool password_set;
    uint8_t net_mode;
    bool persistent_use_static;    /* config.h:60 */
    char persistent_static_ip[16]; /* config.h:61 */
    /* mqtt 域（一次 app_mqtt_lock 拷贝） */
    bool mqtt_connected;
    char mqtt_last_error[96];      /* mqtt_runtime_t.last_error */
    /* trial 域（一次 job_lock 拷贝） */
    bool trial_ready;              /* app_cmd_net_trial_ready(NULL) 判定 */
    char trial_id[20];             /* app_cmd_net_trial_t.id 同宽 */
    char trial_ssid[33];
} app_cmd_network_status_snap_t;
```

- **sizeof ≈ 296B**（字段和 292B + 对齐；编译实测写入代码注释并以
  `_Static_assert(sizeof <= 400)` 守卫）；
- 调用任务：ble_cmd（8192B）/ mqtt 事件任务——**净栈增量 ≈ +250B**
  （现函数已有 rssi 段局部 wifi_ap_record_t≈36B+bssid[18] 等，快照替换的
  锁内构建局部为 0）——占两任务栈 3~4%，栈 HWM 对照列入烧录窗口必做项
  （沿用 CMD-04a 口径：低列表不得新现 ble_cmd/mqtt 事件任务，+逐任务
  HWM 若后续暴露则逐一对照）；
- **全部值为拷贝**，零指向 runtime/config/mqtt/trial 内部成员的指针。

## 2. 四段 → 四域映射（现锁内构建 → 快照域）

| 现锁段（行号） | 现锁内构建 | 快照域 |
| --- | --- | --- |
| runtime (:1121-1136) | 8 次 Add（sta_connected/has_ip/sta_ip/last_disconnect/connected_ssid/connected_profile/internet_verified/net_verify_stage） | runtime 域 8 字段 |
| config (:1175-1183) | 5 次 Add（sta_ssid/password_set/mode/persistent_use_static/persistent_static_ip） | config 域 5 字段 |
| mqtt (:1187-1190) | 2 次 Add（mqtt_connected/mqtt_last_error） | mqtt 域 2 字段 |
| job-trial (:1195-1203) | ready 判定 + 3 次 Add（trial_id/state="got_ip"/ssid） | trial 域（ready/ trial_id/ trial_ssid） |

不变段：nc_tcp（netcheck API，自有锁）、rssi 段（已合规模式）、cfg_state
（收编进 runtime 域一次拷贝——现代码在 config 段后经 getter 单独直读，
收编后少一次锁循环，wire 无差异）。

## 3. 判定表（锁内判定 → 锁外等价判定）

| # | 现锁内判定/构建 | 快照字段 | 锁外等价 |
| --- | --- | --- | --- |
| 1 | runtime 8 字段拷贝进 JSON | runtime 域 8 字段 | 直接 Add（值不变） |
| 2 | nc_tcp_fails/errno（netcheck API，自有锁） | API 调用（保持现状时点） | 不变 |
| 3 | rssi 段：connected_snapshot 判定 → get_ap_info / 缺省分支 | `sta_connected`（runtime 域） | 同判定（get_ap_info 仍锁外） |
| 4 | config 5 字段拷贝进 JSON | config 域 5 字段 | 直接 Add |
| 5 | cfg_state 经 getter | cfg_state（runtime 域快照） | 同值（快照时刻略提前，见 §4-skew） |
| 6 | mqtt 2 字段拷贝进 JSON | mqtt 域 2 字段 | 直接 Add |
| 7 | trial_ready(NULL) 判定（job 锁内） | `trial_ready` | 同判定 |
| 8 | trial 块三字段拷贝进 JSON | trial_id/trial_ssid | 同值；state 仍写常量 "got_ip" |

**wire 逐字段一致**：字段名/类型/条件出现/缺省分支全部不变；
`cfg_state` 取值时点变化（getter 直读 → runtime 域快照）不改变取值来源
（同一 runtime 锁内数据），唯一差异 = 快照时刻早于构建时刻（§4-skew）。

## 4. 一致性与 skew 边界

- **域内一致**：runtime 8 字段 + cfg_state 单锁一次拷贝（比现状"8 次 Add
  期间持锁"同一致性）；config/mqtt/trial 各域单锁一致 ✓；
- **跨域 skew**：`sta_ip`（runtime）与 `persistent_static_ip`（config）、
  `mqtt_connected`（mqtt）跨域字段现状即非原子（顺序锁），快照化**不改变**
  skew 特性（捕获顺序与现状相同）；
- **不可能组合防护**：trial 域的 ready/id/ssid 同一次 job_lock 捕获——
  不会出现 ready=true 但 id/ssid 为空的混搭；
- `cfg_state` 收编进 runtime 域说明：原代码在 config 段后经 getter 取值
  （第二次 runtime 加锁），收编后少一次锁循环且与 runtime 域同锁一致。

## 5. 资源预算

| 项 | 现状 | 改造后 |
| --- | --- | --- |
| runtime 锁持有 | 8 次 cJSON 分配 ≈ 300-500µs | 纯拷贝 ≈ 5µs |
| config 锁持有 | 5 次 ≈ 200-300µs | ≈ 3µs |
| mqtt 锁持有 | 2 次 ≈ 100-200µs | ≈ 3µs |
| job 锁持有 | 判定+3 次 ≈ 150µs | ≈ 5µs |
| 调用任务栈 | +快照 296B（净增量 ≈ +250B） | 烧录窗口 HWM 对照（同 04a 口径） |
| 堆 | cJSON 分配总量不变（同字段集） | 不变 |

（估算值；实测列入烧录窗口——沿用 04a 口径。）

## 6. 实施与验收

- 实现：`app_cmd_build_network_status` 改为 `capture() + build()` 两段
  （同 04a 模式）；快照结构 `sizeof` 实测（目标编译器探针）+
  `_Static_assert(sizeof <= 400)`；
- 验收：wire 逐字段一致（golden 基线 `golden_pre_04a.json` 的 network.status
  对照）+ job/runtime/config/mqtt 锁内 grep 零 cJSON + build 0/0 + 主机 8 套；
- 烧录窗口：栈 HWM 对照（同 §1 口径）+ RACE 回归（与 fencing/04a 合并窗口）。
