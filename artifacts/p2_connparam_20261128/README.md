# P2 连接参数条目生命周期验证证据包（2026-11-28，rev3）

> 证据分支：`evidence/p2-connparam-20261128`，基于 `main@5abe868`。
> P2 定义以 `main@5abe868` 的 `docs/reviews/P2/P2启动定义_2026-11-28.md` 为准；本包只保存证据，**不修改定义**。
> 结论（rev3）：**P2-1 / P2-2 / P2-3 / P2-5 有证据支持；P2-4 为有限支持（仅 rebind 后初始参数）；整体按逐条判据表述，不宣告整体 PASS。**

## 0. 修订记录

- **rev1**（`5ee58d9`）：首版证据包；误将 P2-3 记为 PASS。
- **rev2**（`04162a0`）：P2-3 更正为 `NOT_COVERED`（`btmgmt -i hci1` 被解析为索引 0，power off/on 未执行；脚本未在失败处停止）；脱敏改为通用 MAC 字段替换。
- **rev3**（本次）：追加 P2-3 独立重跑证据。重跑使用数字索引、输出判定与失败即停；power off/on 经 `info` 双向确认后，重置前后连接均为 6000 ms → P2-3 改为**有证据支持**（限定范围见 §5）。rev1/rev2 保留，不改写。

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
| 主实验开始 | `hci1` |
| btusb rebind 后 | **`hci0`** |
| P2-3 重跑 | `hci0` → power off/on → **`hci0`**（索引未变） |
| cleanup | `hci0`（bound + powered） |

说明：power off/on 走 `hci_dev_close`（不释放 hdev），索引不变；rebind 走 `hci_release_dev`，索引可变（1→0）。

## 3. 判据映射（每条 → 文件与字段）

