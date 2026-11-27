# C15-2 BLE 断链证据包（2026-11-28）

> 证据分支：`evidence/c15-ble-disconnect-20261128`，基于 `main@5aaabea`。
> 本包只含脱敏文本、测试脚本、烧写/恢复日志和结果 JSON；不含 NVS、app 镜像或凭据。
> 设备实验结论：**FAILED / STOPPED**，已恢复生产镜像和原配置。

## 1. 设备与镜像

| 项 | 值 |
|---|---|
| 设备 | `IotHub-65D8` / `IotHubBLE-65D8` |
| 适配器 | `1a86:7523` CH340，路径 `pci-0000:00:14.0-usb-0:1:1.0`；端口 `/dev/ttyUSB0` |
| 测试镜像 | `1.3-65f6dbb`，`build_sha 981fd6552`，BIN `43deb92e9683c78ab6848f1a954d585f0dd51f6b1decc4ec6b64d9d2363b44a1` |
| 生产镜像 | `1.3-956afcd`，`build_sha c231c9501`，app 备份 `c6f007f771211710a3b17d1758ebf90fe88eba8f2c97eafdb3799326439aa94c` |
| 分区表 | `bccd1a3e4c07d0d2e0bdfc02438282cd1280016b4a64d2bd4eb8a13dc72ce88e` |
| otadata | `8ba3b110139f45443d4f268d1a3373ef99a1718b71d51664531b83ee2d4b91a3` |
| NVS | `f875681087da135c1b91cc2284859092ef47733ff48b0849f3f501d0af48d13b` |

## 2. 脱敏说明

串口和 BLE 日志中的 SSID、STA IP、网关、BSSID、设备 MAC、host BT 地址、MQTT client_id 已替换为占位符。BLE token 由测试脚本日志自身脱敏。原始文件只在本地 `/tmp/c15_device_20261127/`，未上传；`MANIFEST.json` 保留原始文件与脱敏文件的 SHA256，便于核对。

## 3. 两次实验时间线

### Run 1（`serial/run1_serial.log`）

| 设备 uptime | 事件 |
|---|---|
| 22194 | 验证会话正常断链，reason `0x213`（Remote User Terminated）；不是实验断链 |
| 75644 | 实验 BLE central 连接 |
| 80524 | `network.tryconnect` 受理 `op-AB0EA01287CF49B3` |
| 98234 | `C15 hook armed`，target 同 op |
| 115674 | `central disconnected (reason=0x222)`；距连接 40.03 s，距 tryconnect 35.15 s，距 arm 17.44 s |
| 115704 | `C15 hook: target_cancelled` |
| 115714 | `finish ... -> cancelled/cancelled`；`async network operation cancelled on BLE disconnect` |

### Run 2（`serial/run2_serial.log`）

| 设备 uptime | 事件 |
|---|---|
| 466914 | 实验 BLE central 连接 |
| 471654 | `network.tryconnect` 受理 `op-69491E56CE4AF662` |
| 489514 | `C15 hook armed` |
| 496524 / 502284 | 5 s `device.info` 心跳被设备处理 |
| 506954 | `central disconnected (reason=0x222)`；距连接 40.04 s，距 tryconnect 35.30 s，距 arm 17.44 s |
| 506974 | `C15 hook: target_cancelled` |
| 506984 | `finish ... -> cancelled/cancelled` |
| 506994 | `async network operation cancelled on BLE disconnect` |

**关键修正**：用户先前汇总的“第一次实验 0x213、第二次 0x222”不准确。`0x213` 发生在第一次实验前的独立验证会话，且是正常断开；两次实验断链均为 `0x222`。两次断链都发生在 BLE 连接后约 40 s、tryconnect 后约 35 s、arm 后约 17.4 s；arm 与断链的相对时间固定，但更靠近“连接后约 40 s”这一共同边界。

## 4. reason 来源

IDF 源码：

