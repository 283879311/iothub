# 非 VM Ubuntu 全链路 soak 测试方案（2026-10-18）

> 目标：把 soak 从当前 VM 迁到一台**非虚拟机 Ubuntu**，覆盖 **BLE / HTTP / UART / MQTT**，
> 验证全开（BLE + MQTT + 页面 + UART）长时间稳定，并给出 NimBLE 默认 / Bluedroid 回退的 A/B 结论。
> 本文是方案，不含脚本实现；确认后再落 `tools/soak/`。
>
> **2026-10-18 同步**：1h 四特征 @115200/9600 已完成（1.3-78/79）；页面 1h A/B 已过（1.3-80）；BLE 角色互换 A/B 双向已过（1.3-81）；手机 nRF Connect 四态待真机（§2.4）；另一台 Ubuntu 真机执行包见 §2.5；24h 按用户要求暂不排期。
> `tools/soak/` 尚未实现；当前宿主为 VM，无 BlueZ/mosquitto，主机侧 BLE/MQTT 对照需换非 VM 环境或补装。

## 0. 目标与判据

### 0.1 必过项（每块板、同一配置）

| 项 | 判据 |
| --- | --- |
| 重启/崩溃 | 24h 内无 `panic`、无 `Guru`、无看门狗、无计划外重启（`rst:` 只允许测试开始那次） |
| BLE | 连接/断开/重连符合用例预期；echo/soak 字节 1:1；四态见 §2.4；无计划外断链 |
| HTTP | 页面/API 非 200 只允许登录/会话边界的 302/401；**503 = 0**；`largest` 全程有记录 |
| MQTT | 无计划外断链；outbox/丢弃符合预期；遥测/共享属性双向可达 |
| UART | 1:1 字节校验；`overrun=0`、`dropped=0`（不超线速前提） |
| 资源 | `free/largest/heap new low/stack water` 全程记录；最差值写进报告 |
| 电源/USB | 宿主机 `dmesg` 无 USB 掉线/复位（B 板尤其） |

### 0.2 A/B

同一块板、同一场景、同一 harness，分别烧：
- 默认 **NimBLE**（`tools/idfbuild.sh flash`）
- **Bluedroid 回退**（`IDF_BUILD_DIR=build-bluedroid tools/idfbuild.sh build/flash`）

比较 `free/largest` 稳态与瞬态、`heap new low`、503 次数、BLE/MQTT 丢包/重连；结论按 1.3-63 的稳定性优先口径。

## 1. 测试床

### 1.1 主机

- Ubuntu 22.04/24.04，**非 VM**；内核 6.x；有线以太网接到设备同一路由器（192.168.1.0/24）。
- **真实蓝牙适配器**（内置或 USB dongle）：支持 LE central + peripheral；`bluetoothctl list` / `btmgmt info` 能看到控制器。
- USB：至少 2 个板载串口（console）+ 1 个 3.3V USB-TTL（UART1 数据）；建议带供电的 USB Hub，线材可靠。
- 电源：A/B 板用稳定 5V；建议外部 5V 供电，USB 只作数据。旧 6BBC 已决定废弃，不再接入测试床。

### 1.2 设备

| 板 | 地址 | 当前角色 | 备注 |
| --- | --- | --- | --- |
| A C8E4 | 192.168.1.213 | peripheral + MQTT | 静态 IP；console `/dev/ttyUSB0` |
| B 65D8 | 192.168.1.42 | peripheral + MQTT | 新板 MAC `10:97:BD:D3:65:D9`；静态 IP；console `/dev/ttyUSB1`；旧 B 6BBC 已更换（1.3-80） |
| 废弃 6BBC | — | — | MAC `94:b5:55:f7:6b:bc`；已确认板级异常（USB I/O error/整板掉电/低负载性能异常），**不再作为 B/C 或任何测试/验收板**；历史数据只作故障对照 |

- 默认固件 NimBLE；Bluedroid 为回退 profile。
- B 当前 STA-only；若开 AP+STA 兜底，注意内存余量（页面负载下更紧）。
- 设备端页面轮询已收敛为状态 10s、UART 控制台仅视口内 10s；HTTP 单登录接管 + 空闲 3 分钟回收已生效（1.3-72/73）。

