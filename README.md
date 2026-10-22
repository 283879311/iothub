# iothub

基于 ESP-IDF(v6.0,目标芯片 ESP32)的物联网控制与现场调试网关:本地管理走 BLE NUS 命令面,数据面含 UART/BLE/MQTT,云端为 ThingsBoard MQTT,GPIO 控制与状态上报由共享命令层承载。**Web/HTTP server、captive portal 和 Web OTA 已按产品决定删除**;OTA 核心保留但当前没有 HTTP 传输入口。

当前固件版本:**1.2**(`CMakeLists.txt` → `set(PROJECT_VER "1.2")`)。v1.2 起规划**统一命令解析层(控制面 / 数据面双平面)+ MCP 兼容**,把 MQTT、串口、BLE 收敛到同一套命令语义(经典蓝牙 / SPP 已于本版本移除),详见 `问题与版本优化.md`。

## 功能

- **UART 数据面**:UART 参数配置与原始字节收发,数据经 `data_pipe` 在 UART/BLE/MQTT 间路由;Web 串口调试台已删除。
- **GPIO/命令层**:继电器、LED、触摸输入统一走 `cmd.c`;MQTT RPC 与 BLE CMD 共用 `led.set/led.toggle/relay.set/io.set/io.get` 及旧别名;触摸按下调用同一 `led.toggle`,长按不重复;每路可配置上电策略,引脚由 Kconfig 统一配置。
- **BLE 本地管理**:NUS CMD 通道提供登录、配置、Wi-Fi 扫描/试连/提交、MQTT 配置、GPIO/UART 命令;Web 管理页和强制门户已删除。
- **网络**:STA 为正常目标;BLE 配网提交显式写入 STA;旧 AP/APSTA 配置仍兼容但不再有 Web 门户。最多 5 条 STA profile,RSSI 择优自动重连。
- **MQTT(ThingsBoard)**:10s 周期遥测(继电器/LED/输入/堆内存)+ attributes 上报;下行 RPC `setRelay/setLed/getStatus/setState` 控 GPIO;支持 tb_cloud / tb_selfhost / 自定义 topic,TLS(证书 bundle,可选)与 LWT;数据面自 1.3-82 起走 QoS1。
- **OTA**:OTA 确认/回滚核心保留;HTTP 上传入口和网页资源分区已删除,当前没有 OTA 传输通道,后续需通过 BLE/MQTT 等新通道接入。
- **恢复出厂**:BOOT 键(GPIO0)长按 5s 后 LED 闪烁提示；松开后 LED 恢复原状态，设备清除默认配置、已保存 Wi-Fi 列表和配网 PIN/状态，再重启。
- **蓝牙**:BLE NUS GATT 数据面(peripheral 从机 + central 主机),支持数据 RX/TX 与 CMD 通道(命令写 `6E400004`、回包订阅 `6E400005`;先 `login` 换 token,回包在 `result.token`);默认 NimBLE 主机栈,可回退 Bluedroid;A/B 双向角色互换脚本 `tools/ble_nus/role_swap.py` 已通过(1.3-81);经典蓝牙 / SPP 已于 v1.2 移除(CONFIG_BT_CLASSIC_ENABLED=n)。

## 硬件引脚(编译期固定,`main/constants.h`)

| 功能 | GPIO |
| --- | --- |
| 继电器 | 16 |
| 状态灯 | 4 |
| 输入检测 | 27(T7 触摸焊盘) |
| UART1 TX / RX | 17 / 18 |
| BOOT 键(恢复出厂) | 0 |

## 构建与烧录

推荐用仓库内的统一入口 —— 它会自动定位 ESP-IDF / 工具链 / venv,并把 `ninja` 放进 PATH
(否则 ESP-IDF 会回落到 `make` 并报生成器不匹配),同时**自动清理**被 VS Code CMake 扩展污染的 `build/` 缓存:

```bash
tools/idfbuild.sh              # 默认 profile = NimBLE(1.3-62 起,稳定性优先)
tools/idfbuild.sh flash        # 烧录(可后接 -p /dev/ttyUSB0 等 idf.py 参数)
tools/idfbuild.sh monitor
tools/idfbuild.sh paths        # 自检:ninja / cmake / toolchain / idf.py / python 的定位结果
tools/idfbuild.sh cache-guard  # 仅清理生成器不匹配的 build/ 缓存
```

GPIO 引脚由 Kconfig 统一配置（`main/Kconfig.projbuild` → `Iothub Configuration` → `GPIO pin assignment`），默认继电器 16、状态灯 4、输入 27(触摸 T7)。换脚只需菜单配置/修改 `sdkconfig` 后重新构建烧录，不需要改 `state.c` 或其它源码；GPIO34–39 不能作为输出，构建期会拒绝。

