# iothub

基于 ESP-IDF 6.0.x、面向经典 ESP32（4 MB Flash、无 PSRAM）的物联网控制与现场数据网关。当前产品以 **BLE NUS 本地管理 + ThingsBoard MQTT 远程管理/上报 + UART1 工业数据接入** 为核心；控制命令与原始字节数据分属两个平面。

> 当前源码核对基线：`5104a3b0b3266f6afe22404d24ca89d41253cb63`（CMD-15 R4 一致性修订后的源码基线；其后 `main` 仅有文档维护）。每轮执行前仍须核对实际 `main` HEAD 与源码差异。固件版本由构建时 Git 描述串生成：`1.3-<git短SHA>[-dirty]`；无 Git 元数据时为 `1.3-unknown`。

## 当前产品边界

| 能力 | 当前状态 |
|---|---|
| 本地管理 | **BLE NUS Peripheral**，CMD 特征负责设备命令；Wi-Fi/MQTT 不可用时仍可现场管理 |
| 远程管理 | ThingsBoard MQTT RPC，使用与 BLE 相同的 v1 命令封装，并受命令来源策略限制 |
| 工业数据接入 | UART1、BLE NUS Data 特征、MQTT 数据主题，通过 `data_pipe` 转发原始字节 |
| Wi-Fi | 当前运行路径以 STA 为准；BLE 是首次配网、重新配置和断网救援入口 |
| OTA | 保留双 OTA 分区、镜像启动确认和失败回滚核心；**当前没有固件下载/上传传输入口** |
| Web / HTTP | **已从当前产品构建和运行路径移除**：无 Web 管理页、HTTP API、captive portal 或 Web OTA |
| Classic Bluetooth / SPP | **已移除**：不再构建、不再作为数据或控制通道 |
| MCP / JSON-RPC Server | 10 月的 1.2 方案曾讨论 MCP 兼容；当前固件没有实现 MCP Server，也不使用 JSON-RPC 2.0 |

Web/HTTP 与 Classic Bluetooth/SPP 的移除是面向有限 RAM/heap 的产品取舍，不是等待启用的功能开关。当前不要再按旧版规划把 Web 或 SPP 当作后续待办通道。

## 功能与命令

- **统一命令层**：BLE 和 MQTT 通过 `cmd_context`、来源策略与统一分发入口执行设备命令；底层 Wi-Fi、配置、GPIO、UART、operation 等模块仍各自拥有业务状态和资源。
- **设备与状态读取**：`device.info`、`device.status`、`network.status`、`gpio.status`、`uart.runtime`。
- **配置管理**：`config.get`、`config.set`；读取按 domain 过滤并脱敏。写入走配置应用/事务路径，不由 BLE 或 MQTT 传输代码直接写 NVS。
- **GPIO 控制**：`io.get`、`io.set`、`led.set/toggle`、`relay.set/toggle`。触摸输入事件复用同一设备控制逻辑；引脚默认值来自 Kconfig。
- **网络管理**：`network.status`、`network.scan`、`network.profile.list/delete`、`network.reconnect`、`network.tryconnect/apply/cancel`、`operation.get`。扫描、profile 管理和手动重连为 BLE 本地管理能力；MQTT 的 tryconnect/apply/cancel 受 `APP_CMD_MQTT_NET_TXN_ENABLE` 控制。当前源码头文件默认值为 `1`，因此默认策略允许 MQTT 网络事务，但只能操作 MQTT 自身拥有的 operation；实际镜像仍须核对编译宏/构建变体及设备回归证据。
- **UART**：`uart.send`、`uart.runtime`；UART1 是工业数据端口，不作为第二条独立命令解析通道。UART RX 必须遵守单一读者原则。
- **链路诊断**：`data.loopback` 用于 BLE 数据通道诊断，不是 MQTT 远程控制功能。
- **恢复出厂**：BOOT(GPIO0) 长按触发本地恢复流程；现场恢复能力不依赖 Web 门户。

### 外部命令协议

BLE CMD 与 ThingsBoard MQTT RPC 使用项目自己的 v1 JSON envelope：

```json
{
  "id": "request-id",
  "version": "1.0",
  "cmd": "device.status",
  "args": {}
}
```

