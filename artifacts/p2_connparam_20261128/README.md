# P2 连接参数条目生命周期验证证据包（2026-11-28，rev2）

> 证据分支：`evidence/p2-connparam-20261128`，基于 `main@5abe868`。
> P2 定义以 `main@5abe868` 的 `docs/reviews/P2/P2启动定义_2026-11-28.md` 为准；本包只保存证据，**不修改定义**。
> 结论（rev2）：**P2-1 / P2-2 / P2-4 / P2-5 有证据支持；P2-3（power off/on 后条目保留）NOT_COVERED；整体不得标记 PASS。**

## 0. 修订记录（rev2）

- rev1 误将 P2-3 记为 PASS。复核发现 `btmgmt -i hci1 power off/on` 被解析为数字索引 0，两次命令均返回 `Invalid Index`；power off/on 实际未执行，s3 中观察到的 6000 ms 只能说明该时点参数仍为 6000 ms，不能证明经 power cycle 保留。
- rev1 脱敏只覆盖 3 个已知地址；rev2 改为通用 MAC 字段脱敏（含 `Address:`/`LE Address:` 全字段与 mgmt 原始 hex 中的地址字节），稳定占位符保证跨文件可关联；复扫残留为 0。
- 原始 `.btsnoop` 仍不上传（二进制含 MAC 字面量，不可按行脱敏；SHA256 见 `MANIFEST.json.withheld`）。

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

注意：P2-3 的 power off/on 未执行（见 §5），因此本包**没有**“power cycle 后索引”的观测；索引 1→0 的变化发生在重置 B（rebind）。

## 3. 判据映射（每条 → 文件与字段）

