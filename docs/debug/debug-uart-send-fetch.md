# Debug Session: uart-send-fetch
- **Status**: [OPEN]
- **Issue**: `index` 页在持续发送一段时间后出现 `发送失败：Failed to fetch`，页面显示最近一次发送内容仍停留在上一帧，预期应继续稳定发送或至少返回明确的业务错误。
- **Debug Server**: http://127.0.0.1:7777/event
- **Log File**: `.dbg/trae-debug-log-uart-send-fetch.ndjson`

## Reproduction Steps
1. 烧录最新固件与 `storage.bin`。
2. 打开 `index` 页面并点击“设备上线”或“车辆上磅”开始持续发送。
3. 运行一段时间后观察页面是否出现 `发送失败：Failed to fetch`。

## Hypotheses & Verification
| ID | Hypothesis | Likelihood | Effort | Evidence |
|----|------------|------------|--------|----------|
| A | `/api/v1/uart/send` 在高频连续请求下返回前发生连接断开或 HTTP 任务异常退出 | High | Med | Pending |
| B | 前端在发送循环中产生了重叠请求或状态竞争，导致浏览器主动终止请求 | Med | Low | Pending |
| C | 发送接口内部存在内存/缓冲区处理问题，累计到一定次数后触发异常 | High | Med | Pending |
| D | 轮询 `/api/v1/uart/runtime` 与发送请求之间存在资源竞争，导致连接偶发失败 | Low | Low | Pending |

## Log Evidence
- Instrumentation added in `web/index.html`:
  - `reportDebugEvent`: unified debug event reporting
  - `sendFrame` guard/start/success/error
  - `stopSending` state snapshot
- Waiting for reproduction logs in `.dbg/trae-debug-log-uart-send-fetch.ndjson`
- Static inspection result:
  - `index.html` drives periodic sending through `playNextFrame()` + `window.setTimeout(...)`
  - backend only sends UART data when `/api/v1/uart/send` is called
  - no device-side autonomous keepalive task was found for `0 kg` sending

## Verification Conclusion
- Current implementation depends on browser execution to keep sending `0 kg` data.