- 仅接受 v1 封装；旧版 v0 `method/params`、旧方法别名和 `APP_CMD_V0_COMPAT` 回退路径已删除。
- `id` 用于请求/响应关联；成功结果与错误按当前 v1 命令协议返回。
- `auth.login` 仅用于 BLE 本地会话；登录 token 是应用层鉴权，不等同于 BLE 链路加密。
- MQTT 传输使用 ThingsBoard RPC topic（`v1/devices/me/rpc/request/+`）或自定义后端 topic。MQTT 可执行哪些命令由固件的 v1 命令表和来源策略共同决定，不应把“MQTT 已连接”理解为拥有 BLE 全部权限。

## 软件架构

控制面与数据面相互独立。控制面解释命令语义；数据面只搬运原始字节，不把工业设备帧送进 JSON 命令解析器。

```text
                    控制面
  BLE NUS CMD RX/TX                 ThingsBoard MQTT RPC
   6E400004 / 005                   v1/devices/me/rpc/*
          |                                |
          +---------------+----------------+
                          |
                 command context
              source / auth / policy
                          |
                  v1 command dispatch
                          |
         +----------------+------------------+
         |                |                  |
     config / NVS      Wi-Fi / operation   GPIO / UART
     config_apply      netcheck / owner    device state


                    数据面
    UART1              BLE NUS Data            MQTT data topics
  UART driver          6E400002 / 003          iothub/<MAC>/data/*
       |                    |                       |
       +--------------------+-----------------------+
                            |
                         data_pipe
             framing / routing / bounded queue
```

- **BLE**：正常产品运行固定为 Peripheral。设备提供 NUS 服务，Data RX/TX 使用 `6E400002/003`，CMD RX/TX 使用 `6E400004/005`。源码仍保留 Central/GATTC 兼容和 A/B 测试代码，但当前产品启动流程强制收敛为 Peripheral，Central 不是正常运行模式。
- **启动顺序**：加载配置和 operation 基础设施后先启动 BLE，再启动 Wi-Fi；因此 Wi-Fi 启动/连接故障不应阻断现场 BLE 救援。
- **Wi-Fi 事务**：`network.tryconnect` 由异步 operation 执行，按 owner、trial、operation ID 和 cleanup 生命周期管理；不会把“STA 已关联”直接当作提交成功。
- **联网验证**：DEF-NET-001 在候选拿到 IP 后验证 Broker TCP 路径和 MQTT 会话。当前 `got_ip` 是试连流程的联网验证就绪阶段，不只是 Wi-Fi 关联或 DHCP 成功；验证失败会进入失败清理/恢复路径。详情见网络验收报告。
- **配置面**：配置结构、Wi-Fi profile、配置应用和事务 guard 分层管理；传输层不得绕过 `config_apply` 直接改写配置。
- **数据面**：UART1、BLE Data 和 MQTT 作为三个数据端口，由 `data_pipe` 按 NVS 路由配置转发。队列有界；队列满时允许丢帧并记录统计，不能把它描述为无损传输。MQTT 数据面发布使用 QoS 1 和 outbox 入队机制；这不代表设备侧所有前置队列都绝不丢帧。
- **资源约束**：默认构建针对 4 MB Flash、无 PSRAM 的 ESP32。任务栈、队列、JSON 缓冲、MQTT outbox、BLE host/GATT 和 largest free block 都是需要持续审查的资源；不要为了加功能重新引入已移除的 Web/AP 门户或 SPP 常驻负担。

## 网络与 ThingsBoard

- ThingsBoard 后端使用标准遥测、attributes 和 RPC topic；自定义 MQTT 后端使用项目自定义 topic。
- MQTT 设备 client ID 由 STA MAC 派生。
- 数据面 MQTT 主题独立于命令 RPC：`iothub/<MAC>/data/up`（设备上行）和 `iothub/<MAC>/data/down`（设备下行）；数据内容按数据面编码约定处理，不与 RPC JSON 混用。
- 周期遥测与低优先级 attributes 会在资源/ outbox 积压时让路；RPC 响应不经过低优先级状态合并路径。
- MQTT 网络事务命令受 `APP_CMD_MQTT_NET_TXN_ENABLE` 编译门禁控制；当前 `main/cmd_policy.h` 未被构建参数覆盖时默认值为 `1`。因此默认源码策略允许 MQTT 发起 tryconnect/apply/cancel，但 owner 校验仍限制其只能操作 MQTT 自身的 operation。若部署策略不允许云端换网，必须在目标构建中显式关闭该宏，并核验最终镜像构建变体；不得仅凭旧规划稿判断开关状态。

