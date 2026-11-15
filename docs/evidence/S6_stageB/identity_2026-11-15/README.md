# 65D8 六维身份 ×2 · 受限采集（2026-11-15 20:02）

**授权性质**：单独批准的采集窗口（BLE 只读命令 + 只读 console 日志），
与阶段 B 烧录授权分离。阶段 B 仍为 STOP；本采集不构成烧录许可。

**证据性质（按评审结论收窄）**：本记录可作为**本轮身份一致性证据**，
**不能**作为"无设备副作用的独立只读审计证明"。原因见下节"会话副作用"。

## 会话副作用（必须如实记录）

采集工具 v1（`1c8c447`）存在两处设计缺陷，已在本轮修正（见工具 v2）：

1. 连接前先 `BleakClient.connect()`，随后 `Session._connect_once()` 又断开重连，
   因此产生了**两次连接/两次断开**；断开回调 `app_ble_cmd_on_disconnect()`
   可能取消 BLE 所属的可取消异步网络操作、清理 BLE 所属 trial。
2. 会话建立必然发送 `auth.login`（校验口令并写入**短时 RAM token**，不写持久配置）；
   `device.status` / `network.status` 复用 `app_cmd_build_network_status()`，
   若 trial 恰好过期会触发 trial 清理（必要时结束配置事务）。
   采集前未检查 `trial.state == idle`，因此**不能排除**读取瞬间发生了过期清理。

**本轮之后（工具 v3，`87b62e4` 之后）**：
- 只建立一次连接、一次断开，`connect_count/disconnect_count` 由实际调用点递增
  （**物理连接成功即计数**，`auth.login` 失败也记 connect=1、close 后 disconnect=1）；
- 所有命令 `retries=0`、`allow_reconnect=False`、`allow_relogin=False`；
  **`auth.login` 也走零重试入口**（`_connect_once(login_retries=0)`；其它工具默认仍为 1，行为不变）；
- 连接后先用 `device.info.build_sha` 校验**必须属于快照白名单**，
  校验通过后才允许把 `diag.snapshot` 结果当安全依据；
- 采集前后各读一次 `diag.snapshot`，要求 `trial.state==idle` 且
  `ble_owned_active_operation` **字段存在且严格为 `false`**——缺失/null/`{}`/空串/"none"/其它类型
  一律判快照无效（fail-closed）；
- **当前固件（`a7e04b6a9`）没有 `diag.snapshot`**，工具白名单为空，
  在连接设备前即拒绝运行（rc=6）。重采必须先由固件实现该命令并另行授权窗口。

`diag.snapshot` 契约（供固件实现评审）：

```json
{"ok": true, "data": {
  "trial": {"state": "idle"},                 // 原始状态，构建响应时不得 trial_ready/clear
  "ble_owned_active_operation": false        // 或 {id, owner, state}
}}
```

## 采集范围（本轮实际）

| 项 | 内容 |
| --- | --- |
| 目标 | 65D8（BLE `10:97:BD:D3:65:DA`） |
| 命令 | `device.info`、`device.status`、`network.status`、`config.get`（各两轮）+ `auth.login` |
| 轮次间隔 | **实测 24s**（两轮 Unix 时间戳差）；`gap_s=20` 只是配置的等待参数 |
| console | 只读现有 `artifacts/cmd04_flash_20261114/phaseA/serial_ttyUSB1.log`；未暂停/重启/重开采集 |
| 未执行 | 配置写入、网络扫描/连接/断开、MQTT 发布、`diag.m1hook`、复位、烧录、OTA、C8E4 任何操作 |

**数据形态**：`identity_capture.json` 保存的是**脱敏后的解析响应对象**（`token` → `<redacted>`，
口令仅保留 `*_password_set` 布尔）与**原始长度 `raw_len`**；**未保存原始字节**。

## 结果（两轮逐维一致）

| 维度 | 结果 | 证据 |
| --- | --- | --- |
| 1 BLE 地址 | PASS | 会话地址 `10:97:BD:D3:65:DA` |
| 2 device_id | PASS | `IotHub-65D8` / `IotHub-65D8` |
| 3 build_sha | PASS | 两轮 `a7e04b6a9`；console `app_init: ELF file SHA256: a7e04b6a9` 两轮一致 |
| 4 串口映射 | PASS | console `client_id=iothub-1097BDD365D8`（第 30680 行，两轮相同） |
| 5 boot 会话 | PASS | `rst:0x1` 两轮同在第 30508 行；Δuptime=23.3s vs Δwall=24s（≤3s 容差） |
| 6 网络/MQTT | PASS | `mqtt_connected=true`、SSID `CMCC-502`、IP `192.168.1.45`，两轮一致 |

时间交叉：第一轮 20:02:33 / uptime 2,516,892 ms；第二轮 20:02:57 / uptime 2,540,172 ms；
日志增大 4,703 B / 97 行。

## 文件

- `identity_capture.json`：两轮脱敏响应对象 + raw_len、两轮 console 快照、逐维比对。
- SHA-256：`c879e149b9ea6be0a9693a469a1f46cb9cc595abbf436f18e1cdb482811a5111`
