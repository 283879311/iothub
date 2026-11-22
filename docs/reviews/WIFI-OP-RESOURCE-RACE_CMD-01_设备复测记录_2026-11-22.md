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

原设计步骤：用**不存在的 SSID** 发起 tryconnect，期望 `STA_DISCONNECTED` 事件触发 auto_connect，与新 TRYX 竞争。

实际执行分三次尝试：

| 尝试 | 刺激条件 | 结果 | 原因 |
|---|---|---|---|
| 1 | A、B 均为 `__ble_selftest_absent__`，B 用普通带响应写 | 无竞争：`refused=[]`，auto 正常连接 | 目标不存在时预扫描在拆链之前，不产生断连事件；且普通写在 400ms 内送不到 |
| 2 | A 改为 `CMCC-502` + 错误口令（制造拆链），B 仍为 absent | 出现 `run->init`，但 auto 任务先于 B 取得所有权 | B 的带响应写耗时超过 400ms 窗口 |
| 3（最终） | **A、B 均为 `__ble_selftest_absent__`**；串口检测到 `TRYX -> NONE` 的瞬间，用**无响应写**（write-without-response）抢发 B | 竞争成立 | B 在 A 释放后 270ms 取得 TRYX，400ms 任务运行时被拒 |

**最终采用的偏离点**：不是更换 SSID，而是**触发方式**——用串口时序做触发、用无响应写压缩送达时间，让 B 落在 400ms 窗口内。

**为什么仍覆盖原验收目标**：竞争双方没有改变——一方是 `tryconnect_finish` 排队的 400ms auto_connect 任务（生产路径中真实存在的 auto 触发源，见 Q1.4），另一方是新 TRYX。**这是执行步骤偏离，不是判据放宽。**

**A / B 输入与终态**：

| op | SSID | 口令 | 终态 |
|---|---|---|---|
| A | `__ble_selftest_absent__` | `not-a-real-password-0` | `failed/ap_not_found` |
| B | `__ble_selftest_absent__` | `not-a-real-password-0` | `failed/ap_not_found` |

### 3.2 实际时序（boot ms）

```
20642383  op_owner: NONE -> TRYX by tryconnect_begin                 ← op A
20647233  tryconnect: ssid '__ble_selftest_absent__' not in scan results
20647233  op_owner: TRYX -> NONE by tryconnect_release               ← A 释放
20647243  auto_connect: scheduled caller=tryconnect_finish delay=400ms owner=NONE gen=19
20647503  op_owner: NONE -> TRYX by tryconnect_begin                 ← op B（270ms 后抢入）
20647653  auto_connect: task run caller=tryconnect_finish delay_actual=410ms
20647653  auto_connect: schedule refused caller=tryconnect_finish reason=owner_busy gen=19
20652503  tryconnect: ssid '__ble_selftest_absent__' not in scan results
20652513  op_owner: TRYX -> NONE by tryconnect_release               ← B 释放
20652513  auto_connect: scheduled caller=tryconnect_finish delay=400ms op=op-60EF…
```

关键：`scheduled`(20647243) → `task run`(20647653, delay_actual=410ms) → `refused` 同一毫秒，证明是**延迟任务运行后**被 owner 检查挡住，而不是调度入口直接拒绝。

- 拒绝发生在 B 持有 TRYX 期间；
- B 释放前无 `auto_connect: fast_path` / `selected saved profile`（auto 连接 0 次）；
- A、B 终态均为 `failed/ap_not_found`。

### 3.3 判定

PASS。失败判据（auto 在 TRYX 持有时成功发起连接、或两段 `run -> init` 交叠）均未出现。

## 4. RACE-02 · cancel 后立即新 tryconnect

### 4.1 实际时序

输入：A、B 均为 `tryconnect('__ble_selftest_absent__', 'not-a-real-password-0')`。

```
20658753  cmd: async tryconnect ssid='__ble_selftest_absent__' op=op-60FD…
20663603  op_owner: TRYX -> CLEANUP by tryconnect_release ← cancel 后移交
20663853  wifi: apply_cfg: ssid='CMCC-502' …             ← cleanup 恢复
20664183  cleanup complete op=op-60FD2FD22312572F attempts=1
20664193  op_owner: NONE -> TRYX by tryconnect_begin      ← op B 取得
20669033  tryconnect: ssid '__ble_selftest_absent__' not in scan results ← B 预扫描真实执行
```

- B 在 `cleanup complete` **之后 10ms** 取得所有权；
- 全程无 `scan: start failed: ESP_ERR_WIFI_STATE`；
- op B 终态 `failed/ap_not_found`，无 `wifi_scan_failed`。

### 4.2 判定

PASS。失败判据（`busy` 收尾、`wifi_scan_failed`、cleanup 未完成即取得 TRYX）均未出现。

## 5. RACE-03 · cleanup 恢复与 auto_connect 同时

### 5.1 实际时序

输入：A = `tryconnect('CMCC-502', 'race-wrong-pass-0')`（在场 SSID + 错误口令，制造拆链）；随后 cancel。

```
20672573  cmd: async tryconnect ssid='CMCC-502' op=op-6A67…
20677423  op_owner: TRYX -> CLEANUP by tryconnect_release
20677423  auto_connect: schedule refused caller=tryconnect_finish reason=owner_busy
20677433  cmd: async tryconnect op=op-6A67… stale after wait (gen=7 state=7), no job touch
20677673  wifi: apply_cfg: ssid='CMCC-502' …            ← cleanup 自身的配置恢复
20677773  auto_connect: schedule refused caller=apply_start_released reason=owner_busy
20677793  cleanup complete op=op-6A67513DFD87354F attempts=1
20679403  auto_connect: task run caller=monitor_startup delay_actual=0ms
20679423  auto_connect: fast_path ssid=CMCC-502          ← cleanup 完成后 1.61s
```

