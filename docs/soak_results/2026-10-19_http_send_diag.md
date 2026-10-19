# 2026-10-19 HTTP 页面发送路径诊断（只插桩，不改行为）

## 构建

- B `192.168.1.42`，`ota_0`，`used=1,281,056`；
- 512B 初始分块、selftest 避让、MQTT 低优先级协调代码均在；
- 本轮只增加计时/计数，不改变重试次数、延时、chunk 大小或发布策略。
- 负载：页面 `/config` 每 30s、`led.toggle` 每 5s、status 每 2s，10 分钟。

## 调用链事实（IDF 6.0.2 `httpd_txrx.c`）

- `httpd_resp_send_chunk()` 每个 chunk 至少 3 次 `httpd_send_all()`：chunk 长度、数据、CRLF；
  首 chunk 还要发送状态行和所有响应头。
- `httpd_send_all()` 在部分发送时循环调用 `send_fn`。
- 首 chunk 有 `malloc(required_size)` 临时拼状态行；大小取决于状态行/头，不是整页。
- 我们的 `vTaskDelay(20ms)` 在 `httpd_resp_send_chunk()` 返回后，
  即 len/data/CRLF 全部发完后，不是每次底层 `send()` 后。

## 10 分钟数据（22 页，含 3 页起始验证）

| 指标 | 结果 |
| --- | ---: |
| `send_fn` 调用总数 / attempts | 1,523 / 1,525 |
| 每页调用（首/后续） | 59 / 80 |
| 每页 >128B 数据发送 | 13 / 20 |
| ≤16B / ≤128B / >128B | 1,118 / 44 / 361 |
| 部分发送（partial） | 0 |
| EAGAIN 总数 | 2 |
| EAGAIN 分布 | **≤16B: 0，≤128B: 0，>128B: 2** |
| 页耗时 p50 / p95 / max | 338ms / 588ms / 597ms |
| `chunk_max_us` p50 / max | 23.3ms / 53.5ms |
| `send_fn_max_us` p50 / max | 5.46ms / 37.8ms |
| `override_max_us` p50 / max | 6.21ms / 48.8ms |
| HTTP 200 / 503 / 超时 | 265/265 / 0 / 0 |
| critical | 0 |
| 页内最低 free / largest | 2,208 / 1,152 |
| `largest<2048` 页 | 17/22 |
| `largest<1024` 页 | 0/22 |
| 起始 largest | 9,216 |

## 结论

1. **不是“3 次 send”**：一个页面约 59–80 次 `send_fn`，数据块只占 13–20 次；
   头/长度/CRLF 占大部分调用，但它们没有 EAGAIN。
2. **EAGAIN 集中在 >128B 数据发送**：本次 2 次 EAGAIN 全部落在数据块；
   没有观测到小头部/CRLF 触发 EAGAIN。样本还小，A/B 需要继续累计。
3. **httpd 占用**：页面中位 338ms、最大 597ms；`chunk_max` 最大 53.5ms，
   `send_fn` 最大 37.8ms，`override` 最大 48.8ms。没有出现“20×30ms”级别的重试循环；
   600ms 只是上界，不是本次实测值。
4. **低堆不需要 EAGAIN 也会发生**：17/22 页 `largest<2048`，大量页面重试为 0；
   页面仍把 largest 从 9,216 压到 1,152。说明成功发送路径上的缓冲占用才是主现象，
   不能只用“重试等待”解释。
5. **仍不能断言是 Wi-Fi TX 池唯一根因**：本轮没有 lwIP/Wi-Fi buffer stats；
   但数据已经缩小到“数据块发送 + 成功路径的缓冲分配”，而不是头部小包或重试循环。

## 下一步

- 保持 512B、其他行为不变，先跑 A0/A1/A2：chunk 间隔 20/50/100ms；
  每组 20–30 分钟，记录 page_ms、调用数、EAGAIN、页内最低、命令延迟。
- 暂不做发送级 pacing；等 chunk 级结果证明“间隔确实能改善低谷且不显著伤命令延迟”再单独验证。
- 暂不打开 `CONFIG_LWIP_STATS`/Wi-Fi buffer dump；下一轮再确认头文件/配置/链接，
  做页面前后各一次的低频诊断，避免 stats 本身改变内存与时间。
- 通过标准保持：critical=0、FD 失败=0、非计划 MQTT 断链=0、HTTP/命令无非计划失败，
  且页面耗时/命令延迟/BLE 数据无明显恶化。

## 原始数据

- 串口：`/tmp/link_B_diag.log`
- 测试：`/tmp/diag_baseline/`（10 分钟）
