# C15-2 C 方案 REST 只读核验证据（2026-11-28）

> 文档状态：PENDING（限定时点证据，等待处置决策）。
> 采集时刻：2026-11-28T10:5x–11:01Z。
> 授权范围：TB REST 凭据权限验证 + `Test Device C1` attributes 只读查询；仅 HTTP GET。

## 采集方式

- 基址 `http://139.9.93.245:28080`，认证头 `X-Authorization: ApiKey <key>`；凭据运行时从本地 `.local/test-env.md` 读取，未写入仓库、未输出。
- 使用 `urllib` + 禁用代理；只调用以下 GET：
  - `/api/auth/user`
  - `/api/tenant/devices?deviceName=...`、`/api/tenant/devices?pageSize=100&page=0`
  - `/api/device/{id}`、`/api/device/info/{id}`
  - `/api/plugins/telemetry/DEVICE/{id}/values/attributes`（含 `keys=` 过滤与全量）
- 未创建 MQTT 连接；未读取 telemetry timeseries；未执行任何写操作。

## 脱敏规则

- API Key、access token：不落盘、不输出。
- `sta_ssid`、`sta_ip`：仅保留 `present` 与长度档（`1-16` / `>16`），不保留原值。
- 其它非敏感属性（`net_mode`、`uart_baudrate`、`fw_version`）保留原值。
- 未知属性只保留 `present_only` 与类型，不保留值。

## 文件

| 文件 | 内容 | SHA256 |
|---|---|---|
| `01-auth-device-attrs.json` | authority、设备查找、4 个目标 attributes 与时间戳 | `21b745810fec7e98877a9627f27e0be4020a6f09b203ab66dad2adc6f020c706` |
| `02-device-info.json` | 设备实体与 `DeviceInfo.active` | `c93605096455dc2b2d157bea20bdeed3a2bda49f0701c321095db1e06029c1d6` |
| `03-all-attributes.json` | 全量 attributes key/scope/时间戳（值按规则脱敏） | `3a104eb1d62f6cec888e44ffc05a49172604922340a08a6c3d80dd58bddaeebd` |

分析见 [C15-2 C 方案 REST 只读可行性核实](../../reviews/CMD/C15-2_C方案REST只读可行性核实_2026-11-28.md)。
