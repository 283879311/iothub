# iothub

基于 ESP-IDF(v6.0,目标芯片 ESP32)的物联网控制与现场调试网关:提供 Web 管理界面、UART 串口调试台、ThingsBoard 云接入(MQTT)、GPIO 控制与 HTTP OTA,定位为"底层核心应用与控制"的薄设备端,业务逻辑由手机/电脑端承载。

当前固件版本:**1.2**(`CMakeLists.txt` → `set(PROJECT_VER "1.2")`)。v1.2 起规划**统一命令解析层(控制面 / 数据面双平面)+ MCP 兼容**,把 MQTT、串口、BLE 收敛到同一套命令语义(经典蓝牙 / SPP 已于本版本移除),详见 `问题与版本优化.md`。

## 功能

- **串口调试台**:UART 参数配置(5-8 数据位、奇偶校验、1/2 停止位、任意波特率),收发数据查看(文本/hex)、清屏、手动发送。
- **Web 管理页 `/config`**:网络、MQTT、GPIO、蓝牙、UART 配置,管理员账号修改与固件升级(Basic 认证,可在页面修改口令)。
- **强制门户**:AP 模式下手机/电脑接入后自动弹出配置页(本地 DNS 劫持 + OS 连通性探测 302);mDNS(`.local` 域名)已于 1.3-27 移除(BLE 常驻时堆挤占,局域网内用 IP 直连)。
- **网络**:AP(192.168.8.1)/ STA / APSTA;最多 5 条 STA profile,RSSI 择优自动重连;纯 STA 启动 90s 失败自动降级 AP 并持久化。
- **MQTT(ThingsBoard)**:10s 周期遥测(继电器/LED/输入/堆内存)+ attributes 上报;下行 RPC `setRelay/setLed/getStatus/setState` 控 GPIO;支持 tb_cloud / tb_selfhost / 自定义 topic,TLS(证书 bundle)与 LWT。
- **OTA**:Web 上传固件(`iothub.bin`),带应用描述区校验(魔数/项目名/版本降级拒绝);网页资源 `storage.bin` 支持运行时整分区替换;升级后经健康确认窗口(业务可达才 mark valid,超时自动回滚)。
- **恢复出厂**:BOOT 键(GPIO0)长按 5s。
- **蓝牙**:BLE 广播(占位,暂无 GATT 数据面,见 `问题与版本优化.md` 第 8 项);经典蓝牙 / SPP 已于 v1.2 移除(CONFIG_BT_CLASSIC_ENABLED=n)。

## 硬件引脚(编译期固定,`main/constants.h`)

| 功能 | GPIO |
| --- | --- |
| 继电器 | 16 |
| LED | 27 |
| 输入检测 | 33 |
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
tools/           宿主侧脚本与单测(JS 语法检查、HTML/JS 压缩、协议/JSON 转义单测;**不进固件**)
docs/debug/      UART 排查记录(遗留问题见文档内状态)
问题与版本优化.md  版本优化与问题记录汇总(v1.0/v1.1 审查、v1.2 设计基线、v1.2.1 问题闭环,含立项第 27/28 项)
```

## 已知问题与安全提示

- 全站无 TLS(Basic 凭据明文传输)—— 见 `问题与版本优化.md` 第 1 项遗留部分;
- 蓝牙功能为占位实现(仅 BLE 广播,无 GATT 数据面;经典蓝牙 / SPP 已在 v1.2 移除);
- UART RX 电气层问题排查记录见 `docs/debug/`。