## OTA 与分区

OTA 当前有两部分能力：

1. 双 OTA app 分区和 bootloader rollback 支持；
2. 新镜像启动后的健康确认：在限定窗口内检查 STA/MQTT 健康状态，确认有效或触发回滚。

当前没有 HTTP 上传、网页资源、MQTT 文件下载或 BLE 固件传输实现。**不能仅凭双 OTA 分区和回滚确认代码宣称设备已经具备可用的远程 OTA 升级链路。** 新传输方案需要单独设计、实现和验收。

当前 `partitions.csv`：

| 分区 | Offset | Size |
|---|---:|---:|
| nvs | `0x9000` | 16 KiB |
| otadata | `0xD000` | 8 KiB |
| phy_init | `0xF000` | 4 KiB |
| ota_0 | `0x10000` | `0x1E0000` = 1.875 MiB |
| ota_1 | `0x1F0000` | `0x1E0000` = 1.875 MiB |
| storage | `0x3D0000` | `0x10000` = 64 KiB（当前无 Web/LittleFS 资源镜像） |
| coredump | `0x3E0000` | 32 KiB |

## 构建与烧录

推荐使用仓库构建入口。脚本优先使用环境变量 `IDF_PATH`，未设置时尝试从 `~/.espressif/v*/esp-idf` 定位 ESP-IDF，并显式配置工具链、Python venv 和 Ninja：

```bash
tools/idfbuild.sh              # 默认 NimBLE profile，执行 build
tools/idfbuild.sh flash        # 烧录（可附加 -p /dev/ttyUSB0 等 idf.py 参数）
tools/idfbuild.sh monitor
tools/idfbuild.sh paths        # 查看 ninja / cmake / toolchain / idf.py / python 定位
tools/idfbuild.sh cache-guard  # 检查/清理生成器不匹配的 build 缓存
tools/idfbuild.sh version      # 显示使用的 ESP-IDF 版本
```

当前根 `sdkconfig` 使用 NimBLE。仓库保留独立 `build-bluedroid` profile 作为构建回退路径；它不是默认产品运行配置。使用回退 profile：

```bash
IDF_BUILD_DIR=build-bluedroid tools/idfbuild.sh build
IDF_BUILD_DIR=build-bluedroid tools/idfbuild.sh flash -p /dev/ttyUSB0
```

等价原生命令（需自行激活 ESP-IDF 环境并确保 Ninja 在 PATH 中）：

```bash
idf.py set-target esp32
idf.py build
idf.py -p PORT flash monitor
```

GPIO 默认值由 `main/Kconfig.projbuild` 配置并写入 `sdkconfig`：继电器 GPIO16、状态灯 GPIO4、输入 GPIO27（默认启用触摸输入）。UART1 使用 TX GPIO17 / RX GPIO18。调整引脚前必须核对 ESP32 模组实际引脚能力、Flash/PSRAM 占用和板级电路；GPIO34–39 仅输入，不能配置为输出。

## 首次使用（BLE 本地管理）

1. 手机或主机扫描设备广播，连接配置的 BLE 设备名；当前固件为 NUS Peripheral。
2. 发现 NUS 服务并订阅 CMD TX `6E400005`，向 CMD RX `6E400004` 发送 v1 `auth.login`（`args.username/password`）；成功后 token 位于响应 `data.token`。首次使用请通过受控方式修改出厂管理凭据，不要把默认口令写入公共文档或日志。
3. 使用 `network.scan` 查看现场 AP，调用 `network.tryconnect` 创建异步 operation；通过 `operation.get` 查询阶段。联网验证就绪后，使用 `network.apply` 提交；需要中止时按 operation 状态调用 `network.cancel`。
4. 使用 `config.get` 读取允许的配置域，使用 `config.set` 并在 `args.patch` 中提交配置变更。密码/token 只返回是否已设置，不回显明文。
5. Wi-Fi 配置或 MQTT 配置变更后，分别确认 STA 网络、MQTT 会话和设备状态；不要把“配置已保存”误认为“远程连接已恢复”。