## 2. 接线

### 2.1 console（必接）

- 每板 USB console 到 Ubuntu；只读日志，**全程保持长抓包**，不要反复短开（open 可能复位板子）。
- `tools/capture_serial.py` 已处理 DTR/RTS/HUPCL；测试期间每板一个常驻 capture 进程。

### 2.2 UART1 数据面（必接）

- 3.3V USB-TTL：
  - TTL **TX → 板 GPIO18（UART1 RX）**
  - TTL **RX ← 板 GPIO17（UART1 TX）**
  - **GND 共地**
- 严禁 5V；UART1 与 console UART0 是两路，不要混。
- 需要 UART 自环时用现有接口：`POST /api/v1/data/loopback {"enabled":true}`（运行期生效，不落 NVS）；测完 `false` 恢复路由。
- 先用 9600 8N1 做 1:1，再上 115200；每帧间隔按线速留足，避免把“真实背压”误判为 bug。

### 2.3 BLE

- 主机真实适配器作 central：扫描 DUT 的 NUS 外设，`ble_nus.py echo/soak`。
- 主机作 peripheral（验证 DUT central）：用 BlueZ GATT server（`bless`/`bluez-peripheral`）提供 NUS 服务；
  或第二块板作 peripheral。主机适配器必须支持 LE advertising。
- 手机 nRF Connect 是 **2.4 四态验收的真机执行端**，不是自动化依赖；A/B 角色互换用 `tools/ble_nus/role_swap.py` 自动完成。

### 2.4 手机 nRF Connect 四态验收（真机）

> 目的：补足 A/B 自动测试覆盖不到的“手机 central + GATT 属性可见性/可操作性”。
> 角色互换已由 `tools/ble_nus/role_swap.py` 双向通过（1.3-81）；本节只验 **RX / TX / CMD RX / CMD TX** 四态。
>
> **前置**
> - DUT 为 peripheral：`status.bluetooth.mode=ble`、`runtime_mode=ble`、`adv_active=true`、`connected=false`。
> - 手机装 nRF Connect（Android/iOS）；PC 与设备同网段，可 `curl`（用于 loopback/status）。
> - 记录基线 `GET /api/v1/device/status` 的 `data_pipe` 与 `bluetooth` 两段，所有断言用**前后差值**。
> - 默认口令 `admin/123456`；若已改密，用实际口令。单登录会踢掉旧页面，测试期间不要另开配置页。
> - 下面 curl 以 B(`192.168.1.42`) 为例；测 A(`192.168.1.213`)时替换 IP。

**UUID / 属性（以 `bt_gatts_nimble.c` 为准）**

| 状态 | 特征 | UUID | 属性 | 手机操作 |
| --- | --- | --- | --- | --- |
| RX | 数据 RX | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | Write / Write Without Response | 向它写数据 |
| TX | 数据 TX | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | Notify | 打开通知，接收设备上行 |
| CMD RX | 命令 RX | `6E400004-B5A3-F393-E0A9-E50E24DCCA9E` | Write / Write Without Response | 向它写 JSON 命令行 |
| CMD TX | 命令 TX | `6E400005-B5A3-F393-E0A9-E50E24DCCA9E` | Notify | 打开通知，接收命令回包 |

> NUS 服务 UUID：`6E400001-B5A3-F393-E0A9-E50E24DCCA9E`。
> 手机若把 128-bit 特征显示为 “Unknown Characteristic”，是 App 名称库问题，不影响按 UUID 操作（见主记录 P2）。

**步骤**

