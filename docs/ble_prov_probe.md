# BLE 配网探针（路线 B）

独立分支：`probe/ble-prov`。**不合并到主分支，不删除 Web/AP。**

## 结论（已在本机验证）

- `network_provisioning 1.2.4` 在 ESP-IDF v6.0.2 + NimBLE 下编译通过。
- 探针固件 `iothub.bin = 0x14d680`，主 app 分区剩余 31%。
- 未配网时进入配网专用启动：只启动 Wi-Fi STA、NimBLE/protocomm、UART console；
  **没有**调用 `app_start_wifi()`、`app_captive_init()`、`app_bt_apply_config()`、
  `app_mqtt_start()`、`app_http_start_webserver()`、`data_pipe_init()`。
- B 板实测堆：

| 阶段 | free | largest | internal_free | dma_free |
| --- | ---: | ---: | ---: | ---: |
| 配网启动前（Wi-Fi/BLE 前） | 155,824 | 110,592 | 167,688 | 155,824 |
| 配网运行中 | 71,892 | 69,632 | 83,756 | 71,892 |

- BLE 广播名：`PROV_IOT_<MAC 后 4 位>`（本次 `PROV_IOT_65DA`），Security 1。
- 正常固件恢复后：HTTP 200，`free=15,532 / largest=6,912`。

## 未完成（本机无 BlueZ/手机）

- 官方手机 App 完成配网、错误 PoP 被拒、错误密码/AP 不存在/超时。
- 配网成功后写入 `app_config_t`、重启、正常固件的完整链路。
- 重复进入配网（当前需清 NVS 后重刷探针）。

## 配置写入

官方 `network_provisioning` 会把凭据写到 Wi-Fi 驱动的 NVS，而 iothub 读自己的
`app_config_t`。探针在 `NETWORK_PROV_WIFI_CRED_RECV` 保存 SSID/口令，
在 `IP_EVENT_STA_GOT_IP` 后用 `app_config_save()` 写入主配置，设置
`net_mode=NET_MODE_APSTA`、`sta_use_static=0`，并写 `ble_prov/done` 标记；
随后重启。只有“拿到 IP + 配置保存成功 + 标记成功”三者都成立才重启。

不记录 Wi-Fi 口令；串口只打印 SSID。

## 蓝牙名称区分

- **正常固件**：`IotHubBLE-<Wi-Fi STA MAC 后 4 位>`，例如当前 B 板 `IotHubBLE-65D8`。
  由 `main/identity.c` 的 `APP_BT_BLE_PREFIX` 生成，和配网探针无关。
- **配网探针**：`PROV_IOT_<BT MAC 后 4 位>`，例如 `PROV_IOT_65DA`。
  只有烧录 `probe/ble-prov` 分支、且 NVS 中没有 `ble_prov/done` 标记时才会出现。

因此，扫描到 `IotHubBLE-...` 说明当前跑的是正常固件（或手机缓存了旧设备名），
不是配网模式；这不代表探针的 `PROV_IOT_...` 广播失败。

## 使用

```bash
# 构建
cd /home/tc/workspace/iothub-ble-prov-probe
tools/idfbuild.sh build

# 烧录（B 板；115200 是该 CH340 的稳定速率）
tools/idfbuild.sh app-flash -p /dev/ttyUSB1 -b 115200

# 串口日志
tools/capture_serial.py /dev/ttyUSB1 /tmp/ble_prov_probe.log
```

手机使用 ESP BLE Provisioning App：

- 设备名：`PROV_IOT_65DA`（以实际日志为准）
- Security：1
- PoP：`probe-pop-1234`（测试值，不是生产密钥）

预期串口关键行：

```text
ble_prov_probe: starting BLE provisioning: service=PROV_IOT_65DA security=1
ble_prov_probe: wifi credentials received (ssid=...)
ble_prov_probe: sta got ip: ...
ble_prov_probe: credentials committed; rebooting into normal firmware
```

重启后应看到正常固件的 `[boot-5] ...`、Wi-Fi、BLE、MQTT、HTTP 日志。

## 恢复主固件

```bash
cd /home/tc/workspace/iothub
tools/idfbuild.sh app-flash -p /dev/ttyUSB1 -b 115200
```

## 重新进入配网

探针以 NVS `ble_prov/done` 判断是否已配网。重新测试前可擦除 NVS（会清掉设备配置）：

```bash
python -m esptool --chip esp32 -p /dev/ttyUSB1 erase-region 0x9000 0x4000
```

然后重新烧录探针。生产实现应改为 BOOT 长按/本地物理动作触发，不应依赖擦除 NVS。

## 设计边界

- 本探针验证路线 B：配网阶段官方组件独占 NimBLE，成功后重启，正常固件继续使用现有 NimBLE。
- 路线 A（官方 provisioning GATT 与自定义 NUS 同时驻留）未实现；官方 `protocomm_nimble.c`
  自己调用 `nimble_port_init()`，与 `bt.c` 的初始化冲突。
- Security 2 仍保留在 sdkconfig 中；本轮只验证 Security 1 + PoP，未关闭 Security 2。
