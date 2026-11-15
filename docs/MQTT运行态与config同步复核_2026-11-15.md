# MQTT 运行态与 config 同步复核（2026-11-15）

> **状态：REVIEWED**（只读复核；未改代码、未构建、未上机）
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
