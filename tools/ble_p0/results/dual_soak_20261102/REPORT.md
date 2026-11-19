# 双设备并发测试 + 6h 长跑记录（2026-11-02）

> **2026-11-02 08:21–14:21 已做唯一 runner 的干净重跑**：BLE 6h PASS（1420/1420、6/6 dual、0 复位），但 TQ2018 在 08:32 消失，Wi-Fi/MQTT 段不签。详见 [REPORT_clean_6h.md](REPORT_clean_6h.md)。本文件保留为首轮历史证据。

## 0. 定位与结论

**定位：`Dual-device 6h engineering soak — PASS with evidence qualification`**

> 这是工程 soak 证据，不是正式 6h 验收 PASS；三件事必须分开看。

1. **设备运行稳定性：PASS（工程观察结论）**——两台 uptime 单调、无运行期复位；heap 首末无漂移；Wi-Fi/MQTT 持续在线。
2. **双设备功能：部分 PASS**——双 BLE 并发、跨设备 token 隔离、双 Wi-Fi/MQTT 共存、双 UART0 长跑已有证据；手机双连接、双设备断网恢复、DATA/UART1 尚未覆盖。
3. **正式 6h 单-run 验收：不签收（NOT SIGNED）**——副 runner `Hn` 与主 runner `V8` 共用同一个 JSONL，聚合证据不能代表一个独立、完整的 6 小时测试会话；A 另有主机侧疑似 BLE 连接问题未定性。

- 主 runner `V8` 跑满 6h：无重启、无 panic/WDT、heap 首末完全一致、Wi-Fi/MQTT 全程在线。
- 首次 dual 尝试失败（BlueZ 残留链路），后续小时级 dual 恢复并 5/5 通过；**不能写成“全程 5/5 无失败”**。
- 工程结论：**稳定性/资源 PASS（工程证据）；正式 6h 单-run 不签；需一次唯一 runner 的干净重跑才能进正式账本**。

---

## 1. 环境

| 项 | A | B |
| --- | --- | --- |
| BLE 名 | `IotHubBLE-C8E4` | `IotHubBLE-65D8` |
| BLE MAC | `B0:A7:32:28:C8:E6` | `10:97:BD:D3:65:DA` |
| 固件 | `1.2-a35eef9` | `1.2-a35eef9` |
| 启动 | `rst:0x1 (POWERON_RESET)` 一次 | 同左 |
| Wi-Fi | TQ2018，`192.168.43.63` | TQ2018，`192.168.43.109` |
| 长跑窗口 | 2026-11-02 00:08–06:08 | 同左 |
| 串口 | UART0 `/tmp/soak_A_20261102.log` | UART0 `/tmp/soak_B_20261102.log` |

验证分支：`test/c1-mainline`，HEAD `a35eef9`（Wi-Fi D1 + 状态命令）。  
本轮不涉及代码修改；不改 `data_pipe`、BLE 重连策略、D 分支参数。

---

## 2. 哪些测试真正需要两台设备同时在场

