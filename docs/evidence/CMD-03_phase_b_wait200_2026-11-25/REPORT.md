# CMD-03 阶段 B WAIT200 重复测量报告（2026-11-25，本地证据）

> 状态：测量完成，20/20 PASS；NVS 出现非配置类偏差（`phy/cal_data`），按授权停止后续测试并已恢复生产固件。阶段 B 是否关闭待评审。
> 证据目录：`artifacts/cmd03-phase-b-2026-11-25/`（`.gitignore` 忽略，未提交）。

## 1. 授权与范围

按 2026-11-25 确认的修订版授权包执行：设备 `IotHub-65D8`、`407be43` WAIT200 镜像、仅 app 分区烧写、空闲 10 + BLE 负载 10、NVS 只读核验、恢复指定生产镜像；不改代码/文档/200 ms 常量/生产 cleanup 语义。

## 2. 身份链

| 项 | 值 |
|---|---|
| 串口 | `/dev/ttyUSB0`，VID:PID `1a86:7523`，无 USB serial，by-id `usb-1a86_USB_Serial-if00-port0` |
| 芯片 MAC（eFuse） | `<device-mac>` |
| 生产身份（烧写前） | `device_id=IotHub-65D8`、`firmware=1.3-956afcd`、`build_sha=c231c9501` |
| 生产状态（烧写前） | STA/IP OK、`internet_verified=true`、`net_verify_stage=mqtt`、`mqtt_connected=true`、`trial=idle` |
| 分区表 | NVS `0x9000`/16K；otadata `0xd000`/8K；ota_0 `0x10000`；ota_1 `0x1f0000` |
| 构建环境 | ESP-IDF `v6.0.3-707-gdac489c6718`（22 份设备日志的 2nd stage bootloader 行） |

## 3. 镜像与烧写

| 用途 | 文件 | SHA-256 |
|---|---|---|
| 测试 BIN | `build-wait200/iothub.bin` | `5baef00206b66d916e5ca1e7a848d00a82cb180ca2807122e2c163bd162964b2` |
| 测试 ELF | `build-wait200/iothub.elf` | `2adcbe15b90d33c46d8b1d21f2f1591a3934cc8b409a56aca6e996f057cb08f8` |
| 恢复 BIN | `build-recovery/iothub.bin` | `fe63007484cb11782da1c8642d4b0f080800607e9605363706fba20f199ac995` |
| 恢复 ELF | `build-recovery/iothub.elf` | `c231c9501909eb9ce6dcdf687c812690eeba216502a576aa9ccc1c68eb1b2138` |

烧写均只写 app `0x10000`，`write_flash` 自带校验通过（`Hash of data verified.`），未擦除整片、未写 NVS/bootloader/分区表。测试镜像 `--after no_reset` 保持 bootloader，确保每次样本都由受控复位启动。

## 4. 采样与结果

时间窗：2026-11-25 21:22:33 – 21:27:01（20 次运行）。每次独立串口日志与摘要。

