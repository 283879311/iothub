# CMD-05 · config / profile 锁序静态审查（2026-11-15）

> **材料性质：LOCK-ORDER REVIEW（锁序静态审查依据）**
> 本文保存锁清单、调用偏序和递归锁依赖的审查依据；当前任务状态以 [CMD 当前任务台账](cmd_refactor_plan.md) 为准。本文不证明持锁时长、优先级反转或设备运行时行为已经验收。
> **基线**：`main/cmd.c` / `config.c` / `config_apply.c` / `wifi.c`（提交 `04e9584` 时点；CMD-02 已 CODE_DONE）
> **关联**：`docs/validation/cmd_refactor_plan.md` CMD-05；CMD-06 迁移依赖本结论

---

## 1. 锁清单

| 锁 | 实现 | 定义 | 语义 |
| --- | --- | --- | --- |
| **C** `app_config_lock` | 递归 mutex（`app_mutex_ensure(..., true)`） | `config.c:211` | 主配置 `s_config` 与 NVS 读改写 |
| **P** `app_wifi_profiles_lock` | 递归 mutex | `config.c:239` | Wi-Fi profile blob（NVS 独立 key） |
| **W** `app_wifi_runtime_lock` | 递归 mutex | `config.c:225` | Wi-Fi 运行态快照 |
| **J** `s_net_job_lock` | 递归 mutex（cmd.c） | `cmd.c:241` | trial/job |
| **M** `app_mqtt_lock` | 递归 mutex（mqtt.c） | — | MQTT 运行态 |
| 创建保护 | `portMUX` `s_config_locks_mux` | `config.c:209` | 仅首次惰性建锁，**不得**在内再取任何锁 |

三个 config 侧锁都由 `app_mutex_ensure(..., recursive=true)` 创建，`C/P/W` 允许同任务重入——这一点被现有代码依赖（见 §3.3）。

## 2. 锁序结论

**观测到的嵌套关系（持有者 → 新获取）**：

```
J ──► C ──► P
J ─────────► P        （apply：先 C 后释放，再存 profile）
```

| 边 | 证据（行号） | 说明 |
| --- | --- | --- |
| `J → C` | `cmd.c:1119` 持 J → `cmd.c:1137` 内层 `execute_method("network.apply")` → `cmd.c:2851` `config_apply_patch`（`config_apply.c:757` 取 C） | apply 独占期间写配置 |
| `J → C → P` | 同上 → `cmd.c:2818` `app_config_snapshot_save()`（`config.c:931/932` 取 C 后取 P） | 提交前保存回滚快照 |
| `J → P` | 同上，`config_apply_patch` 已释放 C → `cmd.c:2878` `app_config_save_wifi_profile()`（`config.c:771` 取 P） | 提交成功后固化 profile |
| `C → P` | `cmd.c:2238` guard_enter 持 C → `cmd.c:2243` `app_cmd_save_profile_from_params` → `config.c:771` 取 P；另 `cmd.c:3041` delete、`config.c:931/957` 快照 | config.set / profile.delete / 快照 |
| `C → NVS` | `config_apply.c:757–846`（C 内 `app_config_save`） | 见 §3.1 |
| `P → NVS` | `config.c:771–847`、`862–921`、`931–976` | profile blob 读写 |

**未发现的（反向）边**——静态搜索 `main/*.c` 的嵌套获取，均无命中：

- `P → C`、`P → J`、`P → W`、`P → M`
- `C → J`、`C → W`、`C → M`
- `W → P`、`W → C`（`wifi.c` 中对 W 与 P 的取用是**顺序**的：先 unlock 再取另一把，如 `wifi.c:1505–1518`）

> 结论：config/profile 相关锁存在**一致的偏序** `J → C → P`（以及 `J → P`）。
> 只要不引入反向边，就不会形成 ABBA 死锁。这是 CMD-06 迁移必须保持的不变量。

## 3. 关键路径与风险

### 3.1 C 跨越 NVS 写（已知的长持锁）

