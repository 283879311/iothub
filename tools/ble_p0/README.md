# P0 BLE 验收 harness

PC 侧 BlueZ + bleak 充当 BLE central，模拟微信小程序的 NUS 命令面，验证：

1. 按 NUS Service UUID 过滤扫描（`services` 的 BlueZ 等价物）；
2. ADV / Scan Response 名称观察；
3. 10 次长响应（`network.scan`）按 id 重组，确认 JSON 不被截断；
4. v1 `network.tryconnect` 期间的 `operation.get` 非阻塞响应；
5. 错误口令、重复请求、cancel 竞态、not_found，以及最终 v0 `config.set {"net_mode":"sta"}`；
6. T1 成功网络事务：`tryconnect → got_ip → apply → 重启持久化`（独立脚本）；
7. T2 操作槽生命周期：4 槽 `no_slot`、active `busy`、600s 终态回收、旧 ID `not_found`（独立脚本）；
8. T3 BLE 命令/数据通道：GATT 审计、CMD 20B 长命令/长响应、DATA 回环/长帧/连续流、双通道并发、断连恢复（独立脚本）；
9. AP 配置 schema 清理与旧 NVS/旧客户端检查：`config.get` 无 AP 字段、旧 AP patch 不生效、`net_mode=ap` 拒绝（独立脚本）。

## 依赖

```bash
# 本机已具备 BlueZ 5.72 / bluetoothd；bleak 3.0.2 在项目 .venv-bleak 中
.venv-bleak/bin/python -c "import importlib.metadata as m; print(m.version('bleak'))"
```

若 BlueZ 未运行：`sudo systemctl start bluetooth`。

## 用法

```bash
cd /home/tc/esp/workspace/iothub

# 正确 Wi-Fi 口令只从环境变量读取，不写入结果文件
IOTHUB_WIFI_PASSWORD='<WIFI_PASSWORD>' \
  .venv-bleak/bin/python -u tools/ble_p0/p0_acceptance.py --ssid CMCC-502

# 不发送最后那条 config.set
IOTHUB_WIFI_PASSWORD='<WIFI_PASSWORD>' \
  .venv-bleak/bin/python -u tools/ble_p0/p0_acceptance.py --ssid CMCC-502 --no-final-config-set

# T1 成功路径（会自动 apply 后重启验证持久化）
IOTHUB_WIFI_PASSWORD='<WIFI_PASSWORD>' \
  .venv-bleak/bin/python -u tools/ble_p0/success_path.py --ssid CMCC-502

# T2 操作槽生命周期（硬复位清槽；约 11 分钟；不需要正确 Wi-Fi 口令）
.venv-bleak/bin/python -u tools/ble_p0/t2_slot_reclaim.py --ssid CMCC-502

# T3 BLE CMD/DATA（20B 分片；MTU23 测试构建见 T3 文档）
.venv-bleak/bin/python -u tools/ble_p0/t3_ble_channels.py --ssid CMCC-502 --max-write 20

# AP schema 检查（烧录后不要擦 NVS；验证旧配置布局/旧客户端字段）
.venv-bleak/bin/python -u tools/ble_p0/config_schema_check.py \
  --address b0:a7:32:28:c8:e4 --no-reset
```

可选参数：

- `--address`：指定 BLE 地址（默认按 `IotHubBLE-` 名或 NUS UUID 选择）；
- `--wrong-password`：错误口令（默认随机生成）；
- `--n-long`：长响应次数（默认 10）；
- `--scan-timeout`：过滤扫描秒数（默认 12）；
- `--no-final-config-set`：跳过最终 v0 config.set；
- `--max-write`：CMD/DATA 单次 GATT 写入上限，默认 **20B**（MTU23 的 ATT payload）；MTU256 可显式调大，但小程序应按实际 MTU-3 计算。

## 行为说明

