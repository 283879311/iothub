# MQTT 运行态与 config 同步复核（2026-11-15）

> **状态：CODE_DONE（R1/R2 已实施；R3/R4/R5 未授权）**（未构建、未上机）
> **基线**：`07b08cc` 时点的 `main/mqtt.c`、`main/config_apply.c`、`main/cmd.c`
> **关联**：CMD-05 §3/§5（"mqtt.c 不取 C 不能证明缓存一致"）；本结论新增 CMD-15 跟踪

---

## 1. 复核问题

CMD-05 观察到 `mqtt.c` 没有任何 `app_config_lock` 调用。这**不能**推出"运行态缓存与配置一致"。
本文件回答三件事：

1. MQTT 从哪些任务上下文读 `s_config`，是否持锁；
2. 配置写入后到运行态切换之间存在哪些**有界陈旧窗口**；
3. 是否存在比"陈旧"更严重的**数据竞争/撕裂读**。

---

## 2. 配置写入链（唯一写入口）

```
config.set
  cfg_txn_guard_enter()            // 持 C（cmd_cfg_txn.c:36）
  run_patch → config_apply_patch   // C 内 memcpy(s_config) + NVS（config_apply.c:757–846）
  save_profile_from_params         // C 内再取 P（config.c:771）
  guard_exit()                     // 释放 C
  config_apply_domain_apply(MQTT)  // 锁外（config_apply.c:848 注释明确）
    app_mqtt_clear_fatal()         // 取 M（mqtt.c:58–63）
    app_mqtt_schedule_restart()    // 通知/创建 restart 任务
```

`network.apply` 走 `J → config_apply_patch(C) → app_config_save_wifi_profile(P)`（见 CMD-05 §2）。

## 3. MQTT 的 `s_config` 读点（全部**未持 C**）

> 行号为 **R1/R2 实施前**（`1165d23`）基线；R1/R2 实施后这些读取已集中到
> `app_mqtt_cfg_snapshot()`（仅该函数在 C 内读 `s_config`），见 §9。

| 位置 | 任务上下文 | 读取字段 | 锁 |
| --- | --- | --- | --- |
| `mqtt.c:192–199` `app_mqtt_build_uri` | restart / boot | `mqtt_use_tls/host/port` | 无 |
| `mqtt.c:202–204` `is_thingsboard_backend` | restart / boot / 状态判断 | `mqtt_backend` | 无 |
| `mqtt.c:285–295` `build_uart_frame_str` | telemetry（属性发布） | `uart_baudrate/data_bits/parity/stop_bits` | 无 |
| `mqtt.c:598–615` 属性 payload（config 镜像） | telemetry | `net_mode/sta_ssid/bt_role/uart_*/relay_power_policy/led_power_policy` | 无 |
| `mqtt.c:651–655` 第二组属性 | telemetry | `bt_role/net_mode/uart_baudrate` | 无 |
| `mqtt.c:1641–1702` `app_mqtt_apply_config` | restart / boot | `mqtt_host/port/net_mode/use_tls/token/backend` | 无 |
| `mqtt.c:1645` 同上 | 同上 | `s_wifi.sta_has_ip` | **无 W** |
| `mqtt.c:600` 属性 payload | telemetry | `s_wifi.sta_ip` | **无 W** |

对照：`cmd.c` 的 `config.get` 在 `cmd.c:1800` 持 C 复制后再构造 JSON——**命令层有锁，MQTT 层没有**。
这就是"当前无直接锁调用"与"缓存一致"之间的差距。

## 4. 有界陈旧窗口（config 已保存 → 运行态真正切换）

| 窗口 | 上界 | 依据 |
| --- | --- | --- |
| W1 调度到 restart 任务 | 毫秒级 | `xTaskNotifyGive` / 建任务（mqtt.c:1829–1862） |
| W2 **defer 合并** | **≤15s** | `APP_MQTT_RESTART_DEFER_MAX_MS 15000`（mqtt.c:1172/1268）；defer 期 `schedule_restart()` 返回 true |
| W3 堆紧张推迟 | **≤5s** | `guard < 25 × 200ms`（mqtt.c:1807–1813） |
| W4 等受理回包 ACK | **≤1.2s** | `APP_MQTT_ACK_GATE_TIMEOUT_MS 1000` + 200（mqtt.c:680/789–801） |
| W5 停旧客户端 | **≤20s**（telemetry 12s + boost 4s + restart 4s） | `mqtt.c:1544/1547/1592` |
| W6 无 STA IP | 直到 `IP_EVENT_STA_GOT_IP` | `apply_config` 提前返回 `waiting_sta_ip`（mqtt.c:1645–1649）；GOT_IP 回调再调度（mqtt.c:1950–1953） |

**最坏上界 ≈ 41s**（W2+W3+W4+W5），实际通常远小于此。

窗口内的可观测不一致：

