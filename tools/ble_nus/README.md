# BLE 数据面(NUS)验收 harness —— 1.3-03

PC 侧充当 **BLE central**(`bleak` + BlueZ),对设备的 NUS 服务做写入 / 回显 / 长压测,
并用设备 `/api/v1/device/status` 的 `data_pipe` **累计计数差值**做断言(不依赖绝对值)。

> 本目录**不进固件**(与 `tools/form_url/` 同约定),只用于上机验收。

## 前置条件

```bash
pip install bleak                                   # 本机 IDF venv 已安装 3.0.2
sudo apt install bluez && sudo systemctl start bluetooth   # Linux 必需:提供 org.bluez
```
若报 `org.freedesktop.DBus.Error.ServiceUnknown: org.bluez` ⇒ **BlueZ 未运行**,先装/起它;
不想动系统的话,用**手机 nRF Connect** 手动替代(见下"手机替代方案")。

设备侧前置:BLE 在广播且模式正确 —— `status.bluetooth` 应为 `mode=ble, adv_active=true`;
UART 为 `9600 8N1`(线速 ≈ **960 B/s**,压测间隔必须 ≥ ~8.4ms/8B 才不超线速)。

## 命令与期望

| 命令 | 含义 | 期望结果 |
| --- | --- | --- |
| `scan` | 只扫描 | 命中 `IotHubBLE-XXXX` |
| `echo 5` | **回环态**(自动开 `POST /api/v1/data/loopback`)写入 5 包,校验回显 | PC 收到 **等量**字节;`rx_ble`/`tx_ble` 差值各 = 写入量;`dropped` 不增长 |
| `soak 90` | **交叉路由态**持续写入 90s(BLE→UART1) | `rx_ble` 与 `tx_uart1` 差值**相等且 = 写入量**;`dropped`/`overrun` 不增长 |
| `uartface` | **反向链路**(需 `GPIO17`↔`GPIO18` **跳线**)10×10 字节 | ① `config/uart` 的 `tx_sequence +10`、`last_bytes_sent=10` ② `rx_uart1` 差值 = 100(跳线回灌)③ `tx_uart1` **恒 0**(见下"关键计数语义")④ `tx_ble` = 100(有订阅者)或 `dropped` 代理(无订阅者) |

## 关键计数语义(别记错)

- `data_pipe.tx_uart1` = **管道写 UART** 的字节数 —— **交叉路由(UART1→BLE)下恒为 0**,
  它**不是**"UART 发了多少";后者看 `GET /api/v1/config/uart` 的 `tx_sequence` / `last_bytes_sent`
  (本 harness 的 `uartface` 用这两个做"GPIO17 真发了"的证据)。
- `data_pipe.tx_ble` = 管道向 BLE 通知**且调用返回成功**的字节数 ⇒ 只有
  `status.bluetooth.connected=true`(存在已订阅的 central)时它才算**送达证据**;
  无订阅者(`app_ble_gatts_notify()` 返回 false)时应改用 `dropped`
  (含义 = "已路由到 BLE 出站但未送出")。
- `rx_uart1` / `rx_ble` 是**入站**计数,与路由无关。
- 回环(`POST /api/v1/data/loopback`)会把路由改成"各端口指向自身",此时 `tx_uart1`/`tx_ble`
  才会随入站增长 —— 这是验证**出站通路**用的技巧。

```bash
python3 tools/ble_nus/ble_nus.py scan
python3 tools/ble_nus/ble_nus.py echo 5
python3 tools/ble_nus/ble_nus.py soak 90 --interval 0.012
python3 tools/ble_nus/ble_nus.py uartface        # 先插好跳线
```
可用环境变量覆盖设备:`IOTHUB_HOST=http://192.168.1.9 IOTHUB_USER=admin IOTHUB_PASS=123456`。

## 为什么 `uartface` 必须跳线(不能软件替代)

`data_pipe` 的"回环"是**各端口把入站字节回写自身**,而 UART1 的出站实现是
`app_uart_send_bytes()` → **从 GPIO17 物理发出**,**不会**重新进入管道;
`POST /api/v1/uart/send` 同样是直写 UART(不经 `data_pipe_feed`)。
⇒ 要让"UART1 收到的字节"出现,只能**外部把 TX 短接到 RX**(或外接串口对端)。
已在代码层逐路径确认,并做过"回环 ON/OFF 20s A/B"(无外部数据源时计数恒不变)。

## 手机替代方案(不装 BlueZ)

1. 手机 nRF Connect 连接 `IotHubBLE-XXXX`;
2. **回环**:`POST /api/v1/data/loopback {"enabled":true}` → 向 RX(`6E400002`)写入 → 观察 TX(`6E400003`)通知回显;
3. **BLE→UART**:回环关掉后写入 → 看 `status.data_pipe` 的 `rx_ble`/`tx_uart1`;
4. **反向**(需跳线):开启 TX 通知订阅 → `POST /api/v1/uart/send` → 应收到通知,且 `tx_ble` 增长。

## 判定口径提醒

- 统计是**累计值**,务必用前后差值断言;
- 超线速(如 9600 下每 1ms 发 8B = 8KB/s)出现 `overrun`/`dropped` 是**真实背压**,不是 bug
  —— 验收要求的是"**不超线速时**恒为 0"。
