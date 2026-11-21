# WIFI-OP-RESOURCE-RACE / CMD-01 设备复测记录（2026-11-22）

> 文档状态：**CURRENT（执行记录）**。本文只记录本轮设备验证的实际结果与证据，**不构成状态变更**：
> `CMD-01` 仍为 `TEST_PENDING`；`WIFI-OP-RESOURCE-RACE` 不作为已关闭项；`PROTO-ID-01` 记录不受影响。

## 1. 环境与镜像

| 项 | 值 |
|---|---|
| 镜像 | `1.3-1ff92db`（代码 `1ff92db` + 测试补充 `257e394`） |
| `build_sha` | `32b10dfa5` |
| 镜像完整 SHA-256 | `bbd343b0d3463876fdaa1eb44a2abe90ba7a1c9c84ea6f29690d06e8b9c3f9e0` |
| 设备 | IotHubBLE-65D8，MAC `10:97:bd:d3:65:d8` |
| 烧录 | 本轮**未烧录**（沿用 2026-11-22 M8 窗口的镜像） |
| 测试机 | Ubuntu，`.venv-bleak`（bleak 3.0.2 / paho-mqtt 2.1.0） |
| BLE | PC 单 central，MTU 23，20B 分片；测试期间手机不连接 |
| 串口 | `/dev/ttyUSB0` 115200，单进程采集（每例一个文件） |
| 网络 | `sta` + DHCP，已连 `CMCC-502`，`sta_ip=192.168.1.45` |

**末态回读**（2026-11-22，`device.status`）：

```
uptime_ms=20714197  fw=1.3-1ff92db  build_sha=32b10dfa5
sta_connected=true  sta_ip=192.168.1.45
mqtt.connected=true  mqtt_host=139.9.93.245  mqtt_port=21883
```

## 2. 结果总览

| # | 项目 | 结果 | 说明 |
|---|---|---|---|
| 1 | RACE-01 | **PASS** | auto_connect（`tryconnect_finish` 400ms 任务）在 TRYX 持有期间被拒 |
| 2 | RACE-02 | **PASS** | 新 op 等到 `cleanup complete` 后才取得 TRYX，终态 `ap_not_found` |
| 3 | RACE-03 | **PASS** | CLEANUP 期间 auto 连接 0 次、被拒 2 次；完成后才连接 |
| 4 | #12 回归 | **PASS** | `busy → operation_state → cancelled×2 → ap_not_found`；`wifi_scan_failed=0` |
| 5 | CMD-01 MQTT `io.get` | **PASS** | 临时指向 1883 → 八字段齐全 → 恢复 21883 并核验 |
| 6 | CMD-01 一致性对读 | **PASS** | `io.net_mode` vs `network.status.mode` 等三字段一致 |

## 3. RACE-01 · auto_connect 与 tryconnect 争抢

### 3.1 执行方案偏离（必须记录）

原设计步骤使用**不存在的 SSID** 触发 tryconnect。实际执行发现：预扫描在拆旧链之前，目标 SSID 不存在时不会拆链，也就没有 `STA_DISCONNECTED` 事件，auto_connect 不会被触发，无法形成竞争窗口。

**调整后的刺激条件**：改用**在场 SSID `CMCC-502` + 错误口令**，使 tryconnect 进入拆旧链→连接失败路径；并在串口检测到 `op_owner: TRYX -> NONE` 的瞬间，以无响应写（write-without-response）抢发第二个 tryconnect。

**为什么仍覆盖原验收目标**：竞争双方没有改变——一方是 `tryconnect_finish` 排队的 400ms auto_connect 任务（生产路径中真实存在的 auto 触发源，见 Q1.4），另一方是新 TRYX。调整只改变了“如何让 auto 任务与 TRYX 重叠”，没有改变被测资源仲裁逻辑。**这是执行步骤偏离，不是判据放宽。**

### 3.2 实际时序（boot ms）

```
20642383  op_owner: NONE -> TRYX by tryconnect_begin      ← op A
20647233  op_owner: TRYX -> NONE by tryconnect_release    ← A 释放（拆链失败后恢复）
20647503  op_owner: NONE -> TRYX by tryconnect_begin      ← op B（抢入）
20647653  auto_connect: schedule refused caller=tryconnect_finish reason=owner_busy gen=19
20652513  op_owner: TRYX -> NONE by tryconnect_release    ← B 释放
```

