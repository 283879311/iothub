# Clean single-run 6h dual-device soak（2026-11-02）

## 0. 结论

| 项目 | 判定 | 说明 |
| --- | --- | --- |
| **干净单-run BLE 6h** | **PASS** | 唯一 runner、唯一 JSONL、单 summary；1420/1420 单设备会话、6/6 dual、0 复位、0 BLE 超时 |
| **双设备 BLE 并发 + 跨 token** | **PASS** | 6 个 dual 全过；A→B/B→A 共 12 次全部 `unauthorized` |
| **双 Wi-Fi/MQTT 共存 6h** | **NOT SIGNED** | TQ2018 于 08:32:20 左右消失；两台 Wi-Fi/MQTT 只在线约 10 分钟 |
| **完整双设备 6h（BLE+Wi-Fi+MQTT）** | **NOT SIGNED** | 本轮 BLE 干净但 AP 丢失；首轮 Wi-Fi/MQTT 6h 但 BLE 有超时。两轮各自只覆盖一部分 |
| **120s fast-path / D1** | **未覆盖** | 短 BLE 会话反复 link up/down，120s 计时器被重置；`cycle=0`，D1 未触发 |
| **角色互换** | **N/A** | HTTP 配置入口已删除（见 `docs/soak_test_plan.md` §2.5） |

> 定位：`Clean single-run BLE 6h soak — PASS`；`Full dual-device 6h soak — NOT SIGNED`。  
> 这不是把首轮和本轮拼成一个 PASS：正式验收仍要求同一独立 run 内 Wi-Fi/MQTT 与 BLE 都稳定。

---

## 1. 运行完整性

| 项 | 值 |
| --- | --- |
| 固件 | `1.2-a35eef9`，A/B 相同 |
| 串口映射 | A=`/dev/ttyUSB1` (`b0:a7:32:28:c8:e4`)；B=`/dev/ttyUSB0` (`10:97:bd:d3:65:d8`)，`esptool chip_id` 核对 |
| 预检 | A/B 均登录并 `device.status` PASS |
| runner 启动 | 2026-11-02 08:21:02 |
| summary | 2026-11-02 14:21:04，`elapsed_s=21601` |
| cycles | 716 |
| 单设备会话 | 1420（A 710 / B 710），`seq_ok=1420 / seq_fail=0` |
| dual | 6，`dual_ok=6 / dual_fail=0` |
| 复位 | `resets=0` |
| 证据行 | 1427：1420 sequential + 6 dual + 1 summary；无交错、无第二 runner |

启动前用 `esptool chip_id` 复位并核对 MAC；因此首条 status 的 uptime 约 30s，属于干净的 POWERON 基线。  
A/B UART0 从 08:20:38 开始采集，结束时由编排脚本统一停止。

---

## 2. BLE 6h 结果

- **连接成功率**：1420/1420，0 `TimeoutError`；单次 `connect_ms` 最大 1923ms。
- **dual**：6/6 通过，`connect_ms` 1645–3915ms（顺序建两条链路）。
- **跨设备 token**：每个 dual 做 A→B、B→A，共 12 次全部 `unauthorized`。
- **UART0**：A/B 各只有一次 `rst:0x1 (POWERON_RESET)`；无 Guru/panic/WDT/abort/brownout；无新增 core dump；`data_pipe` drop/error 为 0。
- **BLE 会话**：A/B 各 717 次 central connect / 717 disconnect（710 单设备 + 6 dual + 1 预检）；断开原因均为对端主动断开。
- **告警**：`auth_required` A/B 各 6 次，对应 dual 跨 token 拒绝，预期；`adv flag desync` A 67 / B 69 次，自恢复，未造成连接失败。
- **启动期 core dump**：A 仍为分区镜像校验失败；B 为烧写前遗留的 15,520B dump；不是本轮运行崩溃。

### heap

| 设备 | heap free 首/末 | free 范围 | min_free |
| --- | --- | --- | --- |
| A | 43,264 / 60,428 | 39,652–60,476 | 33,184（全程恒定） |
| B | 42,856 / 60,032 | 41,284–60,032 | 32,616（全程恒定） |

free 在 Wi-Fi/MQTT 停止后升高，属于负载下降，不是泄漏；`min_free` 没有下降趋势。

### 运行期 WiFi/MQTT