1. **连接与订阅**：nRF Connect 扫描并连接 `IotHubBLE-XXXX`，展开 NUS 服务；确认上表四个特征都存在且属性正确。分别点开 **TX** 与 **CMD TX** 的 Notify（三箭头图标），确认 CCCD 写入成功；记录 MTU（1.3-81 实测 256，payload 253）。
2. **RX 态**：保持 loopback 关闭；向 **RX(6E400002)** 写入 `TEST-RX-01`（用 Write with response，确认返回成功）。读 status：`rx_ble` 增加 10，`tx_uart1` 增加 10（默认路由 BLE→UART1），`dropped_ble`/`overrun_ble` 不变（MQTT 侧 QoS0 丢帧另计）。
3. **TX 态**：用 PC 打开回环：
   ```bash
   curl -c /tmp/cj -d 'username=admin&password=123456' http://192.168.1.42/api/v1/auth/login -o /dev/null
   curl -b /tmp/cj -X POST -H 'Content-Type: application/json' \
        -d '{"enabled":true}' http://192.168.1.42/api/v1/data/loopback
   ```
   再向 RX 写 `TEST-TX-01`；**TX(6E400003)** 应收到完全相同的通知（可能按 MTU-3 分片，拼接后比对）。`tx_ble` 增加 10。完成后关闭回环（`{"enabled":false}`）。
4. **CMD RX / CMD TX 态**：确认 **CMD TX(6E400005)** 通知已开。向 **CMD RX(6E400004)** 写登录命令，**必须以 `\n` 结尾**：
   ```json
   {"id":1,"method":"login","params":{"username":"admin","password":"123456"},"token":""}
   ```
   > nRF Connect 的 Text 写入若没有 “Append newline” 选项，改用 Hex 模式，在 JSON 十六进制末尾加 `0A`。
   > 当前固件登录回包多包了一层 `result`：token 在 `result.result.token`（已知协议瑕疵；若后续修复则为 `result.token`）。

   等待 ≥300ms（CMD 限频 200ms），从 **CMD TX** 取 token；再写：
   ```json
   {"id":2,"method":"getStatus","params":{},"token":"<上一步的 token>"}
   ```
   （同样以 `\n` 结尾。）应收到 `{"id":2,"ok":true,"result":{"relay_on":...,"led_on":...,"input_active":...,"net_mode":"...","sta_has_ip":...,"sta_ip":"..."}}`，不得出现 `auth_required`。
5. **断开重广播**：在 App 里断开连接；status 应回到 `connected=false`、`adv_active=true`；重新扫描应能再次看到设备（1.3-78/81 已验证，此处复验手机侧）。
6. **记录**：把四态的通过/失败、实际通知、status 差值填入下方记录表；失败时附 nRF Connect 日志与串口尾部。

**判据（四态全过）**

| 项 | 通过条件 |
| --- | --- |
| RX | 写入成功；`rx_ble`/`tx_uart1` 差值均 = 写入字节；`dropped_ble`/`overrun_ble` 不增长 |
| TX | 通知字节与写入完全一致；`tx_ble` 差值 = 写入字节；`dropped_ble` 不增长；CCCD 订阅成功 |
| CMD RX | CMD TX 收到 login 回包；token 非空；无 `auth_required` |
| CMD TX | getStatus 回包字段完整；通知链路与数据 TX 独立 |
| 断开 | 重广播 `adv_active=true`；无复位/panic；heap 不异常下降 |

**记录表（手机执行时填写）**

| 状态 | 结果 | 实际通知 / status 差值 | 备注 |
| --- | --- | --- | --- |
| RX |  |  |  |
| TX |  |  |  |
| CMD RX |  |  |  |
| CMD TX |  |  |  |
| 断开重广播 |  |  |  |

**本机限制**：当前验收 VM 只有 `hci0`，无 BlueZ/`bluetoothctl`，`bleak` 无法扫描（`org.bluez` 未提供），因此四态必须由手机执行；A/B 角色互换已不依赖主机 BLE。

### 2.5 其他 Ubuntu 真机执行顺序（BLE 全项）

> 适用：**非 VM Ubuntu** + A/B 两块板；四态另需手机 nRF Connect。
> 本机验收 VM 的 `hci0` 没有 BlueZ，不能作为执行机；角色互换只依赖 HTTP，任意同网段 Ubuntu 都能跑。

#### 2.5.1 环境准备

```bash
sudo apt update
sudo apt install -y bluez bluez-tools python3-venv python3-pip python3-serial
sudo systemctl enable --now bluetooth
bluetoothctl list                 # 应能看到控制器；无输出说明适配器/服务未就绪
sudo usermod -aG dialout "$USER"  # 串口权限，重新登录后生效

python3 -m venv .venv-ble
. .venv-ble/bin/activate
pip install bleak pyserial
```