| ID | 实测值 | 结论 | 复核位置 |
|---|---|---|---|
| P2-1 | `LOAD_CONN_PARAM` 帧 `35 00 01 00 …`（opcode 0x0035、index=1、type=1、24/40/0/600）；回包 `01 00 35 00 00` = cmd 0x0035 status 0 | **支持** | `run/p2_p2_1.log`；`run/p2_run.log` 的 `Control Command: 0xffff` / `Control Event: 0xffff`；`btmon/p2_s1_decoded.txt` |
| P2-2 | `LE Create Connection` 30/50 ms、latency 0、**6000 ms**；Enhanced Complete 45 ms/0/**6000 ms**；串口 `satisfied` | **支持** | `btmon/p2_s2_decoded.txt`；`serial/p2_serial.log:111`；`probes/p2_probe_p2_2.json` |
| P2-3 | s3 连接确实为 6000 ms，但 **power off/on 未执行**（`Invalid Index`），无有效输入 | **NOT_COVERED** | 见 §5；`run/p2_run.log` 失败行；`btmon/p2_s3_decoded.txt`；`serial/p2_serial.log:148` |
| P2-4 | rebind 后：30/50/0/**420 ms**（btmon `#469 @ 9.62 s`）；Enhanced 45/0/420；`out of policy` → 条目丢失 | **支持** | `btmon/p2_s4_decoded.txt`；`serial/p2_serial.log:171`；`probes/p2_probe_p2_4.json` |
| P2-5 | 重载 index=0 status 0 → 30/50/0/**6000 ms**（btmon `#720 @ 23.22 s`）；Enhanced 45/0/6000；`satisfied` | **支持** | `run/p2_p2_5.log`；`btmon/p2_s4_decoded.txt`；`serial/p2_serial.log:192`；`probes/p2_probe_p2_5.json` |

连接时长（串口 uptime，均我方主动断开 0x213，无 0x222）：

| 阶段 | 连接 → 断开 (ms) | 时长 |
|---|---|---|
| P2-2 | 9408552 → 9414852 | 6.3 s |
| P2-3 | 9473622 → 9478852 | 5.2 s |
| P2-4 | 9538692 → 9543872 | 5.2 s（远早于 40 s LL 超时） |
| P2-5 | 9552292 → 9557862 | 5.6 s |

btmon 段覆盖：`p2_s1`（P2-1 写入）、`p2_s2`（P2-2）、`p2_s3`（P2-3，仅普通连接）、`p2_s4`（P2-4 + P2-5，按 handle/事件号区分）。每段 60 s 窗口。

**P2-4 边界**：设备看到 420 ms 后确实发起连接参数更新请求，但实验约 5.2 s 后由主机主动断开，未等待更新过程走完。本证据只支持“rebind 后连接初始参数回落到 420 ms”，不支持对后续参数更新结果的判断。

## 4. 停止条件

`run/p2_run.log` 以 `DONE` 结束，但**这不是合规的“无 S 触发”**：P2-3 的 `btmgmt power off/on` 报错后脚本没有停止（见 §5），属于 harness 缺陷；S3 未按预期生效。

## 5. P2-3 未覆盖：证据与原因

`run/p2_run.log` 原文（脱敏后）：

```text
[p2] ===== P2-3 reset A (power off/on) + btmon s3 =====
Set Powered for hci0 failed with status 0x11 (Invalid Index)
Set Powered for hci0 failed with status 0x11 (Invalid Index)
Bluetooth monitor ver 5.72
= New Index: <host-bt-addr> (Primary,USB,hci1)
```

原因链：

1. `btmgmt -i` 期望数字索引；脚本传入 `hci1`，被解析为 `0`，因此两条命令都作用于不存在的 hci0 并返回 `Invalid Index`；
2. `btmgmt` 报错后退出码仍为 0，脚本的 `||` 失败分支未触发，继续执行了 s3 的连接测试；
3. 因此 s3 的 6000 ms 只证明“该时点 per-peer 参数仍为 6000 ms”，**不能**证明“power off/on 后条目保留”。

处理：本轮不重试、不补造；P2-3 记 `NOT_COVERED`。若需覆盖，须另行授权（并把 `btmgmt -i` 改为数字索引、按输出而非退出码判定失败）。

## 6. 噪声说明（单列，未参与判据）

`s4` 在 3.53 s 出现一次 **BlueZ 自动连接**到 `<other-device-mac>`（另一台 NUS 设备），参数 7.5 ms / latency 32 / supervision 3000 ms。该连接与本实验对端 `<device-mac>` 无关，未用于任何判据。

## 7. 脱敏说明（rev2）

- 通用 MAC 正则覆盖 `Address:`、`LE Address:` 等全部字段；已知地址使用语义占位符（`<device-mac>`、`<host-bt-addr>`、`<other-device-mac>`），其余地址使用稳定占位符 `<mac-XXXX>`（CRC 派生，同一地址跨文件一致，保留事件关联能力）。
- mgmt 原始 hex 中的地址字节（LE/BE、空格分隔与裸 hex）单独替换。
- 逐行替换、保留行号；`CRLF → LF`，孤立 `\r` 删除。
- 复扫结果：colon-form MAC 残留 **0**；已知地址 hex 字节形式残留 **0**；SSID/IP/BSSID/client_id/token/password 残留 **0**。
- `"password": "<redacted>"` 是 harness `sanitize()` 的占位符（10 字符），不是真实口令。

## 8. 限制

- P2-3 `NOT_COVERED`；harness 未在 `btmgmt` 失败处停止；`btmgmt -i` 索引用法缺陷；
- 单主机（Qualcomm）、单设备、单次运行；
- 适配器存在一台无关 NUS 设备的自动重连噪声（见 §6）；
- 原始 `.btsnoop` 未上传（哈希见 `MANIFEST.json.withheld`）；
- 本包不包含 NVS/app 镜像、原始未脱敏串口或 `/tmp` 其它旧日志。

## 9. 范围

- 不修改 P2 定义、代码、Contract、C15 文档；未合入 `main`；
- 本包仅 `artifacts/p2_connparam_20261128/`。