| 测试 | 为什么必须双设备 | 本轮结果 |
| --- | --- | --- |
| 双 BLE 并发连接 + 跨设备 token 隔离 | 要证明 A 的 token 在 B 上被拒、B 的 token 在 A 上被拒；单设备无法构造 | **PASS（仅限已覆盖的稳态 dual）**：每个 dual 阶段 A→B、B→A 均为 `unauthorized` |
| 双 Wi-Fi/MQTT 共存 | 两台同时在同一 AP/同一 broker 下保持在线，才能看到身份、client_id、topic 是否互串 | **PASS**：6h 状态采样 `sta_connected/mqtt_connected` 全 true，无 MQTT 断开 |
| 双 UART0 长跑取证 | 单台只能看到一台的复位/panic/WDT/heap；双台同时抓才能覆盖并发负载 | **PASS**：两台各只有一次 POWERON，无运行期复位/panic/WDT |
| BLE 角色互换 A/B 双向 | 需要一块板切 central、另一块保持 peripheral | **N/A（本版本不可执行）**：`role_swap.py` 依赖 `/api/v1/config/bluetooth` HTTP 接口，当前固件无 HTTP server；原 soak_test_plan 过程稿已不在当前仓库；HTTP 入口移除后的 N/A 结论仅属当时测试环境 |
| 同一 Android 微信同时连 A/B | 设备侧双链路不等于微信 App 侧并发连接能力 | **NOT RUN**：本轮用 PC BlueZ 做双 central 代理；Android 真机并发未验证，不能替代 T4-A |
| 双设备同时断 Wi-Fi，验证 120s fast-path | 要同时观察两台在 BLE 保持连接时是否只试正式 SSID、120s 一次 | **NOT RUN**：6h 内 Wi-Fi 一直在线；单台 A-policy 已在现场通过，但双设备同时掉线未测 |
| 双设备 MQTT broker 断连/恢复 | 需要同时观察两台的重连退避和 outbox | **NOT RUN**：本轮没有停 broker |
| 双设备 DATA/UART1 长跑 | 需要两台 UART1 都接线，且 DATA 可靠性已立项 | **BLOCKED**：B 的 UART1 未接；DATA 无应用 ACK/序号/重传，当前只签功能不签可靠性 |
| 双设备 OTA 独立性 | 两台同时升级/回滚，验证互不影响 | **不在本轮验收矩阵**；高风险，未安排 |

> 结论：当前可执行、且真正需要双设备的，是本轮已做的 4 类；角色互换因 HTTP 入口删除而不可执行，手机并发、双设备同时断网/MQTT 恢复和 DATA/UART1 仍是缺口。**“双设备功能 PASS”只适用于已覆盖的 4 项，不代表双设备功能整体签收。**

---

## 3. 6h 执行方式与 runner 污染

### 3.1 设计

- 每 20s 顺序做 A、B 各一次短会话：连接 → `auth.login` → `device.status` → `network.status` → 断开。
- 每小时做一次 dual：顺序建立两条 BLE 链路（BlueZ 不支持同一主机并发 `createBLEConnection`），保持两条同时打开 5s，各自登录/查询，然后做 A→B、B→A 的跨设备 token 拒绝。
- 两台 UART0 全程独立采集。

### 3.2 实际 runner

| runner | 启动 | 结束 | 结果 |
| --- | --- | --- | --- |
| `V8` 主 runner | 00:08 | 06:08（6h 窗口结束） | 最后周期行 `cycle=450 elapsed=21133s`；`dual_ok=5 dual_fail=1 seq_ok=882 seq_fail=6 resets=0`；未打印最终 summary |
| `Hn` 副 runner | 00:10 | 06:08 被终止 | `cycle=90 elapsed=2710s`；`dual_ok=1 seq_ok=178 seq_fail=0`；之后挂起，未再输出 |

- 两个 runner 共用 `/tmp/dual_soak_6h_20261102.jsonl`，且都会在启动时 `open(...,"w")`；JSONL 因此交错/截断，**不能作为单一 run 的正式证据**。
- 副 runner 是 `TaskStop` 只停外层 shell、未杀 Python 子进程留下的；不是固件问题，是测试编排问题。
- **主 runner 的统计只取 `V8` 自己的 stdout**：`cycle=450 / 21133s`，`dual_ok=5 / dual_fail=1`，`seq_ok=882 / seq_fail=6`，`resets=0`；不把交错 JSONL 的数字当作 V8 汇总。
- **`Hn` 的 90 cycles 只作为测试卫生记录**：`dual_ok=1 / seq_ok=178`，00:55 后挂起，06:08 终止；不并入 V8 的 6h 统计。
- **双 UART0 各自独立**：A/B 串口记录分别作为两台设备的运行证据，不依赖 JSONL 聚合。

### 3.3 JSONL 计数（仅作旁证，不作 6h 主统计）

- 1100 行：`sequential` 1094 行（1089 ok / 5 fail），`dual` 6 行（全部 ok）。
- 主 runner 首次 dual 失败（BlueZ 残留链路：`InProgress` / `device not found`）和 1 条早期 seq 失败在 JSONL 中缺失，与副 runner 的截断/交错一致；因此不要用 1100 行直接汇总。

### 3.4 统计差异（必须按此口径引用）