默认 BT 主机栈为 **NimBLE**。需要回退 Bluedroid 时用独立的回退 profile:

```bash
IDF_BUILD_DIR=build-bluedroid tools/idfbuild.sh build
IDF_BUILD_DIR=build-bluedroid tools/idfbuild.sh flash -p /dev/ttyUSB0
```

回退 profile 以 `sdkconfig.bluedroid` 为完整种子(保留 ACL=1 / SMP 关 / GATT profiles=4 等裁剪),
构建结束会自动恢复根 `sdkconfig` 的 NimBLE 配置;不要用 `idf.py -B <其他目录>` 直接构建,
避免把 profile 配置写回根文件。

等价的原生命令(需自行保证环境已激活、`ninja` 在 PATH 中):

```bash
idf.py set-target esp32
idf.py build
idf.py -p PORT flash monitor
```

## 分区表(`partitions.csv`)

nvs 16K / otadata / phy / **ota_0 + ota_1 各 1.9375MB(双 OTA)** / storage(原网页资源分区,现未使用)32KB / coredump 32KB。

> 布局要点(2026-10-08):APP 分区偏移必须 64K 对齐(`gen_esp32part.py`);storage 分区现在不再有 LittleFS 镜像,仅保留占位,后续可单独回收。

## 首次使用（BLE-only）

1. 连接 BLE NUS 设备 `IotHubBLE-XXXX`（XXXX 为 MAC 后缀），订阅 CMD TX `6E400005`；
2. 在 CMD RX `6E400004` 发送 `login`，用户名/口令为本地管理凭据（默认 `admin / 123456`，可经 BLE `auth.admin.set` 修改）；
3. 用 `network.scan` 扫描，`network.tryconnect` 验证上游 WiFi，再用 `network.apply` 提交；提交会显式进入 STA；
4. 用 `config.set` 配置 ThingsBoard MQTT，设备获取 STA IP 后自动连接并上报。

## 目录结构

```
main/            固件源码(模块划分见各 .h 接口)
tools/           宿主侧脚本与单测(协议/JSON 转义单测、BLE NUS/角色互换验收;**不进固件**)
问题与版本优化.md  唯一主记录:v1.0–v1.2.1 历史 + v1.3 增量/清单 + 附录 A–E
docs/soak_test_plan.md  全链路 soak 方案(独立)
docs/archive/     原始日志/历史归档(只读留证)
```

## 文档导航

- **唯一主记录**:`问题与版本优化.md` —— v1.0–v1.2.1 历史、v1.3 增量与当前清单(**1.3-70**)、附录 A NimBLE 评估、B 串口抓包、C 已解决索引、D UART 调试、E 手机验收测试卡。
- **独立方案**:`docs/soak_test_plan.md`(全链路 soak;`tools/soak/` 尚未实现)。
- **原始归档**:`docs/archive/`(日志/代码,只读留证)。

## 已知问题与安全提示

- **BLE 链路无链路加密，登录 token 是应用层鉴权**。首启默认口令必须修改；NVS 中 WiFi/MQTT/管理凭据保持明文，设备需物理受控。TLS/NVS 加密按既有决定不排期，MQTT 可选 TLS 不受影响。
- UART RX 电气层问题排查记录见 `问题与版本优化.md` 附录 D;
- B 板在高负载下曾出现一次 WiFi/IP 失联(BLE/CMD 仍存活,heap 健康),重新插拔 USB 后 115200/9600 各 1h 未复现;保留后续观察(24h 按当前安排暂缓,1.3-77/78/79)。
- Web/HTTP/captive 已删除；C 65D8 在 STA-only + MQTT 下稳态 heap≈55KB、largest≈49KB，BLE+MQTT+worker 峰值已重新获得余量。
- BLE 真机四态(RX/TX/CMD RX/CMD TX)尚未执行;断连重广播、A/B 双向角色互换(1.3-78/81)已通过,手机 nRF Connect 步骤见 `docs/soak_test_plan.md` §2.4,另一台 Ubuntu 的角色互换/数据面/回传执行包见 §2.5。
- MQTT 数据面旧实现误用 `esp_mqtt_client_enqueue` 的 QoS0/store=false(返回 -1、未真正入队),导致实际漏发远大于 `dropped_mqtt` 记录的 2%;已改 QoS1/store=true,1KB/s 负载 6min 0 丢、outbox 0(1.3-82)。