- `config.get` / 属性 payload 报**新**配置；MQTT 实际仍连**旧** broker（`s_mqtt.connected` 仍是旧连接）。
- `app_mqtt_restart_is_pending()` / `is_deferred()` 只在 `operation.c:1030–1071` 打印日志，**没有暴露到 `network.status`/`config.get`**；客户端无法区分"已生效"与"待重启"。
- `config.set` 的 MQTT 域 `effect=subsys`（config_apply.c:457–470），语义为"子系统重启生效"，但响应里没有"尚未完成"的标志。

## 5. 数据竞争（比陈旧窗口更严重，P1）

`config_apply_patch` 在 C 内 `memcpy(&s_config, &s_next, sizeof(s_next))`（config_apply.c:843），
而 MQTT 的 telemetry / restart 任务**无 C** 读同一结构体：

- 撕裂读：`mqtt_host`/`mqtt_token`/`sta_ssid` 等字符数组可能读到"半新半旧"内容，`snprintf` 取不到 NUL 时会截断到旧尾部；
- `mqtt_port`/`mqtt_use_tls` 等标量可能读到非预期组合（如 new host + old port）；
- 属 C11 数据竞争（UB），不是"读到旧值"这么温和。

`credentials.username = s_config.mqtt_token`（mqtt.c:1689）把**全局缓冲指针**交给 esp-mqtt。
若该 IDF 版本的 `esp_mqtt_client_init` 不复制字符串，则后续 config 写入会直接改写在用凭据。
**本轮无法从仓库证明 esp-mqtt 的拷贝语义**，需在构建/实测阶段确认（列入未覆盖）。

另外 `s_wifi.sta_has_ip`/`sta_ip` 在 mqtt.c 中无 W 读（1645/600），与 Wi-Fi 事件任务的 W 写构成同类竞争。

## 6. 建议（设计级，均未实施）

| ID | 建议 | 优先级 |
| --- | --- | --- |
| R1 | `app_mqtt_apply_config` 在 C 内**短快照**所需字段到局部（host/port/tls/token/backend/net_mode），释放 C 后再 stop/init；不得把 esp-mqtt 调用移进 C | P1 |
| R2 | telemetry 的属性 payload 改为读取同一份快照（或持 C 仅复制字段），避免与 `memcpy(s_config)` 并发 | P1 |
| R3 | 在 `network.status` 暴露 `mqtt_restart_pending` / `deferred`（复用现有只读函数），让客户端区分"已生效/待重启" | P1 |
| R4 | `s_wifi.sta_has_ip`/`sta_ip` 读取改为 W 内快照 | P2 |
| R5 | 构建阶段确认 esp-mqtt 是否复制 `credentials.username`；若不复制，改为传局部缓冲 | P2（需构建） |

**不建议**把上述改动并入 CMD-06 迁移；它们是 MQTT 模块自身的读取边界问题，应独立评审与授权。

## 7. 未覆盖 / 待验证

- 未实测窗口长度（W2–W5 为代码上界，不是观测值）。
- 未验证 esp-mqtt 配置字符串拷贝语义（需 IDF 构建或源码核对）。
- 未覆盖 TB RPC 路径中 `config.set` 与 `network.apply` 并发（cfg_txn 串行化命令，但 restart 任务独立）。
- 未覆盖 `app_mqtt_stop()` 在 Wi-Fi 事件任务内同步执行（最坏 ~20s 阻塞事件任务）——属既有已知问题，建议单列。

## 8. 结论

- **存在陈旧窗口**：config 已保存、`config.set` 已返回成功，运行态最坏可在 ~41s 后才切换；窗口内状态/属性读数与真实 broker 不一致，且无 API 字段可观测。
- **存在数据竞争**：MQTT 三个任务无锁读 `s_config`，与 C 内整结构 memcpy 并发；这不是"缓存陈旧"而是撕裂读风险，优先级高于陈旧窗口。
- CMD-05 的正确解读应是"未发现锁序反向边"，**不是**"MQTT 配置一致性已验证"。本文件补上后者，跟踪项记为 **CMD-15**（REVIEWED，修复待授权）。


---

## 9. 实施记录（2026-11-15，R1/R2，`CODE_DONE`）

提交：`999db00fd34d8b8696ae49e8a163c082c891f584`（仅 `main/mqtt.c`）。

| 项 | 落点 | 内容 |
| --- | --- | --- |
| 快照结构 | `app_mqtt_cfg_snap_t` + `app_mqtt_cfg_snapshot()` | 持 C 复制 host/token/backend/sta_ssid/port/uart_*/net_mode/bt_role/relay/led 策略；锁内无其他调用 |
| R1 | `app_mqtt_apply_config` | 停旧客户端后取快照；host/port/tls/token/backend/net_mode 全部用快照；`build_uri(&cfg)` |
| R1 附带 | backend 热路径 | `s_backend_is_tb` 原子缓存（apply_config 内更新）；8 处 topic/RPC 读取不再碰 `s_config` |
| R1 附带 | `credentials.username` | 模块静态 `s_token_snap`（stop 后、init 前刷新），不交局部地址、不指向会被改写的 `s_config.token`；R5 未验证前对两种 esp-mqtt 语义都安全 |
| R2 | `publish_attributes` / `publish_state` | 各取一次 C 内快照，锁外构造 JSON；`build_uart_frame_str(out, size, &cfg)` |

