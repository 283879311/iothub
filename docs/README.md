# 固件 1.3 文档入口

当前产品行为以源码和 `contract/v1/` 为准。这里只保留日常开发、维护和验收所需入口；历史方案稿与过程记录不作为维护入口。

- **产品与构建**：[根 README](../README.md)
- **命令层**：[CMD 架构与权限](CMD共享命令架构与权限边界.md)、[CMD 当前任务台账](cmd_refactor_plan.md)
- **BLE**：[架构与验收总览](BLE_架构与验收总览.md)、[v1 命令协议](BLE命令协议规范.md)
- **MQTT**：[ThingsBoard MQTT 总览](MQTT_架构与验收总览.md)
- **Wi-Fi**：[网络与 owner fencing 总览](WIFI-OP-RESOURCE-RACE_主文档.md)
- **OTA**：[OTA 与启动回滚总览](OTA_架构与验收总览.md)
- **现场验收**：[S6 当前状态](S6_现场验收总览.md)
- **协议契约**：仓库根目录 `contract/v1/`
- **测试入口**：[`tests/README.md`](../tests/README.md)；BLE 工具见 [`tools/ble_p0/README.md`](../tools/ble_p0/README.md)

专题文档描述当前实现和仍有效的限制。历史测试结果如仍被引用，必须保留原设备、镜像和测试范围，不得外推为当前 PASS。
