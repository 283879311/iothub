# iothub

基于 ESP-IDF(v6.0,目标芯片 ESP32)的物联网控制与现场调试网关:提供 Web 管理界面、UART 串口调试台、ThingsBoard 云接入(MQTT)、GPIO 控制与 HTTP OTA,定位为"底层核心应用与控制"的薄设备端,业务逻辑由手机/电脑端承载。

当前固件版本:**1.2**(`CMakeLists.txt` → `set(PROJECT_VER "1.2")`)。v1.2 起规划**统一命令解析层(控制面 / 数据面双平面)+ MCP 兼容**,把 MQTT、串口、BLE 收敛到同一套命令语义(经典蓝牙 / SPP 已于本版本移除),详见 `问题与版本优化.md`。

## 功能

- **串口调试台**:UART 参数配置(5-8 数据位、奇偶校验、1/2 停止位、任意波特率),收发数据查看(文本/hex)、清屏、手动发送。
- **GPIO/命令层**:继电器、LED、触摸输入统一走 `cmd.c`;MQTT RPC、BLE CMD、Web `POST /api/v1/command` 共用 `led.set/led.toggle/relay.set/io.set/io.get` 及旧别名;触摸按下调用同一 `led.toggle`,长按不重复;每路可配置上电策略,引脚由 Kconfig 统一配置;GPIO 面板 1s 实时刷新输入状态。
- **Web 管理页 `/config`**:网络、MQTT、GPIO、蓝牙、UART 配置,管理员账号修改与固件升级(Basic 认证,可在页面修改口令)。
- **强制门户**:AP 模式下手机/电脑接入后自动弹出配置页(本地 DNS 劫持 + OS 连通性探测 302);mDNS(`.local` 域名)已于 1.3-27 移除(BLE 常驻时堆挤占,局域网内用 IP 直连)。
- **网络**:AP(192.168.8.1)/ STA / APSTA;最多 5 条 STA profile,RSSI 择优自动重连;纯 STA 启动 90s 失败自动降级 AP 并持久化。
- **MQTT(ThingsBoard)**:10s 周期遥测(继电器/LED/输入/堆内存)+ attributes 上报;下行 RPC `setRelay/setLed/getStatus/setState` 控 GPIO;支持 tb_cloud / tb_selfhost / 自定义 topic,TLS(证书 bundle,可选)与 LWT;数据面自 1.3-82 起走 QoS1。
- **OTA**:Web 上传固件(`iothub.bin`),带应用描述区校验(魔数/项目名/版本降级拒绝);网页资源 `storage.bin` 支持运行时整分区替换;升级后经健康确认窗口(业务可达才 mark valid,超时自动回滚)。
- **恢复出厂**:BOOT 键(GPIO0)长按 5s。
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

Web 前端(`web/*.html`)在构建期自动做语法检查并压缩,打包为 LittleFS 镜像随固件烧录,无需手动处理。若构建机装有**可选**的 `zopfli`(`pip install zopfli`),网页资产会用 Zopfli 压缩(体积更小,输出仍是标准 gzip,运行期无需改动);未安装则自动回落 `zlib -9`,构建不受影响。

## 分区表(`partitions.csv`)

nvs 16K / otadata / phy / **ota_0 + ota_1 各 1.9375MB(双 OTA)** / storage(littlefs,网页资源)32KB / coredump 32KB。

> 布局要点(2026-10-08):APP 分区偏移必须 64K 对齐(`gen_esp32part.py`),因此把 web 空间收窄到 32K 的收益**只有在同时回收 `0x12000–0x20000` 那段 56K 空洞时**才能均分给两个 OTA 槽;为此 nvs 由 24K 减到 16K(实际用量 ≈2KB),`ota_0` 起点前移到 `0x10000`。

## 首次使用

1. 设备启动后默认 AP 模式,连接 `IotHub-XXXX`(XXXX 为 MAC 后缀),浏览器访问 `http://192.168.8.1`(自动进入配置页);
2. 登录账号 `admin / 123456` 为出厂默认(可经 `idf.py menuconfig → Iothub Configuration` 按部署批次修改),登录后在"管理员账号"面板改密,详见 `问题与版本优化.md` 第 1 项;
3. 在"网络"中填写上游 WiFi 凭据(保存的密码不会回传明文,留空表示保持不变);
4. 在"MQTT"中配置 ThingsBoard 接入,设备获取 STA IP 后自动连接并上报。

## 目录结构

```
main/            固件源码(模块划分见各 .h 接口)
web/             前端页面(配置页 config.html,单页形态)
tools/           宿主侧脚本与单测(JS 语法检查、HTML/JS 压缩、协议/JSON 转义单测、BLE NUS/角色互换验收;**不进固件**)
问题与版本优化.md  唯一主记录:v1.0–v1.2.1 历史 + v1.3 增量/清单 + 附录 A–E
docs/soak_test_plan.md  全链路 soak 方案(独立)
docs/archive/     原始日志/历史归档(只读留证)
```

## 文档导航

- **唯一主记录**:`问题与版本优化.md` —— v1.0–v1.2.1 历史、v1.3 增量与当前清单(**1.3-70**)、附录 A NimBLE 评估、B 串口抓包、C 已解决索引、D UART 调试、E 手机验收测试卡。
- **独立方案**:`docs/soak_test_plan.md`(全链路 soak;`tools/soak/` 尚未实现)。
- **原始归档**:`docs/archive/`(日志/代码,只读留证)。

## 已知问题与安全提示

- **Web 管理页/门户无 TLS,Basic 凭据明文传输;TLS/带 SAN 证书/NVS 加密/首启强制改密已按 2026-10-18 决定不排期**。当前全开堆余量不足以承担 TLS 握手/会话缓冲;继续限定可信局域网/设备 AP,**禁止直接暴露公网**。部署前人工修改默认口令;NVS 中的 WiFi/MQTT/Web 凭据保持明文,设备需物理受控。MQTT 可选 TLS 不受此决定影响。详见 `问题与版本优化.md` 1.3-14/15 与 v1.2.1-1/16。
- UART RX 电气层问题排查记录见 `问题与版本优化.md` 附录 D;
- B 板在高负载下曾出现一次 WiFi/IP 失联(BLE/CMD 仍存活,heap 健康),重新插拔 USB 后 115200/9600 各 1h 未复现;保留后续观察(24h 按当前安排暂缓,1.3-77/78/79)。
- 配置页已按 1.3-80 做导航/按需面板、3 个 JS bundle 顺序加载、版本化缓存和发送回压;新 B 板(65D8)替换后 A/B 1h 页面压力均 60 循环、0 坏页面/0 错误,旧 B 的堆崩溃未复现。
- BLE 真机四态(RX/TX/CMD RX/CMD TX)尚未执行;断连重广播、A/B 双向角色互换(1.3-78/81)已通过,手机 nRF Connect 步骤见 `docs/soak_test_plan.md` §2.4,另一台 Ubuntu 的角色互换/数据面/回传执行包见 §2.5。
- MQTT 数据面旧实现误用 `esp_mqtt_client_enqueue` 的 QoS0/store=false(返回 -1、未真正入队),导致实际漏发远大于 `dropped_mqtt` 记录的 2%;已改 QoS1/store=true,1KB/s 负载 6min 0 丢、outbox 0(1.3-82)。
