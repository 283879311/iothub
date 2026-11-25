# CMD-03 Phase B WAIT200 repeated measurement (2026-11-25)

> 文档状态：CURRENT（限定时点证据）。测试源码 `407be43`；生产固件 `main@956afcd`（文档基线 `6a7c8e0`）；构建环境 ESP-IDF `v6.0.3-707-gdac489c6718`。
> 结论：20/20 单次 PASS（空闲 10 + BLE 负载 10）；NVS 出现 `phy/cal_data` 非配置类变化，按授权停止后续测试并已恢复生产固件；阶段 B 未关闭，`T_max` 未冻结。

## 范围

按 2026-11-25 授权的修订版阶段 B 方案执行：设备 `IotHub-65D8`、WAIT200 镜像、仅 app 分区烧写、两臂各 10 次、NVS 只读核验、恢复指定生产镜像。未改代码、文档以外的仓库内容或 200 ms 常量。

## 结果

- 空闲臂 10/10 PASS；BLE 负载臂 10/10 PASS。
- `wait_us` 190130–199809；全调用墙钟（串口时间戳）200–210 ms；cleanup 收敛 20–50 ms。
- 20/20 均为 `waited_ticks == 20`、返回时 `claim=RUNNING`、`CLEANUP_DONE_OK` 恰好一次、`claim=1 restore=1`、终态 `cancelled`/`cleanup=OK`。
- BLE 负载臂 10/10 连接建立于 3.76–5.09 s，持续到 `DEVTEST_RESULT`，无断连。
- 候选阈值 250 ms / 1 s 未被突破；本轮不冻结 `T_max`。

逐次数据见 [run-summary.tsv](run-summary.tsv)，完整说明见 [REPORT.md](REPORT.md)。

## NVS

| 时间点 | SHA-256 |
|---|---|
| 烧写前 | `c1e04bc96a88f60f41c7b5d8a2a54473edca548d15d1d01dcbc8f7fc2f3f5dc6` |
| 20 次运行后 | `849ccdecbc87de904f409b92dd8b9f08e515d2965508d373073cb958e077a301` |
| 恢复后 | `849ccdecbc87de904f409b92dd8b9f08e515d2965508d373073cb958e077a301` |

IDF `nvs_tool.py` 解析：92 个条目 / 91 个键（`phy/cal_data` 含两个 blob 条目），唯一变化为 `phy/cal_data`：blob0 1280→1600 字节（非零 956→1049），blob1 624→304 字节（非零 92→0）。`iothub`、`nvs.net80211`、`ble_prov`、`misc` 全部键值不变；持久配置子集哈希前后均为 `20cd05f34acf83f791a64ab699473e6051faa46720e1c7d611389e11bf268e3f`。NVS 二进制未发布，仅保留哈希与键级差异摘要。

## 恢复

生产 BIN `fe630074…` 烧写校验通过；`device.info` 为 `1.3-956afcd` / `c231c9501` / `IotHub-65D8`；`network.status` 为 STA/IP、`internet_verified=true`、`mqtt_connected=true`、`trial=idle`；持久配置哈希与测试前一致。

## 偏差与停止

授权要求 NVS 前后哈希一致；实际仅 `phy/cal_data` 变化。发现后未再运行样本，仅执行生产固件恢复；20 个有效样本无 FAIL/SKIP/INVALID，无补测。该偏差未改写为 PASS，阶段 B 关闭与文档登记等待处置。

## 脱敏与发布

- 串口/摘要/状态/配置日志已替换：SSID、设备/网关 IP、BSSID、设备 MAC/BLE MAC、中心端 BLE MAC、MQTT broker、client_id、本机路径。
- NVS 二进制、NVS 逐键值 dump、分区表二进制未发布；仅保留哈希、键级差异摘要和分区表文本。
- 原始未脱敏副本保留在本地 `artifacts/cmd03-phase-b-2026-11-25/`（未提交）。

## 限制

- cleanup 收敛为 ms 级（串口时间戳），`wait_us` 仅覆盖等待循环。
- `DEVTEST_WORKER_SETTLE` 仍为弱证据（`running_observed=unknown`）。
- 本证据只覆盖 WAIT200；不构成完整设备矩阵或 CMD-03/P0 关闭依据。

## 修订记录

- 2026-11-25：`phy/cal_data` 长度统一为解码后字节并区分两个 blob（此前 `raw/nvs-diff-summary.txt` 使用 base64 字符数且只覆盖第二个 blob）；补充实际构建环境 ESP-IDF `v6.0.3-707-gdac489c6718`。