## 目录结构与文档导航

```text
main/             ESP-IDF 固件源码（命令、BLE、Wi-Fi、MQTT、UART、data_pipe、operation、OTA 等）
contract/         当前 v1 协议契约（字段、命令、错误码）
tools/            构建入口、BLE/主机侧工具、测试与证据采集脚本（不编入固件）
tests/            宿主侧单元/门禁测试
docs/             当前架构总览、专项审查、验收状态与历史证据导航
```

- **当前产品与构建入口**：本 README；产品事实以当前源码和对应构建基线为准。
- **BLE 专题总览**：[`docs/BLE_架构与验收总览.md`](docs/BLE_架构与验收总览.md)，说明当前 BLE 架构、安全边界及 T3/T4/T5 验收状态。
- **MQTT 专题总览**：[`docs/MQTT_架构与验收总览.md`](docs/MQTT_架构与验收总览.md)，说明 ThingsBoard MQTT 职责、权限和 CMD-15 状态。
- **CMD 架构与权限**：[`docs/CMD共享命令架构与权限边界.md`](docs/CMD共享命令架构与权限边界.md)；任务状态以 [`docs/cmd_refactor_plan.md`](docs/cmd_refactor_plan.md) 为准。
- **Wi-Fi owner fencing**：[`docs/WIFI-OP-RESOURCE-RACE_主文档.md`](docs/WIFI-OP-RESOURCE-RACE_主文档.md)，当前 owner/cleanup 并发专项入口。
- **OTA 与启动回滚**：[`docs/OTA_架构与验收总览.md`](docs/OTA_架构与验收总览.md)，区分启动回滚与尚未接入的 OTA 传输链路。
- **S6 现场验收总览**：[`docs/S6_现场验收总览.md`](docs/S6_现场验收总览.md)，当前轮次、设备身份、事故和待批准事项。
- **当前命令层审查**：[`docs/cmd.c_review_2026-11-14.md`](docs/cmd.c_review_2026-11-14.md) 与 [`docs/CMD-02_03_并发静态审查_2026-11-15.md`](docs/CMD-02_03_并发静态审查_2026-11-15.md)。
- **阶段变更摘要（2026-11-14 至 11-15）**：[`docs/近期变更记录_2026-11-14至11-15.md`](docs/近期变更记录_2026-11-14至11-15.md)，仅作历史阶段摘要，不代表 11-17 的完整变更清单，也不替代专项状态或授权。
- **项目文档导航**：[`docs/README.md`](docs/README.md)；历史文档保留规则见 [`docs/archive/README.md`](docs/archive/README.md)。
- 原始日志和历史验收证据见 `docs/archive/` 与 `docs/evidence/`；历史 PASS 仅适用于原文记录的设备、镜像和测试范围。

## 已知限制与安全提示

- **BLE 当前未启用链路加密/绑定**；`auth.login` token 只是应用层鉴权，不能替代链路加密。部署时应限制物理接近范围，并在正式使用前更换出厂管理凭据。
- NVS 中 Wi-Fi/MQTT/管理凭据目前没有启用 NVS encryption；不得把可读取 NVS 的设备视为凭据保密。
- MQTT TLS 可按配置启用；使用明文 MQTT 时，网络路径上的凭据与命令缺少传输层机密性保护。
- SPP、Web/HTTP、captive portal 和 Web OTA 均不属于当前产品能力；不要依据旧版规划文档或残留常量推断这些功能仍可用。
- OTA 传输尚未接入；新镜像健康确认/回滚不等于完整 OTA 下载和升级流程。
- 资源数值必须结合具体板型、固件 SHA、运行场景与日志记录；README 不给出跨构建通用的 heap 保证值。

## 测试凭据管理

测试设备 Token、Wi-Fi 密码、管理员口令、私钥和证书不得提交到公开仓库或 README。通过设备本地配置、受控环境变量或不纳入版本控制的文件管理；曾经公开过的凭据应视为已泄露并轮换。
