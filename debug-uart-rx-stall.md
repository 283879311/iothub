# Debug Session: uart-rx-stall
- **Status**: [OPEN]
- **Issue**: UART 发送正常，但接收再次失效，页面接收窗口没有数据。
- **Debug Server**: http://127.0.0.1:7777/event
- **Log File**: .dbg/trae-debug-log-uart-rx-stall.ndjson

## Reproduction Steps
1. 打开设备 Web 页面 UART 面板。
2. 保持当前已知可发送的 UART 参数。
3. 观察接收窗口是否出现被动上报或回显。
4. 发送一段测试数据，确认发送成功。
5. 再次观察接收窗口、状态提示与接口请求结果。

## Hypotheses & Verification
| ID | Hypothesis | Likelihood | Effort | Evidence |
|----|------------|------------|--------|----------|
| A | UART RX 路径再次被并发轮询或重配打断，导致接收读不到数据 | High | Low | Pending |
| B | UART RX 物理链路或运行参数发生漂移，TX 仍正常但 RX 已失配 | High | Medium | Pending |
| C | `/api/v1/uart/console` 返回成功，但缓冲区没有增长，问题在驱动/硬件读路径 | High | Low | Pending |
| D | 页面轮询或渲染状态异常，把“有接收数据”误表现为“无数据” | Medium | Low | Pending |
| E | 最近前端轮询优化或其他代码变更间接影响了 UART 接收链路 | Medium | Medium | Pending |

## Log Evidence
- Instrumentation added to `web/index.html`
- Current browser-side autonomous reproduction failed because `http://192.168.1.25/` is not reachable from the agent browser environment
- 2026-07-20 用户侧复现证据：`GET /api/v1/uart/console` 返回 HTTP 200，`rx_sequence` 持续增长，
  但 `plain` 全为 `.`、`hex` 数百字节全为 `00`（多批次到达，说明数据在持续流入）。
  解读：驱动 → ring buffer → console JSON 链路已通（stall 症状消失）；
  连续纯 0x00 是 RX 线（GPIO26）被持续拉低的典型特征——空闲应为高电平，
  线路恒低时每个帧解为 0x00（帧错误字节照样进 FIFO）。
  波特率失配会混出 FF/乱码，不会出现数百字节纯 0，基本排除。
  排查方向：对端/USB-TTL 未上电（ESD 钳低）、RS-232 电平直连（需 MAX3232）、
  RX 接 GND / 未共地。下一步：万用表量 GPIO26 空闲电平；TX(27)-RX(26) 回环自测。
- 2026-07-20 证据二（config 页手动轮询期间抓取）：与纯 0 不同，出现大量斜坡字节
  (FE/FC/F8/F0/E0/C0/80)、FF，并混有可识别 ASCII（'s' 'f' '4' '-' 'P' Tab '`I$'），
  活动呈一阵一阵的脉冲，脉冲间仍为长串 00。
  解读：对端是活的、在发含 ASCII 的数据；但线路"空闲低 + 向上脉冲"，
  不符合 TTL 空闲高电平特征 → 强烈指向 RS-232 负电平直连被 ESD 钳位（或反相信号）。
  手动轮询扫完所有常用波特率均无连续可读文本 → 电气层问题，非配置问题。
  注意：负电压经 ESD 二极管灌流，持续连接可能损坏 GPIO26。
  下一步：(1) 断开直连，量对端 TX 空闲电压（负压=RS-232，需 MAX3232）；
  (2) 待用户贴回探测明细 JSON（raw_baudrate / probe_reliable / probe_attempts 评分）。

## Verification Conclusion
- Hypothesis C 部分否定：接口返回成功且缓冲区**有**增长，驱动/硬件读路径在工作。
- Hypothesis D 否定：页面渲染无误，缓冲区内容本身就是全 0。
- Hypothesis B 升级为主要方向：RX 物理链路电平异常——证据二进一步细化为
  "线路空闲低 + 向上脉冲"，最符合 RS-232 电平直连（或反相信号），需硬件测量确认。
- [Pending] 等待：对端 TX 空闲电压测量结果 + 探测明细 JSON（raw_baudrate / probe_attempts）。