- 在线窗口：A 08:21:48–08:31:47，B 08:21:54–08:31:54；合计只有 20/710 条 `sta_connected=true`，MQTT true 18/19 条。
- 08:32:20 起两台先后变为 `sta_connected=false`；`last_disconnect` 以 `201(NO_AP_FOUND)` / `no_saved_ap` 为主。
- 串口明确记录：`profile[0] 'TQ2018' NOT in scan (AP off / hidden / out of range?)`；之后 `STA disconnected, stopping MQTT client` 共 A 1422 / B 1358 次。
- 这是 **AP 消失**，不是设备复位：两台 uptime 继续单调，BLE 命令/状态一直可用，无 `connect_stuck`。

> 因此本轮不能写“双 Wi-Fi/MQTT 共存 6h PASS”。首轮 6h 的 Wi-Fi/MQTT 6h 证据仍在，但那轮 BLE 有 6 次 A 侧疑似主机连接超时；两轮不能拼接成正式 PASS。

### 120s fast-path / D1

- `ble fast retry: cycle=`：A/B 均为 **0**；`first attempt in 120000ms` 分别 695/694 次。
- 每次短 BLE 会话的 link up/link down 都重置 120s 计时器，所以计时器从未走满 120s；A-policy 的实际重试没有被本轮触发。
- `STA down with BLE connected: event-chain retry suppressed`：A 12 / B 57 次，说明抑制逻辑在工作；`connect_stuck` 为 0，但 D1 watchdog 也没有被实际触发。
- 这是一个设计边界：短会话/频繁重连会不断重置 fast-path 计时器；正常“BLE 保持连接”场景不受影响，但不能用本轮证明 120s 重试。

---

## 3. 与首轮（08:21 前的 6h）对比

| 项 | 首轮 00:08–06:08 | 干净重跑 08:21–14:21 |
| --- | --- | --- |
| 证据卫生 | 两个 runner 共用 JSONL，污染 | **唯一 runner / 唯一 JSONL / 单 summary** |
| 单设备 BLE | 882 ok / 6 A 侧超时 | **1420 ok / 0 超时** |
| dual | 首次失败，后续 5/5 | **6/6 全过** |
| Wi-Fi/MQTT | 6h 在线 | 约 10 分钟后 AP 消失 |
| 正式完整 6h | 不签 | **BLE 可签；Wi-Fi/MQTT 段不签** |

结论：**BLE 6h 的干净证据已经拿到；完整双设备 6h 仍缺一次 AP 稳定、BLE 无超时的同一 run。**

---

## 4. 证据

目录：`tools/ble_p0/results/dual_soak_20261102/clean_run/`

| 文件 | 说明 | SHA-256 |
| --- | --- | --- |
| `soak_clean.jsonl` | 1427 行，单 runner，含 summary | `1c9eaff5940bfc8a29611d202e7f6b88c7c311873983cd477b4572136141d183` |
| `soak_clean.stdout.txt` | 周期汇总 + runner stdout | `2c96746c79f5dfe6764c74834518409aa213062f575d78a1d24e75c62d56699b` |
| `preflight.txt` | A/B 预检 PASS | `8e1636750050c10adc66892f3545733982521d00c394aef554ab783b3a2783c5` |
| `A_uart0.log.gz` | A 全窗口 UART0（gzip） | `5d477a7f13f467bf93b2d20995e7c1c67d2386ef2bbba3a6a2566deebf000392` |
| `B_uart0.log.gz` | B 全窗口 UART0（gzip） | `e6824dcfef67df61142e722088572eb55f0e84a1843ff7db6413efb21d168054` |

原始未压缩文件：`/tmp/dual_soak_6h_clean_20261102.jsonl`、`/tmp/soak_A_clean_20261102.log`、`/tmp/soak_B_clean_20261102.log`。  
已检查：无明文口令、无 32 位 token 值；日志中的 `token` 仅为登录/失效提示。

---

## 5. 下一步

1. **若只需 BLE 稳定性**：本报告可签“干净 BLE 6h PASS”，不再重跑。
2. **若要完整双设备 6h**：先确认 TQ2018 热点保持常开（关闭省电/休眠、确认客户端数），再跑一次唯一 runner 的 6h；否则重复会出现同样的 AP 丢失。
3. 120s fast-path 要用**持续 BLE 连接 ≥120s**的场景单独验证；本轮的短会话会把计时器重置。
4. 角色互换仍 N/A；#13/#14/#25 仍缺明确验收条件，不猜。