- 与设备同网段，先确认 HTTP：
  ```bash
  curl -s -o /dev/null -w '%{http_code}\n' http://192.168.1.42/portal
  ```
- 默认 A=`192.168.1.213`、B=`192.168.1.42`；不同地址用环境变量覆盖：
  ```bash
  export IOTHUB_A_HOST=http://<A-IP>
  export IOTHUB_B_HOST=http://<B-IP>
  export IOTHUB_USER=admin IOTHUB_PASS=<实际口令>
  ```
- 串口名以 `ls -l /dev/serial/by-id/` 为准；下文 `/dev/ttyUSB0/1` 只是示例，A/B 不要接反。
- 带到另一台机器的文件：`tools/ble_nus/role_swap.py`、`tools/ble_nus/ble_nus.py`、`tools/capture_serial.py` 和本文件。
  **注意**：`role_swap.py` 当前是工作区新增文件（未提交到 git），用 `git archive`/clone 导出时需单独拷贝，否则脚本不在包里。

#### 2.5.2 角色互换（A/B，两个方向）

```bash
python3 tools/ble_nus/role_swap.py --central B --peripheral A
python3 tools/ble_nus/role_swap.py --central A --peripheral B
```

每向约 30–60s（含两次重启）。PASS 判据：

1. 切 central 的一侧：`runtime_mode=central`、`connected=true`、`last_error=""`；
2. 被连的一侧：`connected=true`；
3. 串口出现 `NUS handles: rx=3 tx=5`（central）和 `data TX subscribe on (handle=5)`（peripheral），MTU 256；
4. 恢复 `ble` 后：central 侧 `runtime_mode=ble/adv_active=true`，peripheral 侧 `connected=false/adv_active=true`。

脚本在失败或 Ctrl-C 时会在 `finally` 里把 central 板恢复为 `ble`。若进程被强杀导致未恢复：

```bash
curl -b /tmp/cj -X PUT -H 'Content-Type: application/json' \
     -d '{"mode":"ble"}' http://<central-IP>/api/v1/config/bluetooth
```

#### 2.5.3 数据面（可选，Ubuntu 作 BLE central）

主机蓝牙可用时，先确保 DUT 为 peripheral、手机已断开：

```bash
python3 tools/ble_nus/ble_nus.py scan
IOTHUB_HOST=http://192.168.1.42 python3 tools/ble_nus/ble_nus.py echo 5
IOTHUB_HOST=http://192.168.1.42 python3 tools/ble_nus/ble_nus.py soak 90 --interval 0.012
```

- `echo` 验回环（RX 写入→TX 通知）；`soak` 验 BLE→UART1 的 1:1 与背压。
- 9600 波特率下 `--interval` 不要小于 0.012s；115200 可按目标速率调整。
- 主机无 BlueZ/适配器时跳过本节，改由手机完成四态。

#### 2.5.4 手机 nRF Connect 四态

按 **§2.4** 执行（RX/TX/CMD RX/CMD TX + 断开重广播），并填写该节的记录表。
建议同时常驻抓串口（只开一次，不要反复短开）：

```bash
python3 tools/capture_serial.py /dev/ttyUSB1 /tmp/ble_phone_B.log &
# 测完 kill 该进程
```

#### 2.5.5 结果回传模板

| 用例 | 执行端 | 结果 | 证据 |
| --- | --- | --- | --- |
| 角色互换 B central → A peripheral | Ubuntu `role_swap.py` |  | 脚本输出 + A/B status |
| 角色互换 A central → B peripheral | Ubuntu `role_swap.py` |  | 脚本输出 + A/B status |
| 数据面 echo/soak（可选） | Ubuntu `ble_nus.py` |  | 计数差值 + 串口尾部 |
| 手机四态 RX/TX/CMD RX/CMD TX | 手机 nRF Connect |  | §2.4 记录表 + 串口 |
| 断开重广播 | 手机/串口 |  | `adv_active`、`advertising restart` 日志 |