**约束核对**：C 仅复制；字段覆盖 R1/R2 路径全部配置项；局部快照生命周期覆盖使用点、交给 esp-mqtt 的指针为静态副本；defer/重启/退避语义未改；全量 grep 后 `s_config.` 仅存在于快照函数内（持 C）。

**静态验证**：`git diff --check` 通过；`python3 tests/syntax_check.py --strict main/mqtt.c` OK（1 通过 / 0 失败）。

**未实施（未授权）**：R3 pending 暴露、R4 `sta_has_ip/sta_ip` 的 W 快照、R5 esp-mqtt 拷贝语义确认。`s_wifi.sta_has_ip`/`sta_ip` 仍为无 W 读。

**为何不是 `PASS`**：未运行 ESP-IDF 构建与设备回归；数据竞争的运行时效果（撕裂读概率、MQTT 属性一致性）尚无实测证据。


---

## 10. R4 最小修复设计（Wi-Fi 状态快照，**待授权**）

> 本轮只出设计，未改代码。R1/R2 的授权不延伸到 R4。

### 10.1 问题范围（`mqtt.c` 当前全部 `s_wifi` 读点）

| 行 | 上下文 | 读取 | 现状 |
| --- | --- | --- | --- |
| 653 | `publish_attributes`（telemetry 任务） | `s_wifi.sta_ip` | 无 W |
| 709 | `publish_state`（telemetry 任务） | `s_wifi.sta_has_ip` | 无 W |
| 1451 | `MQTT_EVENT_DISCONNECTED`（esp-mqtt 任务） | `s_wifi.sta_has_ip` | 无 W（M 已于 1446 释放） |
| 1709 | `app_mqtt_apply_config`（restart/boot） | `s_wifi.sta_has_ip` | 无 W |

写者：wifi 事件/驱动路径在 `app_wifi_runtime_lock`（W）内更新 `s_wifi`（如 `wifi.c` 的 STA 事件处理）。
`W` 为递归 mutex（`config.c:225–232`）。

### 10.2 方案

在 `mqtt.c` 内新增**只读快照**，与 R1/R2 的 config 快照同构：

```c
typedef struct {
    bool sta_has_ip;
    char sta_ip[16];
} app_mqtt_wifi_snap_t;

static void app_mqtt_wifi_snapshot(app_mqtt_wifi_snap_t *snap)
{
    app_wifi_runtime_lock();
    snap->sta_has_ip = s_wifi.sta_has_ip;
    app_copy_string(snap->sta_ip, sizeof(snap->sta_ip), s_wifi.sta_ip);
    app_wifi_runtime_unlock();
}
```

替换四处：

1. `publish_attributes`：config 快照之后取 wifi 快照，`sta_ip` 用 `wifi.sta_ip`；
2. `publish_state`：同上，`sta_has_ip` 用 `wifi.sta_has_ip`；
3. `MQTT_EVENT_DISCONNECTED`：在 `is_current_client` 判定（M 已释放）之后取快照，
   `if (is_current_client && wifi.sta_has_ip)`；
4. `app_mqtt_apply_config`：config 快照之后、`waiting_sta_ip` 判定之前取快照。

### 10.3 锁序约束（关键）

- **C 与 W 顺序取用、绝不嵌套**：`app_mqtt_cfg_snapshot()` 返回时 C 已释放，再取 W。
  CMD-05 的偏序审计只覆盖 `J→C→P`，**未审计 C↔W**；本设计不新增该边。
- **保留既有 `M→W`**：`mqtt.c:1411–1415` 在持 M 时调 `app_wifi_link_generation()`
  （`wifi.c:2823–2831` 取 W），这是既有顺序；新增四处取 W 时**均未持有 M**，因此不会出现 `W→M`。
- 快照函数内只复制两个字段；不调用 cJSON、不发布、不阻塞。

### 10.4 明确不做

- 不把 Wi-Fi 字段并入 `app_mqtt_cfg_snap_t`（那会形成 `C→W` 新嵌套边，需另行审计）；
- 不改变 `waiting_sta_ip` 早退语义、不改 `s_mqtt.connected_gen` 的取代次时机；
- 不触碰 R3（pending 暴露）与 R5（esp-mqtt 拷贝语义）。

### 10.5 验收与未决

- 实施后最多 `CODE_DONE`：`git diff --check`、`syntax_check --strict main/mqtt.c`、全量 `grep s_wifi.` 复核；
- `PASS` 需 ESP-IDF 构建 + 设备回归（未授权）；
- **未决**：W 的持有时长未测。`publish_attributes/state` 每次取一次 W（连接建立时各一次，
  IO 变化与遥测周期各触发一次），若未来 W 持有者出现长操作，MQTT 发布会被牵连——建议
  纳入 CMD-14 式测量，而不是在本补丁里猜。
