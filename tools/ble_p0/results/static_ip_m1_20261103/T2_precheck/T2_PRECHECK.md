# T2 前置只读核查（C8E4，2026-11-03）

> 本轮仅读取：串口监听、esptool read_mac / read_flash、主机 ping 探测。未写 NVS、未刷固件、未改配置。

| 项目 | 实测 | 结论 |
| --- | --- | --- |
| 设备身份 | `/dev/ttyUSB1` → MAC `b0:a7:32:28:c8:e4`（client_id `iothub-B0A73228C8E4`） | 与 C8E4 一致 |
| 当前固件 | `App version: 1.2-a35eef9`，compile `Nov 1 2026 23:35:37`，启动分区 `ota_0`（offset 0x10000） | **非 M1 构建**（pre-M1 基线） |
| 当前网络模式 | `STA_CONNECTED: dhcpc restarted`；GOT_IP `192.168.1.28`；MQTT connected | **DHCP，静态未启用** |
| 静态字段现值 | 无（未进入 static 分支，日志无 `static ip configured`） | 无合法/非法静态配置可评估 |
| 候选 BIN | `build/iothub.bin` sha256 `b0e6d6c7…`（M1 正式，FUELT ON） | 文件与批准构建一致 |
| 网关 | `192.168.1.1` 存活（主机默认路由） | 可用 |
| 候选地址探测 | `192.168.1.50`、`192.168.1.60` 无应答（主机 .19） | 未发现占用（仅 ICMP 探测，非充分证明） |

## 结论

C8E4 硬件可用，但 **T2 前置条件未满足**：当前为 DHCP + pre-M1 固件。执行 T2 需要三件写操作，均未获批准：

1. 刷 M1 正式构建 `b0e6d6c7…`（ota_0，需确认 otadata/启动槽）；
2. 写入合法静态配置到 NVS（当前无静态字段；DHCP 池范围未知）；
3. 重启并验证 S1。

写入静态配置的通道：BLE v0 `config.set`（需 `IOTHUB_ADMIN_PASSWORD`，当前未提供）；或经批准的 NVS 直写（不推荐，需额外工具与恢复方案）。

## 建议的 T2 参数（待确认）

| 字段 | 建议值 | 依据 |
| --- | --- | --- |
| IP | 192.168.1.50 | 网关同网段；ICMP 无应答；需确认不在 DHCP 池内 |
| 掩码 | 255.255.255.0 | 与主机 /24 一致 |
| 网关 | 192.168.1.1 | 主机默认网关存活 |
| DNS1 | 192.168.1.1 | 与网关一致（若网关不提供 DNS，改用 223.5.5.5） |
| DNS2 | 空 | 验证空值处理（M2 范围，T2 不强制） |

## T2 执行草案（未执行）

1. 确认 DHCP 池范围，保证 .50 不在池内；
2. 提供 BLE 口令（或批准 NVS 直写方案）；
3. 刷 M1 正式构建并冷启动，确认 `apply_begin mode=dhcp` 基线；
4. BLE v0 `config.set` 写入上表字段（`sta_use_static=true`），确认 `restart_required=true`，NVS 保存；
5. 冷启动，采集 ≥120s：断言 `apply_begin mode=static source=boot`、`static ip configured: ip=192.168.1.50 ...`、GOT_IP `192.168.1.50`、MQTT connected、`net_config_state=static`，无 `apply_failed`/救援/离线；
6. 失败回滚：BLE `config.set` 恢复 `sta_use_static=false` 或按批准方案恢复 NVS 快照，重启验证 DHCP。

## 证据

- 串口日志：`c8e4_boot_readonly.log`（sha256 `54b7e0af…`）
- 心跳：MQTT broker `117.131.99.2:21883`

---

## 追加：BLE 只读核查（登录 + config.get + network.status）

- BLE 地址：广播 `B0:A7:32:28:C8:E6`（基础 MAC `B0:A7:32:28:C8:E4` +2）；此前按基础 MAC 连接会 `device not found`。
- `auth.login`（v1）成功；随后 v0 `config.get` / `network.status` 正常返回。口令仅用于本次登录，未写入任何文件。
- 当前 NVS 关键字段：
  - `sta_use_static=false`（静态未启用，运行时 DHCP，`sta_ip=192.168.1.28`）；
  - **残留静态字段为旧网络值**：`sta_static_ip=192.168.43.213`、`gw=192.168.43.1`、`mask=255.255.255.0`、`dns1=192.168.43.1`、`dns2=8.8.8.8`。
  - 含义：T2 若只置 `sta_use_static=true` 而不改字段，会得到"应用成功但网络不可达"（B 类），不能作为 S1 用例；必须五字段整体更新。
- `config.get` 的 `network.sta_password_set=true`；MQTT `connected=true`。

## 追加：网络前置补充探测（2026-11-03）

| 项 | 实测 | 影响 |
| --- | --- | --- |
| 网关 DNS | `dig @192.168.1.1` → **connection refused** | DNS1 不能用 `192.168.1.1` |
| 公共 DNS | `223.5.5.5` / `119.29.29.29` / `114.114.114.114` 均可解析 | DNS1 建议 `223.5.5.5` |
| `.50` 占用 | ICMP 无应答；本机无 arping/nmap，无法做 ARP 级确认 | 仍需路由器 DHCP 客户端列表确认 |
| `empty_keeps` | 静态字符串字段空串=保持旧值 | 不能用 `""` 清 DNS2；T2 应显式写合法值（如保留 `8.8.8.8` 或写 `119.29.29.29`） |

## 修订后的 T2 参数（待批准写入）

| 字段 | 值 | 说明 |
| --- | --- | --- |
| `sta_use_static` | `true` | 与五字段同一请求提交 |
| `sta_static_ip` | `192.168.1.50` | 用户确认可用；建议路由器侧再核 DHCP 池 |
| `sta_static_netmask` | `255.255.255.0` | |
| `sta_static_gateway` | `192.168.1.1` | 网关存活 |
| `sta_static_dns1` | `223.5.5.5` | 网关 53 端口拒绝，改用公共 DNS |
| `sta_static_dns2` | `8.8.8.8` | 沿用现值（合法）；空串无法清除，M2 处理 |

结论：T2 前置信息已齐，但 **刷写与 NVS 写入仍未执行**，等待批准。