失败时至少保留：`role_swap.py` 完整输出、两板 `/api/v1/device/status` JSON、串口最后 100 行、手机通知原始 hex。

#### 2.5.6 已知限制

- central 侧扫描仍用 `bt_device_name` 前缀 + `"Iot*"` 兜底；同场有多台 `IotHubBLE-*` 时可能连错，双板测试场只留一台 peripheral。
- 当前 CMD `login` 回包是双层 `result`，token 在 `result.result.token`；手机验收按现状取，后续若统一为单层再更新。
- 角色互换脚本只改 `bt_role`，不校验业务数据 1:1；吞吐以 §2.4 四态和 1.3-78 的历史四特征压测为准。

## 3. 软件与工具

```bash
sudo apt install bluez bluez-tools mosquitto mosquitto-clients python3-venv
python3 -m venv .venv-soak
. .venv-soak/bin/activate
pip install bleak paho-mqtt requests pyserial pytest playwright
playwright install chromium
```

- 仓库现有：`tools/ble_nus/ble_nus.py`（scan/echo/soak/uartface）、`tools/ble_nus/role_swap.py`（A/B 角色互换）、`tools/capture_serial.py`、`tools/idfbuild.sh`。
- 需新增（本方案确认后）：`tools/soak/` 统一 runner（共享会话、指标采集、断言、报告）。

## 4. 关键约束（必须先解决，否则 soak 结果会被测试自身污染）

### 4.1 单登录会话（1.3-73）

设备同一时刻只保留最后一个登录：
- **每个设备只允许一个控制会话**：runner 登录一次，保存 `iothub_sid`，所有 HTTP/status 调用共用；
- Playwright 浏览器**不要自己登录**：把 runner 的 Cookie 注入浏览器上下文再打开 `/config`；
- `ble_nus.py` 当前会自行登录，合并 soak 时必须改为接受共享 Cookie（或 runner 复用其状态读取）；
- `role_swap.py` 也会逐板登录；**不要与 `ble_nus.py`/浏览器同时跑**，否则单会话接管会互踢；
- 任何第二次登录都会关闭旧连接、作废旧会话——两个工具各登一次会互相踢。

### 4.2 会话 1h TTL 与 24h soak

`WEB_SESSION_TTL_MS` 目前 1 小时。24h 页面/HTTP soak 有两种做法：
1. **生产口径**：每 ≤55min 重新登录一次；登录接管会关闭旧页面连接，浏览器需重新注入新 Cookie 并重载。把这次中断标记为“计划内”，不计入故障。
2. **稳定性口径（推荐 24h 页面用）**：打一个**测试专用**固件，把 `WEB_SESSION_TTL_MS` 临时改为 24h（或 25h），其余逻辑不变；测试结束恢复正式固件。这样 24h 页面/API 不被 1h TTL 打断。

无论哪种，**状态轮询保持 ≤10s**，否则空闲 3 分钟连接回收会断；这是预期行为，不是故障。

### 4.3 MQTT 隔离

- **推荐本地 mosquitto**（192.168.1.x:1883）做确定性 soak：
  - 通过 `/api/v1/config/mqtt` 把 DUT 指到本地 broker（custom 后端、测试 token），host 用 paho 发布/订阅；
  - 覆盖 `iothub/rpc`、`iothub/attributes`、`iothub/attributes/response/+` 和 data-down topic；
  - 测完恢复 TB 配置。
- **TB 端到端**单独一轮：
  - 用 TB REST/租户 API 设置 shared attributes、读取 telemetry；
  - **不要**用第二个客户端复用 DUT 的 token（broker 侧会顶掉设备会话）；用测试设备/独立 token。

### 4.4 电源/USB

- 旧 B 6BBC 曾出现 USB 串口 I/O error、整板掉电迹象；已更换为新 B 65D8（1.3-80 页面 1h 通过）。测试床仍应使用可靠线材/供电 Hub，或外部 5V。
- console 抓包常驻，不反复 open/close；`dmesg` 监控 USB 复位。

### 4.5 页面