CLEANUP 窗口 = 20677423–20677793（370ms）：期间 auto 路径连接动作 0 次，`schedule refused` 2 次；首次连接动作出现在 `cleanup complete` 之后 1.61s。

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

采样：`io.get` 完成于 `t=1795307100.113`，`network.status` 完成于 `t=1795307100.923`，**间隔 0.809s（<1s）**。

| 字段 | io.get | network.status | 一致 |
|---|---|---|---|
| mode | `sta` | `sta` | ✅ |
| sta_has_ip | `true` | `true` | ✅ |
| sta_ip | `192.168.1.45` | `192.168.1.45` | ✅ |

判定：PASS。

> 说明：首次对读未记录采样时间，为满足“间隔 <1s 并留时间戳”的判据，用 BLE 只读方式重采一次（未改任何配置），结果与首次一致。

### 7.2 MQTT `io.get`

| 步骤 | 结果 |
|---|---|
| 前置配置 | `139.9.93.245:21883` |
| `config.set` 临时指向 1883 | `success=true`，`changed=["mqtt_port"]`，`effect=subsys` |
| 设备接入 | `mqtt_connected=true` |
| MQTT `io.get` | 八字段齐全（值见下表） |
| 恢复原配置 | `139.9.93.245:21883` |
| 恢复后核验 | `config.get` 端口 21883；`device.status.mqtt.connected=true` |

MQTT `io.get` 响应 `data`：

| 字段 | 值 |
|---|---|
| relay_on | `false` |
| led_on | `false` |
| input_active | `false` |
| io_seq | `12` |
| io_source | `input` |
| net_mode | `sta` |
| sta_has_ip | `true` |
| sta_ip | `192.168.1.45` |

判定：PASS（`net_mode/sta_has_ip/sta_ip` 与同轮 BLE `io.get` 一致）。

## 8. 过程异常（四项，逐项记录）

### 8.1 RACE-01 刺激条件调整

见 §3.1 的三次尝试表。最终采用的偏离是**触发方式**（串口时序触发 + 无响应写），A/B 仍使用不存在的 SSID；中间一次“在场 SSID + 错误口令”的尝试只制造了拆链，未能在 400ms 窗口内抢入，已废弃。**属执行方案偏离，已在结果中明确标注。**

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
| `cmd01_consistency.json` | `81646e5ca001fd5fc159bda529618ec4e9bcb795fbe7a99a6dbb256a6dbb5c7f` |
| `race01_serial.log` | `0de78c60d0033c6b6b57c7f430c6d64019ca6272182fc5a28821d078786f9f28` |
| `race02_serial.log` | `768ad35fc014819c069550357b873e59dfd475fd7d9f795272ebdb512d8c4e8d` |
| `race03_serial.log` | `4171421db25cf0a168378ff74491570ed10ceb5d6b278bc721b57440f781f45e` |
| `issue12_serial.log` | `ea6213041dba0caaaa32868564519b7a0094d551f5d167ce2d87476a8185d141` |
| `cmd01_serial.log` | `83d2acf6a0c06f129b6e5d82ffea73f0220fa152134d5f33235260dbbac02c55` |

### 9.1 证据版本说明（哈希变更原因）

审查指出 4 个文件的哈希与执行时汇报的短哈希不一致，原因如下，均属**同一轮验证内的重采**，不是不同结论的证据混用：

| 文件 | 旧版（执行时汇报） | 新版（本文） | 原因 |
|---|---|---|---|
| `race_results.json` | `943d1e60…` | `c5b3dc21…` | run3 结束时脚本在 RACE-03 后异常退出，未写入完整 JSON；为形成完整快照重跑一次（run4） |
| `race01_serial.log` | `fab4038e…` | `0de78c60…` | 随 run4 重采覆盖 |
| `race02_serial.log` | `22345512…` | `768ad35f…` | 同上 |
| `race03_serial.log` | `24d2ea18…` | `4171421d…` | 同上 |
| `cmd01_consistency.json` | `546f4d51…` | `81646e5c…` | 为补采样时间戳以 BLE 只读方式重采一次（未改配置） |

未变化的文件：`issue12_results.json`、`cmd01_results.json`、`issue12_serial.log`、`cmd01_serial.log`。

说明：

- `race_results.json` 为 RACE 三项最终一次执行的快照（run4）；run4 与 run3 的判定结论一致（三项 PASS）；
- `cmd01_results.json` 同时保留了 §8.3 的首跑 FAIL 记录（脚本字段名错误）与 MQTT 全流程 PASS；
- 串口时间戳为 boot ms：RACE 三项为 20.64M–20.69M 窗口，#12 为 2.09M 窗口；两者相隔约 5 小时，**中间空档未做连续监控**，只以末态 uptime 连续作为“无重启”的弱证据。

## 10. 边界

- 本文为**执行方采集与整理**；评审方是否独立复跑/校验哈希，以评审记录为准，本文不代替独立复核。
- 本轮**未修改**代码、测试脚本、Contract；**未重新烧录**；只按授权临时修改过 `mqtt_port` 并已恢复。
- **不改变任何状态**：`CMD-01` 仍 `TEST_PENDING`；`WIFI-OP-RESOURCE-RACE` 未关闭；`PROTO-ID-01` 保持已关闭。
- RACE 与 CMD-01 的状态变更，待评审复核本文与证据后再单独决定。
