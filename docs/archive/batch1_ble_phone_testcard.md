# 批 1 后 BLE soak 复测 · 手机协同测试卡(2026-10-14)

背景:1.3-30 批 1 已把 WiFi/tcpip/httpd 钉到 core 1(core 0 留给 BT 控制器+主机)。
本轮用手机当 BLE central + 观察端,验证核分载后 **BLE 常驻 + 页面访问共存**是否稳定。
设备侧证据由串口日志(PC 抓取)与 `/api/v1/device/status` 计数共同构成。

## 前置

1. 手机连设备 AP(`IotHub-6BBC`,默认无密码/或已设密码),浏览器开 `http://192.168.8.1` 登录(admin/123456)。
2. 配置页 → 蓝牙配置 → 模式选 **BLE** → 保存(设备会整机重启,约 5-8s,AP 重新出现,无需重新登录)。
3. 确认串口出现 `app_bt: ...` 初始化成功日志、设备广播名 `IotHubBLE-XXXX`。

## 步骤

1. **记录基线**:浏览器访问 `http://192.168.8.1/api/v1/device/status`,整段 JSON 存下来(重点看 `data_pipe` 与 `bluetooth` 两段)。
2. **连 BLE**:手机装任一支持 NUS 的调试 App——
   - 推荐 **nRF Connect**(Android/iOS,手动发送)、**Serial Bluetooth Terminal**(Android,NUS 专用)、
     **BLE调试助手**(国内应用市场,带"定时/自动发送"功能,最适合 soak)。
   - 扫描并连接 `IotHubBLE-XXXX`,**订阅 RX 特征(6E400003)的 Notify**,向 **TX 特征(6E400002)** 写数据。
3. **Soak 2-3 分钟**:用带定时发送功能的 App,每 100ms 一条 8 字节(如 `S0000001`),持续 ≥120s。
   手动发送类 App 则尽量连续发送(每 1-2s 一条 20 字节),时长拉长到 5 分钟。
   (UART 线速 9600 8N1 ≈ 960B/s,8 字节间隔 ≥8.4ms 即不超线速,不超速时 dropped/overrun 应为 0。)
4. **共存观察(核心回归)**:soak 进行中,同手机(或另一台设备)浏览器刷新 `http://192.168.8.1` 配置页,
   确认面板能加载、状态轮询不"一直加载中";记录刷新到出内容的大致耗时。
5. **记录终点**:soak 结束后再次访问 `/api/v1/device/status`,整段 JSON 存下来。
6. **观察断连**:App 是否出现断开重连?串口是否打印 BLE 断开/重连日志?(PC 侧同步盯着。)

## 判据(我来核对)

- 终点 JSON vs 基线:`rx_ble` 增量 = 发送字节数;`tx_uart1` 增量相同(BLE→UART1 交叉路由);
  `dropped`/`overrun` 增量 = 0(不超线速前提下);`bluetooth.connected=true`。
- soak 期间串口**无** BLE 异常断开/反复重连;`adv_active` 在断开后能恢复广播。
- 配置页在 BLE 常驻下加载正常(对照批 0 前的"面板一直加载中")。
- diag 每 30s 的 `heap free/min/largest` 无明显下滑(核分载不改堆,min 不应低于基线)。
