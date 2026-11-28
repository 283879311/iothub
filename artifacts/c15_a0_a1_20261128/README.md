# C15-2 A0/A1 设备验证证据包（2026-11-28）

> 证据分支：`evidence/c15-a0-a1-20261128`，基于 `main@5aaabea`。
> 本包只含脱敏文本、测试脚本、抓包参数摘录与结果 JSON；不含 NVS、app 镜像或任何凭据。
> 结论：**A0 PASS**（连接参数预检）、**A1 PASS**（begin/expire 真 TTL 实验）；生产镜像与 NVS 已恢复并核验。

## 1. 环境与对象

| 项 | 值 |
|---|---|
| 设备 | `IotHub-65D8` / `IotHubBLE-65D8`（BLE `<device-mac>`） |
| 适配器 | `1a86:7523` CH340，`/dev/ttyUSB0`；主机蓝牙 `<host-bt-addr>`（Qualcomm） |
| 主机 | Linux 7.0.0-43-generic，BlueZ/btmon 5.72 |
| 工具 | esptool 5.6.0；ESP-IDF 6.0.3-707-gdac489c6718 |
| 测试镜像 | `1.3-65f6dbb` / `build_sha 981fd6552`，BIN `43deb92e9683c78ab6848f1a954d585f0dd51f6b1decc4ec6b64d9d2363b44a1` |
| 生产镜像 | `1.3-956afcd` / `build_sha c231c9501`，app `c6f007f771211710a3b17d1758ebf90fe88eba8f2c97eafdb3799326439aa94c` |
| NVS 快照 | `f875681087da135c1b91cc2284859092ef47733ff48b0849f3f501d0af48d13b` |

## 2. 背景：设备侧 0x222 断链的根因

- 内核按 peer 维护 `hci_conn_params`，条目在**创建时**拷贝 `hdev` 默认值（`le_conn_min/max_interval`、`le_conn_latency`、`le_supv_timeout`，默认 24/40/0/42 = supervision 420 ms）。事后改 debugfs 只影响新建条目，已有条目不变。
- 设备策略要求 supervision 6000 ms，因此首连 420 ms 被判 `out of policy` → NimBLE 发起 LL 参数更新；主机侧 40 s 未完成该过程 → 设备控制器以 `LL Response Timeout` 断链（设备日志 `0x222`，主机侧随后 `0x08 Connection Timeout`）。
- 修复方式：连接前用 `MGMT_OP_LOAD_CONN_PARAM (0x0035)` 写入 per-peer 参数（24–40 / 0 / 600）。
- 两个实现坑（均已用内核源码核实）：
  1. `mgmt_hdr = {opcode, index, len}`；`0x0015` 是 `GET_CONNECTIONS`（固定长度 0），用错会 EINVAL。
  2. `addr_type` 必须为 `BDADDR_LE_PUBLIC (0x01)`；传 `0`（`BDADDR_BREDR`）时 `load_conn_param` 静默 `continue`，但仍回 `Command Complete status=0`——表现为"成功却无效"。
- BlueZ 5.72 的 `device_connect_le()` 通过 `bt_io_connect(BTPROTO_L2CAP, CID=ATT)` 建链 → 内核 `hci_connect_le()` → `hci_le_create_conn_sync()` 读取 `hci_conn_params`，因此上述 MGMT 参数对该连接路径生效。

## 3. A0：连接参数预检（生产固件，只读）

条件：先 `LOAD_CONN_PARAM(24/40/0/600, type=1)`，再 btmon 全程抓包；连接后仅发送免 token 的 `device.info`（5 s 一次）。

| 判据 | 结果 |
|---|---|
| `LE Create Connection` | min 30 ms / max 50 ms / latency 0 / **supervision 6000 ms (0x0258)** |
| `LE Enhanced Connection Complete` | interval 45 ms / latency 0 / supervision 6000 ms |
| 设备策略 | `sup=6000ms -> satisfied, skip update request`（仅此一条） |
| 60 s 静置 | 11 次心跳全部 `ok`，无断链；无 `LE Connection Update`、无远端参数请求 |
| 结束方式 | 我方 `Disconnect`（0x16/0x13），非设备侧超时 |

文件：`a0/a0_hold_r4.json`、`a0/a0_serial.log`、`a0/a0_btmon_params.txt`。

## 4. A1：begin/expire 真 TTL 实验（测试镜像）

