# CMD-02/CMD-15 阶段 3 设备回归证据（2026-11-26）

> 文档状态：CURRENT（限定时点证据）。设备 `IotHub-65D8`；固件 `1.3-956afcd` / `build_sha=c231c9501`。
> 本目录归档 r1–r7 七轮执行；不同轮次结果分别保留，**未拼接成单轮完整 PASS**。

## 1. 轮次与 harness 版本

| 轮次 | harness SHA 前缀 | 停止原因 |
|---|---|---|
| r1 | `88ede887` | paho 2.x `ReasonCode` 崩溃 |
| r2 | `d6c62316` | C15-1 属性断言缺 `sta_has_ip` |
| r3 | `dc9ab8b6` | C15-2 匿名订阅 21883 被拒（rc=135） |
| r4 | `09346297` | SUBACK 回调计数错误 |
| r5 | `4191be15` | C15-2 tryconnect 返回 `busy` |
| r6 | `d05c8976` | C15-1 `c151st` 读超时（15s） |
| r7 | `fb79bd7a` | C15-1 `c151st` 读超时（30s） |

最终 harness 版本 `fb79bd7a…` 已入 `main@3d5d67f`；本目录仅归档执行事实与结果。

## 2. 逐轮结果

见 [run-summary.tsv](run-summary.tsv)。要点：

- C2-1～C2-4：r1–r7 每轮均 PASS；C2-3 `got_ip` 关联每轮 `NOT_COVERED`（授权允许的 PARTIAL）。
- C15-1：r3、r5 完整 PASS（切 1883、attributes 快照、恢复 21883、只读核验）。
- C15-2：r5 属性订阅建立成功（使用设备 token），但 tryconnect `busy`；r6/r7 未到达；其余轮次在 C15-2 前停止。
- 每轮停止后均完成配置恢复与只读核验；设备保持 `<mqtt-broker>:21883`、MQTT/STA/IP 正常、`trial=idle`、持久配置哈希不变、无 panic。

## 3. 已发现的 harness 缺陷

1. paho-mqtt 2.x `ReasonCode` 不能直接 `int()`（r1）。
2. C15-1 曾要求 attributes 提供 `sta_has_ip`；该字段实际仅存在于 telemetry（r2）。
3. 21883 匿名 CONNACK 被 ThingsBoard 拒绝（rc=135）；需设备 token（r3）。
4. 设备 token 订阅 telemetry 被 SUBACK 0x80 拒绝；21883 只能订阅 attributes（r4）。
5. 一次 SUBSCRIBE 只产生一个 SUBACK 回调；按回调数等待 2 次会超时（r4）。
6. 单次只读 `device.status` 超时直接终止轮询，未在总预算内继续（r6/r7）。
7. C15-2 判据仍从 attributes 读取 `sta_has_ip`，与字段来源冲突（未执行到判据，代码审查发现）。
8. C15-2 就绪等待日志写“连续 5s”，代码为 `stable >= 10`（文案错误）。

## 4. 脱敏与边界

- 串口/摘要/控制台日志已替换：SSID、设备/网关 IP、BSSID、设备 MAC/BLE MAC、中心端 BLE MAC、MQTT broker、client_id、本机路径。
- 设备 MQTT token 仅通过环境变量注入，未写入日志、文档或本目录；扫描确认 0 命中。
- 未归档 NVS 二进制或分区镜像；未修改生产源码、Contract、CMake。
- 原始未脱敏副本保留在本地 `artifacts/`，未提交。

## 5. 结论边界

- 本目录只证明各轮实际执行事实；C15-2 未完成，不能据此判定 CMD-15 PASS。
- CMD-02 阶段 3 的 PASS 判定依据 C2 的多轮结果与 C2-3 PARTIAL 的授权口径，见 [阶段 3 报告](../../reports/CMD-02_15_阶段3设备回归结果_2026-11-26.md)。
- P0 关闭评审另行记录于 CMD 任务台账。