- 真实页面用 Playwright/Chromium 单标签页；页面可见（`document.hidden=false`），否则 10s 轮询不会跑，连接 3 分钟后被回收。
- 页面 soak 与人工登录不可同时（登录接管会主动关闭人工页面）。

## 5. 用例矩阵

| ID | 用例 | 拓扑/负载 | 时长 | 主要断言 |
| --- | --- | --- | --- | --- |
| T0 | 冒烟 | console + HTTP 登录/status + BLE scan + MQTT connect + UART 自环 | 30min | 五项全通；无复位；会话/连接正常 |
| T1 | BLE 外设 soak | Ubuntu/手机 central ↔ DUT peripheral；`ble_nus.py echo/soak`；手机四态见 §2.4 | 2h | 字节 1:1；无断链；MTU 256；dropped/overrun=0；四态全过 |
| T2 | BLE 主机 soak | DUT central ↔ Ubuntu/第二板 peripheral；角色互换先跑 `role_swap.py` | 1h | 扫描/连接/发现/订阅；双向 1:1；断开后重扫；角色恢复后广播 |
| T3 | HTTP API soak | 共享会话，每 2s `/api/v1/device/status` + 每 10s `/config` | 1h/2h | 503=0；无非预期非 200；heap 记录 |
| T4 | 真实页面 soak | Playwright 单页，页面可见；UART 面板滚入视口 | 1h | 200；无 503；页面 JS 无报错；heap |
| T5 | MQTT soak | 本地 mosquitto 或 TB；数据面 QoS1（1.3-82）+ QoS1 遥测/共享属性 | 2h | 无断链；outbox 正常；数据双向；`tx_mqtt` 与入站 1:1、PUBACK 可计数 |
| T6 | UART soak | USB-TTL；9600 与 115200；loopback 或跨 BLE/MQTT | 2h | 1:1；overrun/dropped=0 |
| T7 | 全开 1h | BLE + MQTT + 页面 + UART | 1h | 无复位/503/掉线；heap 瞬态记录 |
| T8 | 全开 24h | 同 T7 | 24h | 无复位/看门狗；503=0；无计划外掉线；UART 1:1 |
| T9 | 故障注入 | WiFi 断/恢复、broker 停/恢复、BLE 断/重连、页面会话接管 | 每组 10min | 自动恢复；无崩溃；恢复时间记录 |
| T10 | A/B | T7/T8 分别 NimBLE 与 Bluedroid 回退 | 1h/24h | heap/瞬态/503/掉线对比 |
| T11 | 手机四态 | 手机 nRF Connect ↔ DUT peripheral（§2.4） | 10min | RX/TX/CMD RX/CMD TX 四态；断连重广播；无复位 |

> 全开 24h 建议**先单板**跑 A，再单板跑 B；双板并行只用于最终共存检查（主机 BLE 适配器连接数、USB/电源、两设备各自会话）。

## 6. 指标与采集

### 6.1 串口（每板常驻 capture）

关注行：
- `diag: heap free=… min=… largest=… level=… | … mqtt_outbox=… mqtt_shed=… pipe_yield=…`
- `diag: heap new low: free=… largest=… t=… ble_adv=… ble_conn=… mqtt=… page=… outbox=…`
- `diag: stack water: …`
- `http idle session reap: fd=… idle_ms=…`
- `login takeover: closed … other http connection(s)`
- `html send skipped (heap low)` / `html send failed`
- `mqtt: MQTT_EVENT_DISCONNECTED` / `MQTT_EVENT_CONNECTED` / `MQTT_EVENT_PUBLISHED`
- `bt_gatts: central connected/disconnected`、`bt_gattc: scanning/connected/disconnected/NUS handles`
- `data_pipe: queue full/dropped`
- `rst:` / `panic` / `Guru` / `Task watchdog`

### 6.2 HTTP

`/api/v1/device/status` 每 5s 采样：`free_heap`、`largest_free`、`httpd_clients`、`mqtt.connected/outbox/publish_count`、`bluetooth.connected`、`data_pipe` 各计数；页面请求记录状态码/耗时/字节。

### 6.3 BLE