- 拒绝发生在 B 持有 TRYX 期间，且仅 150ms 后；
- B 释放前无 `auto_connect: fast_path` / `selected saved profile`（auto 连接 0 次）；
- op 终态 `failed/ap_not_found`。

### 3.3 判定

PASS。失败判据（auto 在 TRYX 持有时成功发起连接、或两段 `run -> init` 交叠）均未出现。

## 4. RACE-02 · cancel 后立即新 tryconnect

### 4.1 实际时序

```
20658763  op_owner: NONE -> TRYX by tryconnect_begin      ← op A
20663603  op_owner: TRYX -> CLEANUP by tryconnect_release ← cancel 后移交
20664183  cleanup complete op=op-60FD2FD22312572F attempts=1
20664193  op_owner: NONE -> TRYX by tryconnect_begin      ← op B 取得
```

- B 在 `cleanup complete` **之后 10ms** 取得所有权；
- 全程无 `scan: start failed: ESP_ERR_WIFI_STATE`；
- op B 终态 `failed/ap_not_found`，无 `wifi_scan_failed`。

### 4.2 判定

PASS。失败判据（`busy` 收尾、`wifi_scan_failed`、cleanup 未完成即取得 TRYX）均未出现。

## 5. RACE-03 · cleanup 恢复与 auto_connect 同时

### 5.1 实际时序

```
20672573  op_owner: NONE -> TRYX by tryconnect_begin
20677423  op_owner: TRYX -> CLEANUP by tryconnect_release
20677423  auto_connect: schedule refused caller=tryconnect_finish reason=owner_busy
20677773  auto_connect: schedule refused caller=apply_start_released reason=owner_busy
20677793  cleanup complete op=op-6A67513DFD87354F attempts=1
（之后）  auto_connect: task run → fast_path ssid=CMCC-502
```

### 5.2 判定

PASS（按复核确认的收窄口径）：CLEANUP 期间可归因 auto_connect 的连接动作 **0** 次，拒绝 **2** 次；`cleanup complete` 之后才出现 auto 连接。cleanup 自身的 `esp_wifi_stop/start` 不计入抢占。

## 6. #12 回归

序列（boot ms，独立采集窗口）：

```
2094203  op_owner: NONE -> TRYX by tryconnect_begin
2099053  op_owner: TRYX -> CLEANUP by tryconnect_release
2099633  cleanup complete op=op-382178F59938561C attempts=1
2100463  op_owner: NONE -> TRYX by tryconnect_begin   ← 新 op
```

响应序列：

| 步骤 | 结果 |
|---|---|
| 第二次 tryconnect | `busy` |
| early apply | `operation_state` |
| cancel / 重复 cancel | `cancelled` / `cancelled` |
| 新 op 终态 | `failed/ap_not_found`（`finished_uptime_ms=2104691`） |
| `scan_state_fail` / `wifi_scan_failed` | 0 / 0 |

判定：PASS。

## 7. CMD-01 · MQTT `io.get` 与一致性对读

### 7.1 一致性对读（BLE）

字段映射按契约：`io.get.net_mode` ↔ `network.status.mode`；其余同名。

| 字段 | io.get | network.status |
|---|---|---|
| mode | `sta` | `sta` |
| sta_has_ip | `true` | `true` |
| sta_ip | `192.168.1.45` | `192.168.1.45` |

判定：PASS。

### 7.2 MQTT `io.get`

| 步骤 | 结果 |
|---|---|
| 前置配置 | `139.9.93.245:21883` |
| `config.set` 临时指向 1883 | `success=true`，`changed=["mqtt_port"]`，`effect=subsys` |
| 设备接入 | `mqtt_connected=true` |
| MQTT `io.get` | 八字段齐全，`net_mode/sta_has_ip/sta_ip` 与 BLE 一致 |
| 恢复原配置 | `139.9.93.245:21883` |
| 恢复后核验 | `config.get` 端口 21883；`device.status.mqtt.connected=true` |

