# json_escape — JSON 转义等价性宿主单测(**不参与固件构建**)

本目录只有测试;**被测源码是 `main/json_escape.c`(固件也在用同一份)**,
所以这里的通过结果直接代表端上行为,不存在"参考副本漂移"问题。

## 为什么需要

`/api/v1/uart/console` 原先"整段转义到堆缓冲 + 一次性 `httpd_resp_sendstr`",
单次请求要 **2,049 + 6,145 + 8,448 = 16,642 B** 同时存活的堆块,叠加 TCP 发送缓冲
(5,760 B)后超过稳态可用堆(~19.6 KB)→ 必然 `HTTP 500 out_of_memory`。

改造为"锁内静态快照 + 锁外 `httpd_resp_send_chunk` 分片转义发送"后**堆零分配**,
但前提是:**分片输出必须与旧的整段输出逐字节一致**,否则前端解析行为会变。
本单测就是这条不变式的守门人(见 `问题与版本优化.md` 第 45 项)。

## 运行单测

```bash
gcc -Wall -Wextra -I main -o /tmp/t_json_escape \
    tools/json_escape/test_json_escape_stream.c main/json_escape.c && /tmp/t_json_escape
# 期望:结果: 共 70 例, 失败 0
```

## 覆盖点

| 用例 | 目的 |
| --- | --- |
| `all-quotes` | 每字节膨胀 2 字节(`\"`),覆盖片边界 |
| `c0-controls` | 0x01-0x1F 全量走 `\u00XX`(6 字节原子,最长) |
| `escapes-set` | `\\ \/ \b \f \n \r \t` 全表 |
| `del-and-high` | 0x7F / 0x80 / 0xFF 不转义(与旧实现一致) |
| `utf8` | 多字节 UTF-8 序列不被拆坏(`你好` + 4 字节 emoji) |
| `console-plain-like` / `console-hex-like` | 模拟真实 `rx_plain` / `rx_hex` 载荷 |
| `long-hex-3073` | 与 `UART_CONSOLE_HEX_SIZE` 等长的长串跨多片 |

每个用例都会用 **7 种分片窗口**(6/7/8/13/64/512/4096)各跑一遍:窗口 6 是"刚好放下最长
转义原子"的极限,4096 是"一次装完"。

## 约束

`json_escape.h/.c` **刻意保持零 ESP-IDF 依赖**(只用 `<stdio.h>` / `<string.h>` /
`<stdbool.h>` / `<stddef.h>`),就是为了能让本单测直接编译源码。**请勿在其中引入 IDF 头文件。**