`config_apply_patch` 在 `config_apply.c:757` 取 C，`memcpy`/校验/`app_config_save()`（NVS commit）全部在锁内，
`config_apply.c:846` 才释放；随后的 `config_apply_domain_apply()` 明确在锁外执行（`config_apply.c:848` 注释
"生效动作在锁外执行:重连/IO 不应持有配置锁"）。

影响：
- `cmd_cfg_txn_guard_enter()`（`cmd_cfg_txn.c:36`）会**在整个 config.set 命令期间持有 C**，期间还包括
  `app_cmd_save_profile_from_params()` 的第二次 NVS 写（P→NVS）——一次命令最多阻塞两段 NVS 时间。
- 任何读 `s_config` 的路径（BLE/MQTT 命令、`config.get`、MQTT attributes）都会在这段时间内排队。
- 与 CMD-14 同类，属**设计取舍**（读-改-写原子性）；本轮只记录，不缩短锁区。
- 建议：后续若做 CMD-14 式测量，应把 `config.set` 的 C 持锁时长一并纳入（含 profile 二次写）。

### 3.2 `J → C` 把网络事务与配置写串在一起

`apply` 持 J 调 `config_apply_patch`（C），再存 profile（P）。后果：
- BLE 断连清理（NimBLE 任务，需 J）与 worker 的 job 快照会等在整段 NVS 之后（见 CMD-02 §1.3 C1 的
  "非原子"边界）；
- CMD-06 若把网络事务迁出 `cmd.c`，**不得**在持有 C/P 时回调网络事务（会形成 `C → J` 反向边）。

### 3.3 递归锁是隐式契约

- `app_config_save_wifi_profile()` 在持 P 时调用 `app_wifi_mark_profiles_dirty()`（`config.c:841`），
  后者再取 P（`wifi.c:815–820`）——依赖 P 递归。
- `app_config_snapshot_restore()` 在持 C+P 时调 `app_config_save()`（再取 C）与 `app_wifi_mark_profiles_dirty()`（再取 P）。
- 结论：**C/P/W 的 recursive 属性是硬约束**，改成普通 mutex 会立即自锁；与 CMD-03 的 J 递归同性质。

### 3.4 相邻发现（不属 CMD-05 结论，勿在本项修）

`config_apply_domain_apply(CFG_DOMAIN_DATAPIPE)` 在 C 内调用 `data_pipe_apply_routes()`
（`config_apply.c:711–715`），而 `data_pipe_set_route()`（`data_pipe.c:122–128`）**不加锁**，
data_pipe 任务读取路由位图时也没有持 C。即"C 保护了写者，但没有保护读者"。
这属数据面并发问题，建议在 CMD-04/数据面专项单独审查，不要并入 CMD-05。

## 4. 对 CMD-06 的约束（迁移时必须保持）

1. 保持 `J → C → P` 与 `J → P` 的既有顺序；禁止出现 `C/P → J`。
2. 不得在持有 P 的路径中调用 `config_apply_*` 或任何取 C 的函数。
3. `config_apply_domain_apply()` 必须在 C 释放后调用（现状即如此，迁移不得合并进锁内）。
4. 不得把 `app_config_lock` / `app_wifi_profiles_lock` 暴露为可写全局状态；对外只经函数接口。
5. C/P/W 的递归属性不得改变；`app_mutex_ensure` 的创建 mux 内不得取任何锁（现状无）。

## 5. 未覆盖 / 待验证

- 本审查为**静态**；未测真实持锁时长、优先级反转、NVS 抖动。
- 未覆盖 `s_config` **无锁读者**（如 Wi-Fi 自动连接读取 `s_config.sta_ssid` 的路径）——属字段所有权问题，
  建议并入 CMD-02 后续或单列。
- 未覆盖 MQTT 侧与 C 的组合（`mqtt.c` 无 `app_config_lock` 调用，配置值应为运行态缓存；需在 CMD-06 前复核）。
- 未执行 IDF 构建与设备回归。

## 6. 结论

- config/profile 锁存在明确的偏序 `J → C → P`，未发现反向边；风险集中在**长持锁**（C 跨 NVS、guard 跨两次 NVS）
  与**递归锁隐式契约**，不是死锁。
- CMD-05 标记 `REVIEWED`；不产生代码修改。CMD-06 启动前必须以本文件 §4 为前置约束。
