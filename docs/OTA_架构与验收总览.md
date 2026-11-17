# OTA 与启动回滚架构及验收边界

> **状态：ACTIVE（当前 1.3 OTA 能力边界）**
> 本文只描述当前代码中已存在的启动回滚确认机制，以及尚未接入的固件传输能力。实现事实以 `main/ota.c`、`partitions.csv` 和 `sdkconfig.defaults` 为准。

## 1. 当前已实现

- 分区表定义 `ota_0` / `ota_1` 两个 OTA app 槽。
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` 启用 bootloader 镜像回滚机制。
- `app_ota_confirm_running_image()` 只对 `ESP_OTA_IMG_PENDING_VERIFY` 状态启动独立健康确认任务；常规启动或已确认镜像不重复执行。
- 健康确认任务先等待至少 15 秒，再在最长 120 秒窗口内检查 STA 是否取得 IP 或 MQTT 是否已连接；满足任一条件即调用 `esp_ota_mark_app_valid_cancel_rollback()`。
- 窗口耗尽则调用 `esp_ota_mark_app_invalid_rollback_and_reboot()`；确认任务创建失败时保持 PENDING_VERIFY，预期在后续重启时由 bootloader 回滚。
- 以上是启动后的镜像健康确认/回滚机制，不是固件下载或升级传输。
- 当前健康判定实际只检查 STA 是否取得 IP 或 MQTT 是否连接；`main/ota.c` 超时日志仍残留 `ap_up` 字样，与 AP/Web 已移除后的真实判定不一致。该项仅是诊断文本陈旧，不代表代码仍以 AP 为健康条件；源码清理应单独处理。

## 2. 当前未接入/未验收

- 当前 `main/ota.c` 未实现 OTA 固件下载、分块接收、镜像写入、镜像校验和升级触发的完整传输链路。
- ThingsBoard MQTT OTA 控制/元数据与固件传输分离的方案尚未在当前产品代码中形成可验收的完整链路。
- Bluedroid 回退构建 profile 的实际升级/回滚及升级后配置保持尚未完成当前 1.3 设备验收；该 profile 不代表已存在 OTA 固件传输能力，当前仍无可用的固件下载/写入链路。
- 恢复出厂、首次启动、NVS 配置保持必须作为独立用例验证，不能由自动回滚代码存在推导为通过。

## 3. 验收与安全边界

1. “双 OTA 槽 + bootloader rollback + app 健康确认”只证明回滚基础设施已接入；不代表 OTA 升级功能已交付。
2. OTA 传输实施前必须单独评审 ThingsBoard 消息控制面、固件数据面、镜像来源校验、分块完整性、断点/失败处理、分区容量和堆/栈预算。
3. 每次设备升级/烧录/回滚验证均需明确镜像 SHA、设备身份、目标槽、操作步骤和回滚判据，并获得独立授权。
4. 本文不授权下载固件、写 Flash、切换启动槽、重启设备或操作串口/BLE。

## 4. 当前状态入口

- CMD 与设备命令状态：[`cmd_refactor_plan.md`](cmd_refactor_plan.md)。
- BLE 设备侧与 NimBLE 兼容性：[`BLE_架构与验收总览.md`](BLE_架构与验收总览.md)。
- 项目文档导航：[`README.md`](README.md)。