- 扫描前会 `bluetoothctl remove` 清掉缓存的 IotHub 设备，确保过滤扫描是新鲜命中。
- 命令按 `id` 配对；CMD TX 通知按 `{}` 边界重组，支持字符串内 `}`/转义。
- 每次命令间隔 ≥250ms，避开固件 5 次/秒限频；收到 `rate_limited` 会自动重试。
- 默认单次 GATT 写 **20B**，按实际协商 MTU-3 调整；275B 的静态 IP `config.set` 会拆成 14 片。固件 CMD RX 按 512B 行重组。小程序也应按 MTU-3 分片，不能一次写整包。
- DATA TX 是原始字节 Notify，不是 JSON；`data.loopback` 只用于测试 BLE→BLE 回环，测试后关闭。DATA 没有应用层 ACK/序号/重传/帧重组，GATT 写成功不等于送达。
- 主机侧 BLE 断链会自动重连并重登，长响应测试会重试同一条并记录 `reconnects_total`。
- 错误口令测试的 PASS 要求终态 `failed` 且错误为 `wrong_password`/`connect_failed`/`timeout`；`ap_not_found` 不算错误口令通过。
- `config.set` 只在设备侧核心项（过滤、长响应、operation.get）通过后发送；当前设备若已是 `net_mode=sta`，响应会是 `changed=[]`、`effect=none`，不是失败。
- token、Wi-Fi 口令、管理口令在日志/结果中均脱敏。

## T1 成功路径脚本

`success_path.py` 只做 T1，适合不想重跑扫描/长响应全套时使用：

- 连接、login、记录 `network.status`、扫描确认目标 SSID 可见；
- `network.tryconnect` → 轮询 `operation.get` 到 `got_ip`；
- `network.apply`、重复 apply、成功后 cancel；
- 等待 `config.get` 确认 `sta_use_static=false` 和 IP；
- 默认用 `esptool.py` 硬复位并重新连接，验证重启持久化和 MQTT 自动恢复；
- 结果写入 `tools/ble_p0/results/success_path_<timestamp>.json`。

注意事项：

- apply 按固件逻辑会把 `sta_use_static` 置为 false，设备 IP 可能从静态地址变为 DHCP 地址；
- `operation.get` 返回 `got_ip` 时，`network.status` 可能仍显示 `sta_connected=true / sta_has_ip=false / 0.0.0.0`；最终以 `config.get`/重启后的 `sta_has_ip` 为准，客户端必须等 `sta_has_ip=true`；
- apply success 不等于 MQTT 已连，脚本会再等 5s 查询一次；
- 同 SSID（CMCC-502）和跨 SSID（CMCC-502→TQ2018）都已通过；
- **P0-4**：失败候选后的 profile 回写缺陷已修复；0/4/12/20s、重启和成功交错回归通过，见 `p0_4_regression_<timestamp>.json`。

## 静态 IP 脚本

`static_ip_check.py` 验证 v0 `config.set` 的静态 IP 字段（v1 `config.set` 会拒绝 network 域）：

```bash
.venv-bleak/bin/python -u tools/ble_p0/static_ip_check.py --ssid TQ2018
```

- 默认从当前 DHCP IP 推导同 /24 的 `.213`、网关 `.1`、掩码 `/24`、DNS 网关/8.8.8.8；
- 静态字段 effect 是 reboot；脚本等待重启、重连，验证 `config.get` 持久化和 `network.status` 的真实静态 IP；
- v0 命令含 token 后约 **275B**，超过 MTU-3（253B）；harness 默认按 **20B 分片**（MTU23 安全值），MTU256 可用 `--max-write` 调大。客户端/小程序应按实际协商 MTU-3 计算，不要固定某个值；
- 若运行时静态 IP 失败，默认回退 DHCP 并复验；
- 结果写入 `tools/ble_p0/results/static_ip_<timestamp>.json`。

## P0-4 回归脚本

`p0_4_regression.py` 用于验证“正式配置只由 apply/config.set 修改”：

