# 65D8 六维身份 ×2 · 只读采集（2026-11-15 20:02）

**授权性质**：单独批准的**只读**身份采集窗口，与阶段 B 烧录授权分离。
阶段 B 仍为 STOP；本采集不构成烧录许可。

## 采集范围

| 项 | 内容 |
| --- | --- |
| 目标 | 65D8（BLE `10:97:BD:D3:65:DA`） |
| 命令 | `device.info`、`device.status`、`network.status`、`config.get`（各两轮） |
| 轮次间隔 | 24s（授权要求 ≥20s） |
| console | 只读现有 `artifacts/cmd04_flash_20261114/phaseA/serial_ttyUSB1.log`；未暂停/重启/重开采集 |
| 未执行 | 配置写入、网络扫描/连接/断开、MQTT 发布、`diag.m1hook`、复位、烧录、OTA、C8E4 任何操作 |

原始帧已 `sanitize`（`token` → `<redacted>`；口令仅保留 `*_password_set` 布尔）。

## 结果（两轮逐维一致）

| 维度 | 结果 | 证据 |
| --- | --- | --- |
| 1 BLE 地址 | PASS | 会话地址 `10:97:BD:D3:65:DA`（scan 过滤 NUS 后唯一命中） |
| 2 device_id | PASS | `IotHub-65D8` / `IotHub-65D8` |
| 3 build_sha | PASS | 两轮 `a7e04b6a9`；console `app_init: ELF file SHA256: a7e04b6a9` 两轮一致 |
| 4 串口映射 | PASS | console `client_id=iothub-1097BDD365D8`（第 30680 行，两轮相同） |
| 5 boot 会话 | PASS | `rst:0x1` 两轮同在第 30508 行；Δuptime=23.3s vs Δwall=24s（≤3s 容差） |
| 6 网络/MQTT | PASS | `mqtt_connected=true`、SSID `CMCC-502`、IP `192.168.1.45`，两轮一致 |

## 文件

- `identity_capture.json`：两轮原始响应（sanitize）、两轮 console 快照（大小/行数/uptime/行号）、逐维比对。
- SHA-256：`c879e149b9ea6be0a9693a469a1f46cb9cc595abbf436f18e1cdb482811a5111`

采集工具：`tools/s6_identity_capture.py`（只读；失败即停，不做修复尝试）。