`ble_nus.py` 的字节计数 + 设备 status 的 `data_pipe.rx_ble/tx_ble`；记录 MTU、RSSI、断链次数/原因。
- 角色互换用 `tools/ble_nus/role_swap.py --central B --peripheral A`（反向同理），判定两侧 `runtime_mode/adv_active/connected/last_error`，失败自动恢复 central 为 `ble`。
- 手机四态按 §2.4 记录；`nRF Connect` 的 GATT 属性/通知/CMD 回包为准。

### 6.4 MQTT

paho 的 pub/sub 计数；设备 `mqtt_outbox/publish_count/mqtt_shed`；断链/重连时间。

### 6.5 UART

pyserial 发送/接收字节、CRC/序号；设备 `data_pipe.rx_uart1/tx_uart1/dropped/overrun`。

### 6.6 判定

- 计划内事件（登录接管、1h TTL 重登录、故障注入）单独标记，不计入故障。
- 数据落 `docs/soak_results/<date>/<case>/{serial_*.log,status.csv,ble.csv,mqtt.csv,uart.csv}`；报告汇总最差 `free/largest`、503 次数、掉线/重连、复位/看门狗。

## 7. 执行顺序

| 阶段 | 内容 | 预计 |
| --- | --- | --- |
| D0 | 主机环境、BT 适配器、USB/供电、接线、console 常驻 | 0.5 天 |
| D1 | T0 冒烟 + T1/T3/T5/T6 各 10–30min 短测，调通 harness；先跑 §2.5 角色互换与 §2.4 手机四态 | 1 天 |
| D2 | T1–T6 正式 1–2h + T7 全开 1h | 1 天 |
| D3 | T10 A/B（NimBLE / Bluedroid，各 1h） | 1 天 |
| D4–D5 | T8 全开 24h（A 板，NimBLE；必要时 B 板） | 2 天 |
| D6 | T9 故障注入 + 报告 | 1 天 |

## 8. 风险与回退

| 风险 | 处置 |
| --- | --- |
| 主机 BT 适配器不支持 LE peripheral/广告 | 换 USB BT dongle；DUT central 用第二块板兜底 |
| B 板 USB/供电不稳 | 换线/换口/带供电 Hub/外部 5V；flash 用 115200 |
| 1h 会话 TTL 打断 24h | 计划内每小时重登录（生产口径），或测试专用 24h TTL 固件（稳定性口径） |
| 登录接管踢掉其他测试工具 | 所有工具共用同一 Cookie；浏览器注入 Cookie，不自行登录 |
| 页面 3 分钟空闲回收 | 保持 ≤10s 轮询；后台标签页会断，属预期 |
| AP+STA 内存压力 | 默认 STA-only；需要 AP 兜底时单独测，不与 24h 页面 soak 混跑 |
| TB token 冲突 | 不复制 DUT token；用 TB REST/独立测试设备 |
| central 目标名回退 `Iot*` | 配置名是设备自己的名字；多台 `IotHubBLE-*` 同场时 central 可能连错对端。双板测试场只留一台 peripheral；量产前应改独立目标前缀/按 NUS 服务 UUID 匹配（主记录 P2） |
| 本地 broker 配置忘恢复 | 测试脚本 finally 恢复原 MQTT 配置并重启 MQTT；记录前后配置 |
| 测试固件误留 | 测试专用 TTL 构建单独目录/镜像，结束后烧回默认镜像 |

## 9. 交付物

1. `tools/soak/`：统一 runner（共享会话）、BLE/HTTP/MQTT/UART 组件、日志采集、断言、报告；
2. `docs/soak_results/`：CSV/串口日志/图表；
3. 一份测试报告：T0–T10 结果、最差 heap/瞬态、503/掉线/复位统计、A/B 结论、遗留问题。

---

**确认点（实施前）**
- 非 VM Ubuntu 的 BT 适配器型号/是否支持 LE peripheral；
- USB 口数量与供电方案（尤其 B 板）；
- 24h 页面 soak 选“每小时重登录”还是“测试专用 24h TTL 固件”；
- MQTT 以本地 mosquitto 为主还是 TB 为主；
- 是否双板并行，还是先单板 A/B。