| 统计项 | 主 runner `V8` stdout | 交错 JSONL | 报告口径 |
| --- | --- | --- | --- |
| A BLE 连接超时 | **6** | **5** 条保留记录 | 引用 **V8 的 6**；JSONL 少 1 条是双 runner 截断/交错所致 |
| dual 阶段 | **5 通过 + 1 次首次失败**（周期 1 失败；周期 113/183/253/322/393 通过） | 6 行全部 ok | 写成“首次尝试失败，后续恢复并 5/5 通过”；**不得写成“5/5 全程无失败”** |
| 顺序会话 | **882 ok / 6 fail** | 1089 ok / 5 fail | 以 V8 的 882/6 为准；JSONL 仅作旁证 |
| 复位 | **0** | 0 | 一致；V8 `reset_detect` 为 0 |

---

## 4. 6h 结果

### 4.1 复位与崩溃

| 项 | A | B |
| --- | --- | --- |
| `rst:` | 1 次 `POWERON_RESET` | 1 次 `POWERON_RESET` |
| Guru/panic/WDT/abort/brownout | 0 | 0 |
| 运行期新增 core dump | 0 | 0 |
| 启动期 core dump 日志 | `Core dump data check failed`（分区镜像校验失败/空白） | `Core dump data checksum is correct`，发现旧 dump 15,520B |

说明：A 的 `E` 发生在启动 716ms，是 coredump 分区内容无效，不是本次运行崩溃；B 的 15.5KB dump 是烧写前遗留。两台在 6h 内 uptime 单调，未发生第二次启动。

### 4.2 资源

| 设备 | heap free 首/末 | heap min_free 首/末 | 备注 |
| --- | --- | --- | --- |
| A | 43,252 / 43,252 | 33,368 / 33,368 | 6h 完全无漂移 |
| B | 42,868 / 42,868 | 32,904 / 32,904 | 6h 完全无漂移 |

没有 `low heap` / OOM / malloc 失败日志。

### 4.3 Wi-Fi / MQTT

- 1094 条 sequential 采样中 `sta_connected=false` 为 0，`mqtt_connected=false` 为 0。
- A/B 各仅在启动后约 6s 出现一次 `mqtt restart attempt (backoff step 1)`，随后 `MQTT connected`；这是启动顺序，不是 6h 内掉线。
- 运行期 `MQTT_EVENT_DISCONNECTED`、STA 断开日志均为 0；发布计数 A 2,249 / B 2,247。
- A RSSI -66..-54 dBm，B -45..-24 dBm；两台全程在线。

### 4.4 BLE 会话与双设备

- A 550 次 GATT 连接/断开，B 556 次；断开原因均为 `0x213`（对端主动断开），符合短会话设计。
- 主 runner dual 共 6 次尝试：**周期 1 首次失败**（BlueZ 残留链路：`org.bluez.Error.InProgress` / `Device ... not found`），清理旧 link 后，周期 113/183/253/322/393 **5 次通过**；副 runner 另 1 次通过。应写成“首次尝试失败，后续恢复并 5/5 通过”，不能写成“全程 5/5 无失败”。
- 所有成功 dual 的 A→B、B→A 跨 token 均为 `unauthorized`；这证明的是两台设备之间的会话隔离，不只是各自登录成功。
- dual `connect_ms` 17,592–28,789ms（顺序建两条链路，非性能门禁）。

### 4.5 失败与告警

- **A 的 BLE 连接超时**：JSONL 保留 5 条（02:50:53、04:31:38、04:39:34、05:24:19、05:45:04），主 runner 汇总 6 次；均为 `BleakClient.connect` 超时（`connect_ms=None`，未进入登录/查询），B 为 0。设备侧无复位，后续会话恢复。当前只能判为**疑似主机侧 BlueZ 链路问题，待 btmon 确认**；不能直接归类为“BlueZ 已确认故障”。这是本轮不能签 100% BLE 成功的原因之一。
- `ble_cmd: ble cmd rejected: auth_required`：A/B 各 6 次，与 dual 跨设备 token 测试次数吻合，是预期拒绝，不是故障。
- `app_bt: adv flag desync ... (treat host as truth)`：A 34 次 / B 29 次，随后自动恢复；未造成连接失败，保留为非阻塞告警。
- `data_pipe` drop/error：0。