| 样本 | run_id | op | wait_us | ticks | claim | call_ms | cleanup_ms | 结果 |
|---|---|---|---:|---:|---:|---:|---:|---|
| idle-01 | 9F2D05F6000D79B1 | op-C704F4B68A79BA60 | 199067 | 20 | RUNNING | 210 | 20 | PASS |
| idle-02 | 1505E0EA000DB10E | op-17BB3171AFCAD1FD | 199809 | 20 | RUNNING | 210 | 20 | PASS |
| idle-03 | CD225BE8000DA115 | op-C4A38F5B5D372B08 | 190764 | 20 | RUNNING | 200 | 20 | PASS |
| idle-04 | 67A6621F000DA0AF | op-9BD4F45649ACDC46 | 197490 | 20 | RUNNING | 210 | 30 | PASS |
| idle-05 | 85C29331000DB090 | op-44D1379FE21D62B3 | 190170 | 20 | RUNNING | 200 | 20 | PASS |
| idle-06 | 7B59D494000DD7C9 | op-946719F381226327 | 198791 | 20 | RUNNING | 210 | 20 | PASS |
| idle-07 | 33C4BFF7000DA0AA | op-808B1572D11BE4F6 | 190358 | 20 | RUNNING | 210 | 20 | PASS |
| idle-08 | EED6B6D0000DA0F7 | op-2BF49F873CC0D7F0 | 190243 | 20 | RUNNING | 200 | 20 | PASS |
| idle-09 | 38A2209E000DA24A | op-B93947DDC9F1D788 | 190827 | 20 | RUNNING | 200 | 20 | PASS |
| idle-10 | 60253101000DA0F0 | op-BB19344A8225DFDC | 190149 | 20 | RUNNING | 210 | 50 | PASS |
| load-01 | A952C5ED000DB05B | op-1A9110E77767B2B0 | 190841 | 20 | RUNNING | 210 | 30 | PASS |
| load-02 | 772B01D3000DA0E1 | op-FAEC05A162138AA9 | 198037 | 20 | RUNNING | 210 | 20 | PASS |
| load-03 | 25ECF0ED000DC8BF | op-1F04EE24EB6F15A6 | 190169 | 20 | RUNNING | 200 | 20 | PASS |
| load-04 | 184E6DBD000DA1C6 | op-09D409FD38B41724 | 199345 | 20 | RUNNING | 210 | 20 | PASS |
| load-05 | D9F6E02B000DA0F4 | op-F2F19CC9B63CD93F | 190352 | 20 | RUNNING | 200 | 20 | PASS |
| load-06 | 39E76659000DC876 | op-B57E91C03F92F2BB | 190209 | 20 | RUNNING | 200 | 20 | PASS |
| load-07 | F940F41A000DEF99 | op-9B2DBF7F2B336FD6 | 190743 | 20 | RUNNING | 210 | 20 | PASS |
| load-08 | 83A9D3BE000F5E51 | op-7AA0E635B2BFC1EE | 191686 | 20 | RUNNING | 200 | 20 | PASS |
| load-09 | 5900D660000FC3FB | op-986D8AD510F0BD0F | 190130 | 20 | RUNNING | 200 | 20 | PASS |
| load-10 | E680404D000DC92B | op-506572F6E329376A | 190164 | 20 | RUNNING | 200 | 20 | PASS |

`wait_us` 为 `app_cmd_net_cleanup_wait_result()` 内部 `esp_timer` 测量；`call_ms` 为串口 `DEVTEST_WAIT_START`→`WAIT_END` 时间戳差（ms 级，含日志开销）；`cleanup_ms` 为 `WAIT_END`→`CLEANUP_DONE_OK`。

### 4.1 统计

| 指标 | 空闲（10） | BLE 负载（10） |
|---|---|---|
| wait_us 范围 | 190149–199809（均值 193767） | 190130–199345（均值 192168） |
| call_ms 范围 | 200–210 | 200–210 |
| cleanup_ms 范围 | 20–50 | 20–30 |
| waited_ticks | 20/20，全部 `normal_exhaustion` | 20/20，全部 `normal_exhaustion` |
| 单次断言 | 10/10 PASS | 10/10 PASS |

两臂均未突破候选阈值：`call_ms ≤ 250`、`cleanup_ms ≤ 1000`。候选阈值不构成本轮冻结的 `T_max`。

### 4.2 七项断言（20/20 全满足）

1. 第二调用者进入等待：`wait_start/end` 非零、`cleanup wait timeout` 各 1 次；
2. 返回时 `claim=RUNNING`；
3. 第一 claimant 随后 `CLEANUP_DONE_OK` 恰好 1 次、`claim=OK`；
4. 真实 cleanup 一次：`cleanup_request` 日志 1 次、`claim_count=1`、无 anomaly/inconsistent；
5. `restore_count=1`、`restore_ok=1`；
6. 终态 `cancelled`/`cleanup=OK`；job/trial 已清（无 `DEVTEST_RESOURCE`）；
7. 无 panic/assert、无第二次 cleanup。

第二调用者 `second_result=0` 为预期超时返回；helper 自身 `outcome=PASS acceptance=pilot_pending`。

### 4.3 BLE 负载覆盖

10/10 次连接建立于 3.76–5.09 s（相对 hold 脚本启动），`DEVTEST_NET ready` 于约 8.9 s，连接持续到 `DEVTEST_RESULT`，无断连事件。

## 5. NVS 核验（偏差）