```bash
# 先跑 static_ip_check.py 建立 TQ2018/static .43.213 基线
IOTHUB_WIFI_PASSWORD='<WIFI_PASSWORD>' \
  .venv-bleak/bin/python -u tools/ble_p0/p0_4_regression.py
```

- 错误候选失败后，在 0/4/12/20s 检查正式 SSID/静态字段/IP/MQTT；
- 重启验证持久化；
- 失败候选后紧接成功 apply，验证旧延迟清理不会覆盖新提交的 TQ2018 DHCP `.63`；
- 结果写入 `tools/ble_p0/results/p0_4_regression_<timestamp>.json`。

## T2 槽位生命周期脚本

`t2_slot_reclaim.py` 从硬复位、空 operation 表开始，验证 4 槽上界和 600s 终态回收：

```bash
.venv-bleak/bin/python -u tools/ble_p0/t2_slot_reclaim.py --ssid CMCC-502
```

- 不 apply，也不需要正确的 Wi-Fi 口令；用一个可见 AP + 随机错误口令，让 A 候选保持 `connecting`；
- A active 时 B 应立即 `busy`；A 终态后 C/D/E 各 start 后立即 cancel，填满 4 个终态槽；
- 第 5 个 F 应为 `no_slot`；A 旧 ID 从终态 +590s 起每 2s 查询，600s 后变 `not_found`，F2 随即 accepted；
- C 是 cancel 终态，同样在 600s 后变 `not_found`，G 随即 accepted；
- 默认 `--retention 600 --margin 15`；worker 每 1s 回收，不要把 600.000s 当成唯一判据；
- 结果写入 `tools/ble_p0/results/t2_slot_reclaim_<timestamp>.json`，包含每个 operation 的 ID、状态、创建/终态/回收观测时间。

2026-10-23 结果（`t2_slot_reclaim_20261023_232220.json`，14/14 PASS）：A 在 +600.526s 仍 `failed`、+602.729s `not_found`；F2 accepted；C 在 +602.866s `not_found`，G accepted。首轮 `231105` 的 F2 探针未立即取消，C 边界观测被推迟到 615.2s，不作为边界证据。

## T3 BLE CMD/DATA 通道脚本

`t3_ble_channels.py` 同时驱动 CMD RX/TX 和 DATA RX/TX：

```bash
.venv-bleak/bin/python -u tools/ble_p0/t3_ble_channels.py \
  --ssid CMCC-502 --max-write 20 --data-max-write 20
```

- 连接后先枚举 GATT 服务/特征，确认 DATA RX/CMD RX 支持 write/without-response、DATA TX/CMD TX 支持 notify 且有 CCCD；
- 通过现有 `data.loopback` 命令打开 BLE→BLE 回环（运行期，不落 NVS），验证 DATA 单帧/长帧/重复/连续/退订/断连；
- CMD 侧验证 275B 合成命令（未知方法，不改配置）、1186B 长响应、长响应期间 `operation.get`、CMD+DATA 并发；
- `--data-max-write` 是单次 DATA 边界写；MTU23 传 20，MTU256 可传 240；
- `--no-require-opget-during` 仅用于 MTU256 复核：长响应约 100ms，opget 请求通常落在结束后，严格 MTU23 验收不要关闭该要求。

MTU23 测试构建：临时改 `sdkconfig` 的 `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=23` 和 `main/bt_gatts_nimble.c` 的 `ble_att_set_preferred_mtu(23)`，构建/烧录后跑脚本；完成后恢复两处为 256，重新 build/flash。T3 文档：`docs/T3_BLE数据通道验收_2026-10-23.md`。