### 4.6 120s fast-path 与 D1

- 串口 `ble fast retry` 共 1,100/1,110 行，归一化后是 **A 550 + B 555 次 link up，A 550 + B 555 次 link down**；B 另有 1 次连接/断开未配对到 fast-retry 日志。它们对应短会话连接/断开，不是 120s 重试循环。
- 6h 内 Wi-Fi 一直在线，未触发 A-policy 的 120s formal-only 快试；`connect_stuck` 为 0，但 D1 watchdog 也没有被实际触发。
- 因此本轮验证的是稳态资源与并发，不替代此前单台的 120s A-policy 现场结论，也不能作为 D1 故障注入证据。

---

## 5. 判定

| 项目 | 判定 | 说明 |
| --- | --- | --- |
| **设备运行稳定性** | **PASS（工程观察结论）** | 两台 uptime 单调、无运行期复位；heap 零漂移；Wi-Fi/MQTT 持续在线 |
| 双设备功能（已覆盖的 4 项） | **PASS** | 双 BLE 并发 + 跨 token、双 Wi-Fi/MQTT、双 UART0；首次 dual 失败后恢复，稳态 5/5 |
| **双设备功能（整体）** | **部分 PASS** | 手机双连接、双设备断网恢复、DATA/UART1 未覆盖，不能写成“双设备功能全部 PASS” |
| 6h 稳定性/资源 | **PASS（工程证据）** | 无复位/panic/WDT，heap 零漂移 |
| **正式 6h 单-run 验收** | **NOT SIGNED（不签收）** | 副 runner 共用/污染 JSONL；A 有疑似主机侧 BLE 连接问题未定性 |
| 120s fast-path / D1 | **未覆盖** | Wi-Fi 未断，watchdog 未触发 |
| 双设备 DATA/UART1 | **BLOCKED** | B UART1 未接；DATA 可靠性未立项 |
| 角色互换 | **N/A（历史入口）** | HTTP 配置入口已删除，§2.5 仅为历史记录 |

**建议**：如果这份 6h 要进正式验收账本，重跑一次**唯一 runner** 的 6h，使用新的 JSONL 文件名和新的双 UART 文件，并在启动前确认 `pgrep -af dual_soak` 只有一份；同时用 btmon 对 A 的超时做定性。当前更值得优先投入的是 T4-A 微信小程序兼容性，而不是重复一次已经提供大量稳定性证据的长跑。

---

## 6. 原始证据

目录：`tools/ble_p0/results/dual_soak_20261102/`

| 文件 | 说明 | SHA-256 |
| --- | --- | --- |
| `primary_runner_stdout.txt` | 主 runner V8 stdout，含 450 周期汇总 | `038bc36bd59d0d2c79017b0422c2258dcdbefb1df0ef9b66c97c58b26b034763` |
| `second_runner_stdout.txt` | 副 runner Hn stdout，含 90 周期后终止 | `836b49493e847e30b39d73bc17345b0dce8fda76eb93f92261abb73546259af7` |
| `soak_A_uart0.log.gz` | A 全 6h UART0（gzip） | `f847729c37d037aa29891867153f7fae7f7eacfd452bdfe4703c3d708e118a3e` |
| `soak_B_uart0.log.gz` | B 全 6h UART0（gzip） | `7b1d209430cea801645112933eedff66f83272e4d2b35292f5c807f0b86685be` |
| `soak_jsonl_contaminated.jsonl` | 两个 runner 交错的 JSONL，仅作旁证 | `c21260c5df287a6eb062f7eb98312463b2ac999d9bf2f10c344793eaebd46f16` |

原始未压缩日志仍在 `/tmp/soak_A_20261102.log`、`/tmp/soak_B_20261102.log`、`/tmp/dual_soak_6h_20261102.jsonl`。  
已检查：日志中没有明文口令，也没有 32 位 token 值；`token` 命中仅为 `cmd session token invalidated`、`token issued (ttl=...)` 和 `token_len`。