| 时间点 | SHA-256 |
|---|---|
| 烧写前 | `c1e04bc96a88f60f41c7b5d8a2a54473edca548d15d1d01dcbc8f7fc2f3f5dc6` |
| 20 次运行后 | `849ccdecbc87de904f409b92dd8b9f08e515d2965508d373073cb958e077a301` |
| 恢复后 | `849ccdecbc87de904f409b92dd8b9f08e515d2965508d373073cb958e077a301` |

- 已用 IDF `nvs_tool.py` 解析：92 个条目 / 91 个键（`phy/cal_data` 含两个 blob 条目），**唯一变化为 `phy/cal_data`**：blob0 1280→1600 字节（非零 956→1049），blob1 624→304 字节（非零 92→0，归零）。`iothub`、`nvs.net80211`、`ble_prov`、`misc` 全部键值不变。
- 持久配置子集 SHA-256（排除连接态/计数/IP）：烧写前与恢复后均为 `20cd05f34acf83f791a64ab699473e6051faa46720e1c7d611389e11bf268e3f`。
- 恢复烧写与恢复启动未再改变 NVS（后两次哈希相同）。
- 判定：NVS 哈希未保持逐字节一致，按授权该条件未满足；但差异属于 Wi-Fi 驱动 PHY 校准数据，不是 `iothub` 配置或 Wi-Fi profile 凭据变更。阶段 B 不据此单独宣布关闭，待评审处置。

## 6. 恢复核验

- `device.info`：`1.3-956afcd` / `c231c9501` / `IotHub-65D8`；
- `network.status`：`sta_connected=true`、`sta_has_ip=true`、`internet_verified=true`、`net_verify_stage=mqtt`、`mqtt_connected=true`、`trial=idle`；
- 持久配置哈希与测试前一致；
- 恢复镜像 SHA-256 与授权一致，烧写校验通过。

## 7. 停止条件与偏差记录

- 触发项：NVS 前后哈希不一致（授权要求一致）。发现后未再运行任何样本，仅执行生产固件恢复。
- 20 个有效样本已完成，无 FAIL/SKIP/INVALID，无补测。
- 偏差不属于 harness INVALID；未改写为 PASS。

## 8. 限制

- `cleanup_ms` 为串口日志 ms 级；`wait_us` 仅覆盖等待循环，不含调用方前处理。
- `DEVTEST_WORKER_SETTLE` 仍为弱证据（`running_observed=unknown`）。
- NVS 仅解析结构与键值，未对 `phy/cal_data` 内容做进一步语义解释。
- 原始日志含 SSID/BSSID/IP 等，发布前需脱敏；本目录未提交。

## 9. 结论

- WAIT200 测量：两臂各 10/10 PASS，七项断言全满足，候选阈值未突破。
- NVS：仅 `phy/cal_data` 变化，配置未变；与授权“NVS 哈希一致”条件冲突，需评审决定是否接受为驱动正常行为或调整核验口径。
- 阶段 B 关闭、`T_max` 冻结、文档登记均未执行，等待授权（2026-11-25 后续决策见“决策登记”）。

## 10. 决策登记（2026-11-25）

测量完成后作出的正式验收决策；不改变上文数据、NVS 哈希与偏差记录：

- **D2 WAIT200**：全调用墙钟 `T_max=250 ms` 正式冻结；计时边界为第二调用者进入 `app_cmd_net_cleanup_run()` 到该调用返回；内部 `elapsed_us` 不替代全调用墙钟；不设 200 ms 下限；1 s cleanup 收敛窗口维持观察性要求。超限判验收失败，不以调度因素豁免。
- **D1 NVS**：整分区逐字节一致仍记 **FAIL**（`c1e04bc9…` → `849ccdec…` 保留）；应用持久配置完整性 **PASS**；`phy/cal_data` 接受为 ESP-IDF 系统管理数据例外；无法解释的条目变化或其他应用命名空间变化仍须调查。
- 阶段 B 测量与技术判定完成；CMD-03、P0 状态仍以 [CMD 任务台账](../../status/CMD任务台账.md) 为准，未因本决策自动关闭。

规范见 [CMD-03 WAIT200 与 NVS 验收规范](../../reviews/CMD/CMD-03_WAIT200与NVS验收规范_2026-11-25.md)。