2026-10-23 结果：MTU23 `t3_ble_channels_20261023_234439.json` 功能 15/15 PASS（CMD 通知 20B；DATA 200B/600B 回环一致；`operation.get` 在长响应期间发出）；MTU256 恢复复核 `235335` 功能 15/15（CMD 253B、DATA 240B 边界）。**DATA 是 best-effort 字节管道，无应用 ACK/序号/重传/帧重组/流控；断连无显式 `data_pipe` flush。** 不能据此宣称端到端可靠。AP schema 清理后 T3 smoke `t3_ble_channels_20261024_000822.json` 仍 15/15，`config.get` 长响应降为 1107B。

## T4 微信小程序交互协议

小程序侧完整协议（扫描/订阅顺序、20B 写入、CMD 分片重组、登录/token、配网 operation、错误码、DATA 边界、iOS/Android 测试矩阵）：

[`docs/BLE微信小程序交互协议_T4_2026-10-24.md`](../../docs/BLE微信小程序交互协议_T4_2026-10-24.md)

关键约束：先订阅 CMD TX 再发命令；20B 保守分片；`config.get` 长响应按 JSON 边界重组；token 600s 且断连失效；`operation.get` 轮询到终态；apply 后等 `sta_has_ip`/MQTT；DATA 无应用 ACK。T4 需 iOS/Android 各一台真机。

## 6.0.3 全量 P0 与审计

全量套件（IDF 6.0.3 构建、TQ2018）：

```bash
IOTHUB_WIFI_PASSWORD='<WIFI_PASSWORD>' \
  .venv-bleak/bin/python -u tools/ble_p0/p0_acceptance.py --ssid TQ2018
```

清槽后结果 `p0_20261023_222123.json`：scan/long_scan/long_config/operation/wrong_password/cancel_race/not_found/final_config_set 8/8 PASS，P0 core true，phone UI false。AP 删除后 app 1,140,624B；AP schema 清理后 1,139,568B。注意：

- 10 连扫有 1 次主机断链，自动重连后 10/10 完整；
- operation.get 本次在 MTU 256 下命中 `sent_before_long_complete=true`（558.4ms，state connecting），但试连终态因主机断链被 cancelled；apply 未执行，T1 成功 apply 由独立脚本覆盖；
- 全量错误口令项返回 `failed/connect_failed` 0.27s；独立 TQ2018 错误候选曾返回 `wrong_password` 13.545s；
- 全量后设备正式配置仍是 TQ2018，但运行时回退到 CMCC-502 `.28`；随后 T1 恢复返回 `no_slot`（4 槽已满），复位后回到 TQ2018 DHCP `.63`、MQTT。

正式配置写入审计：

- 允许：`config_apply_patch`（apply/config.set）、apply 快照/回滚、boot 默认/迁移、显式 `auth.admin.set`；
- 自动重连/DHCP/断线/profile 不再写 `s_config.sta_*`；profile 保存只在显式 apply/config.set；
- AP 自动回退已删除：启动 STA 失败只记录原因并保持 STA，等待 BLE 救援；不再修改/持久化 `net_mode`，也没有 AP netif/DHCPS；
- 审计中补了静态守卫 fast path 失败后的 `s_wifi_sta_connecting=false`，避免 boot monitor 不再重试；
- 非静态 auto-connect 仍可回退到其他 profile，但正式配置不变；`network.status.sta_ssid` 是正式配置、`sta_ip` 是运行时，回退时可能不成对；静态模式禁止跨 profile 回退（设计取舍）。

## BLE 独立救援（AP 回退已删除）

`ble_rescue.py` 验证 STA 失败、无 IP、MQTT 断开时 BLE 是否仍能完成救援：

```bash
IOTHUB_WIFI_PASSWORD='<TARGET_WIFI_PASSWORD>' \
  .venv-bleak/bin/python -u tools/ble_p0/ble_rescue.py --target-ssid TQ2018
```

