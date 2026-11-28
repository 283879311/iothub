# P2 连接参数条目生命周期验证证据包（2026-11-28）

> 证据分支：`evidence/p2-connparam-20261128`，基于 `main@5abe868`。
> P2 定义以 `main@5abe868` 的 `docs/reviews/P2/P2启动定义_2026-11-28.md` 为准；本包只保存实验证据，**不修改定义**。
> 结论：**P2-1～P2-5 全部满足，S1–S8 均未触发**。

## 1. 环境与对象

| 项 | 值 |
|---|---|
| 设备 | `IotHub-65D8` / `IotHubBLE-65D8`（BLE `<device-mac>`） |
| 镜像 | 生产 `1.3-956afcd` / `build_sha c231c9501`（未烧写、未改配置） |
| 主机 | Linux 7.0.0-43-generic，BlueZ/btmon 5.72 |
| 适配器 | `0cf3:e500`（Qualcomm），地址 `<host-bt-addr>`，USB 接口 `1-10:1.0` |
| 工具 | esptool 5.6.0（未使用）；ESP-IDF 6.0.3-707 |

## 2. 索引映射（实测）

| 阶段 | 控制器索引 |
|---|---|
| 实验开始 | `hci1` |
| btusb rebind 后 | **`hci0`** |
| cleanup 结束 | `hci0`（bound + powered） |

索引变化正好验证 P2 定义中的“运行时发现”逻辑：P2-1 写入使用 index=1，P2-5 写入使用 index=0；若写死 `hci0`，P2-2/P2-3 会失败。

## 3. 判据映射（每条 → 文件与字段）

| ID | 实测值 | 复核位置 |
|---|---|---|
| P2-1 | `LOAD_CONN_PARAM` 帧 `35 00 01 00 …`（opcode 0x0035、index=1、type=1、24/40/0/600）；回包 `01 00 35 00 00` = cmd 0x0035 status 0 | `run/p2_p2_1.log`；`run/p2_run.log` 的 `Control Command: 0xffff` / `Control Event: 0xffff`；`btmon/p2_s1_decoded.txt` |
| P2-2 | `LE Create Connection` 30/50 ms、latency 0、**6000 ms**；Enhanced Complete 45 ms/0/**6000 ms**；串口 `satisfied` | `btmon/p2_s2_decoded.txt`；`serial/p2_serial.log:111`；`probes/p2_probe_p2_2.json` |
| P2-3 | power off/on 后：30/50/0/**6000 ms**；Enhanced 45/0/6000；`satisfied` → **条目保留** | `btmon/p2_s3_decoded.txt`；`serial/p2_serial.log:148`；`probes/p2_probe_p2_3.json` |
| P2-4 | rebind 后：30/50/0/**420 ms**（btmon `#469 @ 9.62 s`）；Enhanced 45/0/420；`out of policy` → **条目丢失** | `btmon/p2_s4_decoded.txt`；`serial/p2_serial.log:171`；`probes/p2_probe_p2_4.json` |
| P2-5 | 重载 index=0 status 0 → 30/50/0/**6000 ms**（btmon `#720 @ 23.22 s`）；Enhanced 45/0/6000；`satisfied` → **恢复** | `run/p2_p2_5.log`；`btmon/p2_s4_decoded.txt`；`serial/p2_serial.log:192`；`probes/p2_probe_p2_5.json` |

连接时长（串口 uptime，均我方主动断开 0x213，无 0x222）：

| 阶段 | 连接 → 断开 (ms) | 时长 |
|---|---|---|
| P2-2 | 9408552 → 9414852 | 6.3 s |
| P2-3 | 9473622 → 9478852 | 5.2 s |
| P2-4 | 9538692 → 9543872 | 5.2 s（远早于 40 s LL 超时） |
| P2-5 | 9552292 → 9557862 | 5.6 s |

btmon 段覆盖：`p2_s1`（P2-1 写入）、`p2_s2`（P2-2）、`p2_s3`（P2-3）、`p2_s4`（P2-4 + P2-5，两次连接按 handle/事件号区分）。每段均为 60 s 窗口。

## 4. 停止条件

`run/p2_run.log` 只有 `DONE`，无 `STOP`；S1–S8 均未触发。P2-4 的 420 ms 连接在 5.2 s 内主动断开，未等待设备侧参数更新超时。

## 5. 噪声说明（单列，未参与判据）

`s4` 在 3.53 s 出现一次 **BlueZ 自动连接**到 `<other-device-mac>`（另一台 NUS 设备），参数 7.5 ms / latency 32 / supervision 3000 ms。该连接与本实验对端 `<device-mac>` 无关，未用于任何判据；记录为 rebind 后的环境自动重连噪声。

## 6. 脱敏说明

- 采用**逐行字段级正则替换**（非整行遮蔽），行号与原始日志保持一致，便于按行复核；替换项：设备/第三方 MAC、host BT 地址、SSID、STA IP、网关、BSSID、MQTT client_id、口令、token，以及 mgmt 帧中的 MAC 裸 hex。
- `"password": "<redacted>"` 是 harness `sanitize()` 的占位符（10 字符），**不是真实口令**。
- **未上传原始 `.btsnoop`**：二进制内含 host/设备/第三方 MAC 字面量，无法按行脱敏；其 SHA256 记录在 `MANIFEST.json` 的 `withheld` 字段，可按授权另行提供脱敏版本。判据所需的 `LE Create Connection`/`Enhanced Connection Complete` 字段均已在 `btmon/*_decoded.txt` 中。

## 7. 限制

- 单主机（Qualcomm）、单设备、单次运行；
- 适配器存在一台无关 NUS 设备的自动重连噪声（见 §5）；
- 本包不包含 NVS/app 镜像、原始未脱敏串口或 `/tmp` 其它旧日志；
- 不涉及固件烧写、设备配置写入、`contract/`、`main/`、`tests/` 变更。