- `BLE_HS_ERR_HCI_BASE = 0x200`（`ble_hs.h`）
- `BLE_HS_HCI_ERR(x) = 0x200 + x`
- `0x213 = 0x200 + 0x13` → `BLE_ERR_REM_USER_CONN_TERM`（Remote User Terminated Connection）
- `0x222 = 0x200 + 0x22` → `BLE_ERR_LMP_LL_RSP_TMO`（LMP Response Timeout / LL Response Timeout）

日志中 `app_bt: conn params: status=531` 的 531 也是 `0x213`，出现在验证会话；随后该会话被 central 主动断开。

## 5. operation 取消调用链（源码事实）

```text
NimBLE disconnect event
  -> bt.c: BLE_GAP_EVENT_DISCONNECT
  -> bt_gatts_nimble.c: app_ble_gatts_nimble_on_disconnect(reason)
  -> ble_cmd.c: app_ble_cmd_on_disconnect()
  -> cmd.c: app_cmd_net_on_ble_disconnect()
  -> app_cmd_net_job_disconnect_target()（仅 BLE 源 job）
  -> app_operation_request_finish(CANCELLED, CLEANUP_REQUIRED)
  -> finish_fn -> cleanup（restore_persistent）
  -> operation terminal CANCELLED
  -> C15 notify：target_cancelled，清除已 armed 目标 hook
```

- `expire task started` 已在两次运行中记录；目标 operation 的 GOT_IP deadline 尚未到达（disconnect 约 115 s / 507 s，deadline 约 395 s / 786 s）。
- 因此没有进入 `expire_post_cleanup` pause：不是 expire task 未启动，也不是 hook 匹配失败，而是 BLE 断链先触发了 **CANCELLED** finish；`pause_expire` 对 CANCELLED 为 false。
- `app_operation_request_finish` 的 cleanup 在 disconnect 回调路径同步执行；日志 `rescue=restore_persistent`、`CLEANUP_DONE_OK` 对应。

## 6. BLE 连接参数观察

- 首次连接（验证会话）：central 参数 `45 ms / latency 0 / supervision 420 ms`；设备请求 `24-40`（30-50 ms）/ `timeout=600`（6 s）；随后 `CONN_UPDATE status=0x213`，host 主动断开。
- 两次实验会话：连接时同样打印 `conn params policy ... requesting update` 和 NimBLE `connection parameter update ... supervision_timeout=600`；但没有看到 `BLE_GAP_EVENT_CONN_UPDATE` 完成日志。
- 两次实验均在 BLE 连接后约 40 s 出现 `0x222`（LL/LMP response timeout），此前有 5 s 心跳（Run 2）仍发生。
- 未出现 `idle terminate` 日志；因此这不是 `APP_BLE_IDLE_TERMINATE_*` 的空闲回收路径。

## 7. 恢复核验

| 对象 | 回读结果 |
|---|---|
| app | 与 `c6f007...` 完全一致 |
| NVS | 与 `f8756810...` 完全一致（测试中曾变为 `b172397e...`，已恢复） |
| otadata | 与 `8ba3b110...` 完全一致 |
| 分区表 | 与 `bccd1a3e...` 完全一致 |
| 固件 | `1.3-956afcd` / `c231c9501` / `IotHub-65D8` |
| 运行态 | STA connected、IP、MQTT connected、internet_verified、trial idle |

## 8. 文件索引

- `serial/run1_serial.log`、`serial/run2_serial.log`：两轮完整串口日志（脱敏）
- `ble/run1_ble.log`、`ble/run2_ble.log`：BLE 客户端日志（token 已脱敏）
- `ble/run1_result.json`、`ble/run2_result.json`：脚本结果
- `flash/flash_test.log`、`flash/flash_restore.log`、`flash/flash_nvs_restore.log`：烧写/恢复
- `scripts/`：两轮测试脚本
- `MANIFEST.json`：原始/脱敏文件 SHA256