修复前历史（`ble_rescue_20261023_210816.json`）：BLE 可连接、status/config 可读，坏 SSID 下 `scan ok=true, count=0, aps=[]`，目标不可见，**救援在 scan 处 FAIL**。等待 >90s 后当时固件的 AP 自动回退把 `net_mode` 改为 AP 并保存；随后 BLE 从 AP 状态 `tryconnect → apply` 恢复 TQ2018（`success_path_20261023_211113.json`）。这说明 AP 回退停掉了 STA 自动重连，才让 BLE scan 可用；不是 AP 提供了配置通道。该 AP 回退现已删除，不能作为当前行为。

修复后结论（PASS）：

- BLE `network.scan`/`tryconnect` 已取得 Wi‑Fi 独占权（suspend auto-connect、等待 connect task、串行 scan）。
- AP 运行时已删除：无 AP netif/AP/APSTA/自动回退/`net_mode=AP` 持久化；`net_mode` 只接受 STA；softAP/DHCPS 关闭；AP 删除后 app `1,140,624B`。
- AP 配置 schema 已清理：`config.get` 无 AP 字段；`config.set` 的 AP 字段被忽略（`changed=[]`）；`net_mode=ap` 拒绝；旧 NVS 布局保留、STA 配置不丢；app `1,139,568B`。
- 有效救援（`ble_rescue_20261023_221537.json`）：CMCC-502 错误 profile → 100s 无 IP/MQTT、mode=sta → BLE scan 命中 → tryconnect/apply → `.28`/MQTT。
- 首次用 TQ2018 作 target 时 TQ2018 热点不可见且设备回退到 CMCC-502；该次失败是场景问题。
- 清槽后 6.0.3 全量 `p0_20261023_222123.json` 8/8 PASS；首次 7/8 的 cancel_race `no_slot` 是 T2 槽位保留。
- 下一步：T4 iOS/Android 微信真机；串口维护通道（UART0/USB 或独立 UART2，复用 v1 JSON）另行安排。DATA 若要可靠传输，需要另立应用层协议设计。

## 结果

P0 全套写入 `tools/ble_p0/results/p0_<timestamp>.json`，包含：

- 过滤扫描命中与 ADV 字段；
- 名称观察（BlueZ 只暴露完整名时 `short_name_seen=false`）；
- 10 次长响应的 bytes/frags/id/attempts；
- tryconnect、operation.get、apply/cancel 的响应与时间线；
- 错误口令、cancel 竞态、not_found；
- 最终 config.set 响应。

T1 写入 `tools/ble_p0/results/success_path_<timestamp>.json`，包含：

- before `network.status` / scan 目标可见性；
- tryconnect、17 次 operation.get 状态序列、got_ip；
- apply / 重复 apply / 成功后 cancel；
- apply 后 `config.get`（`sta_use_static`/IP）、MQTT 重查；
- 重启后的 SSID/IP/MQTT 持久化验证。

静态 IP 写入 `tools/ble_p0/results/static_ip_<timestamp>.json`；P0-4 回归写入 `p0_4_regression_<timestamp>.json`；T2 写入 `t2_slot_reclaim_<timestamp>.json`；T3 写入 `t3_ble_channels_<timestamp>.json`；AP schema 写入 `config_schema_<timestamp>.json`。修复前的回滚 FAIL 证据保留在 `rollback_after_success_20261023_200845.json`。6.0.3 全量结果见 `p0_20261023_204921.json`；全量后 `no_slot` 证据见 `success_path_20261023_205143.json`；BLE 救援 FAIL 见 `ble_rescue_20261023_210816.json`；AP 状态恢复见 `success_path_20261023_211113.json`；T2 PASS 见 `t2_slot_reclaim_20261023_232220.json`；T3 MTU23 PASS 见 `t3_ble_channels_20261023_234439.json`，MTU256 恢复见 `t3_ble_channels_20261023_235335.json`，AP schema 后 T3 smoke 见 `t3_ble_channels_20261024_000822.json`；AP schema 见 `config_schema_20261024_001220.json`。

完整验收记录与问题清单：`docs/P0_BLE验收记录_2026-10-23.md`。
