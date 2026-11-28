# C15-2 C 方案 REST 只读核验证据（2026-11-28）

> 文档状态：PENDING（限定时点证据，等待处置决策）。
> 采集时刻：2026-11-28T10:5x–11:20Z。
> 授权范围：TB REST 凭据权限验证 + `Test Device C1` attributes 只读查询；租户 Key 会话仅 HTTP GET；客户账号会话仅登录 + GET。遥测 timeseries 查询依据用户 2026-11-28 对 `net_mode` 遥测数据的指认补做。

## 采集方式

- 基址 `http://139.9.93.245:28080`，认证头 `X-Authorization: ApiKey <key>`；凭据运行时从本地 `.local/test-env.md` 读取，未写入仓库、未输出。
- 客户账号会话：`POST /api/auth/login`（仅认证，不改状态）取得 JWT，随后仅 GET；密码/JWT 未落盘、未输出。
- 使用 `urllib` + 禁用代理；只调用以下 GET：
  - `/api/auth/user`
  - `/api/tenant/devices?deviceName=...`、`/api/tenant/devices?pageSize=100&page=0`
  - `/api/device/{id}`、`/api/device/info/{id}`
  - `/api/plugins/telemetry/DEVICE/{id}/values/attributes`（含 `keys=` 过滤与全量）
  - `/api/plugins/telemetry/DEVICE/{id}/keys/timeseries`、`/values/timeseries`（`net_mode`/`uart_baudrate`/`sta_has_ip`，15 min 窗口）
- 客户账号会话另含 `POST /api/auth/login`（认证用途）与上述同类 GET。
- 未创建 MQTT 连接；未执行任何写操作。

## 脱敏规则

- API Key、access token：不落盘、不输出。
- `sta_ssid`、`sta_ip`：仅保留 `present` 与长度档（`1-16` / `>16`），不保留原值。
- 客户账号：密码与 JWT 不落盘、不输出；仅记录 authority、HTTP 状态与脱敏 attributes。
- 其它非敏感属性（`net_mode`、`uart_baudrate`、`fw_version`）保留原值。
- 未知属性只保留 `present_only` 与类型，不保留值。

## 文件

| 文件 | 内容 | SHA256 |
|---|---|---|
| `01-auth-device-attrs.json` | authority、设备查找、4 个目标 attributes 与时间戳 | `21b745810fec7e98877a9627f27e0be4020a6f09b203ab66dad2adc6f020c706` |
| `02-device-info.json` | 设备实体与 `DeviceInfo.active` | `c93605096455dc2b2d157bea20bdeed3a2bda49f0701c321095db1e06029c1d6` |
| `03-all-attributes.json` | 全量 attributes key/scope/时间戳（值按规则脱敏） | `3a104eb1d62f6cec888e44ffc05a49172604922340a08a6c3d80dd58bddaeebd` |
| `04-customer-account.json` | `customerC` 登录、authority、设备读取、tenant API 403、attributes 读取 | `4f11e831a7689eb0d8c848cd26073f55cccf06b76280521af03323f1518d6ea9` |
| `05-telemetry-timeseries.json` | 15 min 窗口内 `net_mode`/`uart_baudrate`/`sta_has_ip` 点数、节奏、值域；`sta_ssid`/`sta_ip` 不在 telemetry | `e478327e7c0f798cc9d0f7e35917a5b80005d85f23c543bba65b5fc36c53fbfa` |

分析见 [C15-2 C 方案 REST 只读可行性核实](../../reviews/CMD/C15-2_C方案REST只读可行性核实_2026-11-28.md)。