| ID | 实测值 | 结论 | 复核位置 |
|---|---|---|---|
| P2-1 | `LOAD_CONN_PARAM` 帧 `35 00 01 00 …`（opcode 0x0035、index=1、type=1、24/40/0/600）；回包 `01 00 35 00 00` = cmd 0x0035 status 0 | **支持** | `run/p2_p2_1.log`；`run/p2_run.log`；`btmon/p2_s1_decoded.txt` |
| P2-2 | `LE Create Connection` 30/50 ms、latency 0、**6000 ms**；Enhanced 45/0/6000；串口 `satisfied` | **支持** | `btmon/p2_s2_decoded.txt`；`serial/p2_serial.log:111`；`probes/p2_probe_p2_2.json` |
| P2-3 | 重跑：power off/on 经 `info` 双向确认；重置前后 `LE Create Connection` 均 30/50 ms、latency 0、**6000 ms**；串口两次 `satisfied` | **支持**（限定范围见 §5） | `p2_3_rerun/run/p2r_run.log`；`p2_3_rerun/btmon/p2r_pre_decoded.txt`、`p2r_post_decoded.txt`；`p2_3_rerun/serial/p2r_serial.log`；`p2_3_rerun/probes/*.json` |
| P2-4 | rebind 后：30/50/0/**420 ms**（btmon `#469 @ 9.62 s`）；Enhanced 45/0/420；`out of policy` | **有限支持**（仅初始参数） | `btmon/p2_s4_decoded.txt`；`serial/p2_serial.log:171`；`probes/p2_probe_p2_4.json` |
| P2-5 | 重载 index=0 status 0 → 30/50/0/**6000 ms**（btmon `#720 @ 23.22 s`）；Enhanced 45/0/6000；`satisfied` | **支持** | `run/p2_p2_5.log`；`btmon/p2_s4_decoded.txt`；`serial/p2_serial.log:192`；`probes/p2_probe_p2_5.json` |

主实验连接时长（串口 uptime，均我方主动断开 0x213，无 0x222）：

| 阶段 | 连接 → 断开 (ms) | 时长 |
|---|---|---|
| P2-2 | 9408552 → 9414852 | 6.3 s |
| P2-3（rev2 无效运行） | 9473622 → 9478852 | 5.2 s |
| P2-4 | 9538692 → 9543872 | 5.2 s（远早于 40 s LL 超时） |
| P2-5 | 9552292 → 9557862 | 5.6 s |

**P2-4 边界**：设备看到 420 ms 后确实发起连接参数更新请求，但实验约 5.2 s 后由主机主动断开，未等待更新过程走完。本证据只支持“rebind 后连接初始参数回落到 420 ms”，不支持对后续参数更新结果的判断。

## 4. 停止条件

- rev2 主实验：`p2_run.log` 以 `DONE` 结束，但 P2-3 的 `btmgmt` 错误未被识别（退出码为 0），**不是合规的“无 S 触发”**；该缺陷已在 rev2 记录，rev3 重跑修正。
- rev3 重跑：数字索引 + 输出判定；未触发任何 STOP；单次执行，无重试。

## 5. P2-3：rev2 失败记录与 rev3 重跑

### 5.1 rev2 失败（保留历史）

```text
[p2] ===== P2-3 reset A (power off/on) + btmon s3 =====
Set Powered for hci0 failed with status 0x11 (Invalid Index)
Set Powered for hci0 failed with status 0x11 (Invalid Index)
= New Index: <host-bt-addr> (Primary,USB,hci1)
```

原因：`btmgmt -i` 需要数字索引，传入 `hci1` 被解析为 0；脚本未按输出判定失败。rev2 因此记 `NOT_COVERED`。

### 5.2 rev3 重跑（数字索引 + 输出判定）

| 步骤 | 实测 |
|---|---|
| 索引 | `hci0` → 数字 `0`；地址核验通过 |
| 加载 | `LOAD_CONN_PARAM` index=0 → cmd 0x0035 status 0 |
| power off | `rc=0`；输出 `hci0 Set Powered complete, settings: ssp br/edr le secure-conn`（无 `powered`）；`info-after-off` 复核确认断电 |
| power on | `rc=0`；输出含 `hci0 class of device changed: 0x6c010c` 与 `hci0 Set Powered complete, settings: powered …`；`info-after-on` 确认 `powered`、地址与索引不变 |
| 重置前连接 | `LE Create Connection` 30/50/0/**6000 ms**；串口 `satisfied` @ 11 390 922；身份 `1.3-956afcd` |
| 重置后连接 | `LE Create Connection` 30/50/0/**6000 ms**（btmon `#156 @ 10.02 s`）；串口 `satisfied` @ 11 455 192；身份 `1.3-956afcd` |
| 断开 | 两次均 `0x213`（主机主动）；`0x222` 计数 0 |

**限定结论**：在本次已验证的适配器 power off/on 操作后，已加载的 per-peer MGMT 参数条目仍然保留，后续连接继续使用 6000 ms supervision。结论限定于本控制器、BlueZ 5.72 与本次操作，不外推到其他控制器或其他重启方式。

## 6. 噪声说明（单列，未参与判据）

主实验 `s4` 与重跑 post 段均出现 BlueZ 对 `<other-device-mac>`（另一台 NUS 设备）的自动连接（7.5 ms / latency 32 / supervision 3000 ms）。该连接与本实验对端 `<device-mac>` 无关，未用于任何判据。

## 7. 脱敏说明

- 通用 MAC 正则覆盖 `Address:`、`LE Address:` 等全部字段；已知地址用语义占位符，其余用稳定占位符 `<mac-XXXX>`（CRC 派生，跨文件一致）。
- mgmt 原始 hex 中的地址字节（LE/BE、空格分隔与裸 hex）单独替换。
- 逐行替换、保留行号；`CRLF → LF`，孤立 `\r` 删除。
- rev3 复扫结果：colon-form MAC 残留 **0**；已知地址 hex 字节形式残留 **0**；SSID/IP/BSSID/client_id/token/password 残留 **0**。
- `"password": "<redacted>"` 是 harness `sanitize()` 的占位符（10 字符），不是真实口令。

## 8. 限制

- P2-3 结论限定于本控制器/BlueZ 5.72/本次 power off/on 操作；未覆盖“仅重启 bluetooth 服务”；
- P2-4 仅支持 rebind 后初始参数回落，不判断后续参数更新结果；
- 单主机（Qualcomm）、单设备；主实验为单次，P2-3 为一次独立重跑；
- 原始 `.btsnoop` 未上传（4 个主实验 + 2 个重跑文件，SHA256 见 `MANIFEST.json.withheld`）；
- 本包不包含 NVS/app 镜像、原始未脱敏串口或 `/tmp` 其它旧日志。

## 9. 范围

- 不修改 P2 定义、代码、Contract、C15 文档；未合入 `main`；
- 本包仅 `artifacts/p2_connparam_20261128/`。