判定：PASS。

## 8. 过程异常（四项，逐项记录）

### 8.1 RACE-01 刺激条件调整

见 §3.1。首次两次尝试（不存在 SSID）均未产生竞争：`refused=[]`、`auto连接(释放前)=1`。调整为在场 SSID + 错误口令后通过。**属执行方案偏离，已在结果中明确标注。**

### 8.2 RACE-02 首判取错事件

首版判定用 `text.find("NONE -> TRYX")` 取到的是 op A 的取得事件（在 cleanup 之前），导致 `cleanup顺序=False`。修正为从 `cleanup complete` 之后查找 op B 的取得事件。设备日志本身顺序正确；修正后 PASS。

### 8.3 CMD-01 一致性首判字段名错误

首版用 `net_mode` 去比较 `network.status`，而该接口字段名为 `mode`，取到 `None` → FAIL。按契约映射重跑 PASS。**FAIL 为脚本字段名错误，不是设备不一致。**

### 8.4 RACE-03 之后 BLE 链路断开

RACE-03 用例本身已判定完成；之后 `wait_sta` 期间 BLE 报 `ConnectionLost`。串口显示 Wi-Fi 正常重连、`IP_EVENT_STA_GOT_IP` 恢复，随后 `device.info`/`device.status` 可正常读取；末态 uptime 连续（20.71M 与 run4 末条连续），**未见重启**。

结论（执行方判断，未独立证明根因）：疑似 Wi-Fi 重启期间的 BLE 监督超时；`#12` 另起连接完成，不受影响。

## 9. 证据清单（完整 SHA-256）

路径：`artifacts/wifi_race_20261122/`

| 文件 | SHA-256 |
|---|---|
| `race_results.json` | `c5b3dc216d858bfd23846abce62825e6c97edd1ba049b8407d609e0bb23e9c97` |
| `issue12_results.json` | `10531698c495ff9507d93df36550a0f54a7865ad8f2ae8b0e74c07a910a438d8` |
| `cmd01_results.json` | `5d53035ec08ebc1262a5e05d99f08fac5f96a6fe03293f2fe30c2d5df723b8fb` |
| `cmd01_consistency.json` | `546f4d516ab35981fd84b7f2dc0bd589017daf4519d1525e45c2f6d08a2f70ba` |
| `race01_serial.log` | `0de78c60d0033c6b6b57c7f430c6d64019ca6272182fc5a28821d078786f9f28` |
| `race02_serial.log` | `768ad35fc014819c069550357b873e59dfd475fd7d9f795272ebdb512d8c4e8d` |
| `race03_serial.log` | `4171421db25cf0a168378ff74491570ed10ceb5d6b278bc721b57440f781f45e` |
| `issue12_serial.log` | `ea6213041dba0caaaa32868564519b7a0094d551f5d167ce2d87476a8185d141` |
| `cmd01_serial.log` | `83d2acf6a0c06f129b6e5d82ffea73f0220fa152134d5f33235260dbbac02c55` |

说明：

- `race_results.json` 为 RACE 三项最终一次执行的快照（此前一次执行因 §8.2 判定缺陷未写入完整结果）；
- `cmd01_results.json` 同时保留了 §8.3 的首跑 FAIL 记录（脚本字段名错误）与 MQTT 全流程 PASS；
- 串口时间戳为 boot ms：RACE 三项为 20.64M–20.69M 窗口，#12 为 2.09M 窗口；两者相隔约 5 小时，**中间空档未做连续监控**，只以末态 uptime 连续作为“无重启”的弱证据。

## 10. 边界

- 本文为**执行方采集与整理**；评审方是否独立复跑/校验哈希，以评审记录为准，本文不代替独立复核。
- 本轮**未修改**代码、测试脚本、Contract；**未重新烧录**；只按授权临时修改过 `mqtt_port` 并已恢复。
- **不改变任何状态**：`CMD-01` 仍 `TEST_PENDING`；`WIFI-OP-RESOURCE-RACE` 未关闭；`PROTO-ID-01` 保持已关闭。
- RACE 与 CMD-01 的状态变更，待评审复核本文与证据后再单独决定。