条件：`1.3-65f6dbb`，首次 `network.tryconnect` 到 `<wifi-ssid>`，`diag.testhook arm hold_ms=310000`（剩余 TTL 298408 ms）。

设备串口时间线（uptime ms）：

| uptime | 事件 |
|---|---|
| 327834 | `conn params policy: itvl=45ms lat=0 sup=6000ms -> satisfied` |
| 648794 | `cmd: rescue=restore_persistent op=op-2CB87A304B620A66 (cleanup without commit)` |
| 648804 | `C1 hook: pause expire_post_cleanup enter (expiring=1 state=got_ip gen=1)` |
| 659954 | `C1 hook: pause expire_post_cleanup exit`（暂停 11.15 s ≈ 310 s hold − 298.4 s 剩余） |
| 659954 | `finish op-2CB87A304B620A66 gen=1 -> expired/timeout outcome=0` |
| 663714 | `central disconnected (reason=0x213)`（客户端主动断开） |

| 判据 | 结果 |
|---|---|
| 身份 | `IotHub-65D8` / `1.3-65f6dbb` / `981fd6552` |
| 首次 tryconnect | 受理 → `got_ip`，stages 四项全 true |
| 暂停期第二次 tryconnect | `busy`（拒绝；op id 为空） |
| 终态 | `expired` / `error=timeout` / `cleanup_state=ok` |
| 回滚 | `rescue=restore_persistent`；post 网络 sta/ip/mqtt/inet 全 true、trial idle |
| BLE 链路 | btmon 连续保持 362.9 s，全程无 0x222/0x08、无参数更新过程 |

文件：`a1/a1_begin.json`、`a1/a1_run.log`、`a1/a1_serial.log`、`a1/a1_btmon_params.txt`。

## 5. 恢复生产

| 项 | 结果 |
|---|---|
| app @0x10000 | `Hash of data verified`；`c15_restore_app.bin` 与恢复前回读 `post_app.bin` 一致 |
| NVS @0x9000 | 写入后立即回读 `f8756810…`，与恢复快照**逐字节一致** |
| 启动日志 | WiFi `<wifi-ssid>`、STA IP `<sta-ip>`、MQTT `mqtt://139.9.93.245:21883`、`netcheck ok gen=1 stage=mqtt` |
| BLE `device.info` | `1.3-956afcd` / `c231c9501` / `IotHub-65D8` |

文件：`restore/prod_serial.log`、`restore/restore_verify.txt`。

## 6. 凭据与失败的预跑尝试（记录）

- 测试 harness（`p0_acceptance`）在日志中对 `password` 字段统一调用 `sanitize()`，落盘值为占位符 `<redacted>`（10 字符），日志不能用于恢复口令。
- 设备当前 BLE 口令为 Kconfig 出厂默认值（见 `main/Kconfig.projbuild` 的 `AUTH_DEFAULT_PASSWORD`）；离线核对 NVS `runtime_cfg` 中 `SHA256(salt‖默认值)` 与存储哈希一致，未发现口令被更改的证据。
- 预跑尝试 1：启动器误将日志中的 `<redacted>` 占位符当作口令，`auth.login` 返回 `invalid_credentials`；脚本按门禁在**任何写命令之前**退出，未产生设备配置变更。该次输出随后被尝试 2 覆盖，未纳入本包。
- 尝试 2：使用 Kconfig 默认口令登录成功，完成本包记录的全部实验。本轮未修改设备口令或认证配置。

## 7. 限制

- 单主机（Qualcomm 控制器）、单设备、单次运行；未覆盖手机 central 或其它控制器。
- A1 的 300 s TTL 来自测试镜像的测试参数，不代表量产默认。
- MGMT 参数条目为内核内存态：适配器下电/重启或重启 bluetooth 服务后需重新写入。
- P2 未执行；`network.apply` 路径未在本轮覆盖。

## 8. 脱敏说明

SSID、STA IP、网关、BSSID、设备 MAC、host BT 地址、MQTT client_id、口令与 token 均替换为占位符（`<wifi-ssid>`、`<sta-ip>`、`<gateway>`、`<bssid>`、`<device-mac>`、`<host-bt-addr>`、`<device-id>`、`<redacted>`）。原始文件仅保留在本地 `/tmp/c15_device_20261127/`，未上传；`MANIFEST.json` 同时记录原始与脱敏文件的 SHA256 便于核对。
