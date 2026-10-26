# A 阶段 · token store 统一鉴权（契约与实施记录）

> 状态：**A 阶段代码完成；host 单测/静态审计/整机编译/板上回归通过，待评审签收**
> 评审裁决（2026-10-26）：**接受新增 `auth_token.c` 的偏差**。原实施标准“不新增源文件”据此修订；理由是该文件使 host 单测可直接编译真实源码，避免为 `auth.c` 的 `config/config` 依赖伪造整套配置结构体，且 `CMakeLists` 仅增加一行。后续同类偏差须先报批，不得由实施方单方面改写标准。
> 范围：只做 BLE 侧的 token 存储层统一与那个「空 token 响应」bug 的收口。
> 串口维护通道、B/C 阶段的会话生命周期**不在本次范围内**。
> 日期：2026-10-26 · 基线 ESP-IDF v6.0.3-707

---

## 1. 为什么做这件事

原来 `ble_cmd.c` 自己持有一对变量：

```c
static char    s_token[33];
static int64_t s_token_expire_ms;
```

签发（逐字节 `snprintf` 写入 `s_token`）、校验（`strcmp`）、断连清理（置 `'\0'`）
三个入口分别在不同任务/回调里跑，**没有锁**。GATT 回调与命令任务并发时，
可以读到「新 token 内容 + 旧过期时间」这种撕裂组合——表现为登录刚成功就
提示未鉴权，且难以复现。

另外，串口维护通道接入后需要同一套语义的**第二个来源**。如果继续在各自模块里
存一份 token，两边的 TTL、比较方式、清理时机必然分叉。

所以抽出 `auth_token.c`：把「槽位 + TTL + 常量时间比较」收在一处，
所有访问在同一把临界区锁内完成。

---

## 2. 契约（`auth.h`）

```c
#define APP_TOKEN_HEX_LEN   32
#define APP_TOKEN_TTL_MS    600000      /* 10 分钟，原 BLE token TTL 迁入 */

typedef enum {
    TOKEN_SRC_BLE = 0,
    TOKEN_SRC_SERIAL,   /* 槽位已就绪但尚未接入 */
    TOKEN_SRC_MAX,
} token_source_t;

bool app_token_store_issue(token_source_t src, char *out, size_t out_size);
bool app_token_store_validate(token_source_t src, const char *token);
void app_token_store_clear(token_source_t src);
void app_token_store_clear_all(void);
```

语义要点（写进头文件注释，改动前先对齐）：

| 行为 | 约定 |
| --- | --- |
| `issue` 成功 | 返回 `true`，`out` 保证 NUL 结尾；`out_size < 33` 或 `src` 越界返回 `false`，**且不破坏已签发的旧 token** |
| `issue` 回包 | 只从局部缓冲取，**禁止二次读槽位**（否则并发 clear 会把刚发的 token 打成空串） |
| `validate` | 格式检查（32 位十六进制）在锁外；32 字节定长异或在锁内 |
| `validate` 失败 | **不清槽**——否则一个错误 token 就能把有效会话踢下线 |
| `validate` 过期 | 锁内顺手清槽；判断用 `now_ms >= expire_ms`（到点即失效） |
| `clear_all` | 单次临界区内清全部槽，不允许「清了一半被打断」 |

---

## 3. 实现要点

- 新增 `main/auth_token.c`（约 140 行），**不并入 `auth.c`**：`auth.c` 依赖
  `config.h` / `common.h`（口令哈希路径），并进去会让地基单测必须伪造整套配置结构体。
  现在 `auth_token.c` 只依赖 `auth.h` + `esp_random.h` + `esp_timer.h` + `freertos/FreeRTOS.h`，
  单测可以编译**真实源码**而不是复制一份逻辑。该偏差已由评审书面接受。
- `s_lock` 用 `portMUX_INITIALIZER_UNLOCKED`；临界区内只出现
  `memcpy`（33 字节固定拷贝）、`expire_ms` 读写比较、32 次异或、以及
  `token_slot_clear_locked()` 的 33 次固定写。随机数、hex 编码、取时间全在锁外。
- 清槽走 `token_slot_clear_locked()` 的定长写，**不用 `memset`**：
  临界区里只保留固定长度的内存操作，静态审计才能机械判定。
- 旧宏名 `BLE_CMD_TOKEN_TTL_MS` 已删除；`ble_cmd.c` 直接使用 `APP_TOKEN_TTL_MS`，
  `grep` 零残留。

### 已修的老 bug

`issue` 失败时，旧代码会走「正常回包」路径并把空 token 发出去——小程序收到
`{ok:true, token:""}` 之后所有命令都被拒，但登录看起来是成功的。现在失败
直接返回 `{ok:false, error:"internal_error"}`，有确定出口。

---

## 4. 接线改动

| 文件 | 位置 | 改动 |
| --- | --- | --- |
| `main/auth_token.c` | 新增 | 槽位实现（140 行） |
| `main/auth.h` | 末尾 | 追加契约声明与设计约束注释 |
| `main/CMakeLists.txt` | `SRCS` | 加入 `auth_token.c` |
| `main/ble_cmd.c` | 头部 | 保留既有 `auth.h` include；删 `esp_random.h`；TTL 改用 `APP_TOKEN_TTL_MS`，旧宏名删除 |
| `main/ble_cmd.c` | 静态变量 | **删除** `s_token[33]`、`s_token_expire_ms` |
| `main/ble_cmd.c` | `app_ble_cmd_issue_token` | 改为一行转调 `app_token_store_issue(TOKEN_SRC_BLE, ...)`，返回 `bool` |
| `main/ble_cmd.c` | `app_ble_cmd_authed` | 删 `strcmp`，改调 `app_token_store_validate` |
| `main/ble_cmd.c` | 登录成功处 | 用局部 `char token[APP_TOKEN_HEX_LEN+1]`，失败路径返回 `internal_error` |
| `main/ble_cmd.c` | `app_ble_cmd_on_disconnect` | 只保留 `app_token_store_clear(TOKEN_SRC_BLE)`；`s_last_cmd_ms`、行缓冲、TX 队列、network candidate 清理**保持原样** |
| `main/cmd.c` | `web.auth.set` | 口令哈希**落盘成功之后**才 `app_token_store_clear_all()`；成功响应加 `sessions_invalidated:true`；`save_failed` 路径不动 |

`web.auth.set` 的时机是刻意的：先 `app_config_save()` 成功，再构造成功响应，
最后 `app_token_store_clear_all()`。失败路径不能清 —— 否则用户输错一次旧口令
就把自己踢下线。响应发送路径不读 token，因此清理不会截断 `sessions_invalidated`
响应；即使响应对象分配失败，改密已落盘，仍会清全部会话。

`app_token_store_clear_all()` 遍历 `TOKEN_SRC_MAX`，单测同时签发 BLE + SERIAL
两个槽再清全部；未来新增来源会自动纳入，不允许退化成只清 BLE。

---

## 5. 不在 A 阶段范围

- **串口维护通道未接入**。`TOKEN_SRC_SERIAL` 只有槽位和单测覆盖，
  不代表串口鉴权已经可用。
- 审计遗留项，原样记录，留待 B/C 阶段核对：

  > 串口 token 清理时是否连带取消其 trial/operation 留待 B/C 核对

- B/C 阶段（串口生命周期、会话与 operation 的关联）**已冻结**，
  本次不顺手做任何假设性改动。

---

## 6. 验证

### 6.1 编译

整机编译已通过：

```
idf.py -B build build
→ app 0x1173a0 = 1,143,712B，分区余量 42%（IDF 6.0.3）
→ 较 A 阶段前 1,143,152B 增加 560B（0x230）
```

仅有 `uart.c` 既有的 unused-function 警告；token 改动没有新增警告。

单文件快速检查（复用 `build/compile_commands.json`）：

```
python3 tests/syntax_check.py --strict main/auth_token.c main/ble_cmd.c main/cmd.c
→ 3 个文件：3 通过 / 0 失败
```

### 6.2 token store 单测（验收第 0 项，不经 ble_cmd）

`tests/token_store/`，7 组 58 条断言，**全部通过**；连续 10 次运行 10/10，ASan/UBSan 58/0：

| 组 | 覆盖 |
| --- | --- |
| 1 | 签发格式（32 位大写 hex + NUL）、立即校验、两次签发不重复 |
| 2 | 重签使旧 token 立刻失效，不并存 |
| 3 | `out == NULL` / `out_size < 33` / `src` 越界（枚举外、99、-1）/ 失败调用不破坏已有 token |
| 4 | NULL、空串、31 字符、33 字符、含非 hex、中间截断、小写形式（不通过且不清槽） |
| 5 | TTL 边界：`TTL-1ms` 有效、`== TTL` 正好失效（守住 `>=`）、过期后重复校验 |
| 6 | 双槽隔离、跨槽 token 不通过、错 token 不清槽、`clear` 只清指定来源、`clear_all` 清全部 |
| 7 | 4 线程 × 20000 次并发签发/校验/清槽 |

单测桩把临界区映射成**真实 pthread 互斥锁**，不是空实现 —— 空实现会让
「锁写漏」在并发用例里完全看不出来。

### 6.3 临界区静态审计（把审查清单变成可执行检查）

`tests/token_store/audit_critical_sections.py`，10 项全 PASS：

```
A1  临界区 enter/exit 逐个配对、无嵌套
A2  临界区内无日志/随机数/时间/分配/阻塞调用
A3  无 memcmp/strcmp 等短路比较
A4  比较为定长 APP_TOKEN_HEX_LEN 次异或、无提前退出
A5  过期判断用 >=（到点即失效，不是 >）
A6  isxdigit 参数已转 unsigned char
A7  issue 回包取自局部缓冲，未二次读槽位
A8  clear_all 在单次临界区内一次清完
A9  常量时间比较发生在锁内（未复制出锁再比较）
A10 清槽辅助函数只在持锁状态下被调用
```

审计器会先剥掉注释再匹配——注释里出现 `isxdigit(` 和 `portENTER_CRITICAL`
这类字样，不剥会直接误报（第一版就栽在这里）。

### 6.4 板上回归（2026-10-26，C8E4，已完成）

正常 TTL 固件（`APP_TOKEN_TTL_MS=600000`）按 6 组执行，结果
**24/24 PASS**，证据 `tools/ble_p0/results/a_token_board_20261026_185258.json`
（`password_restored=true`）：

| 组 | 实际结果 |
| --- | --- |
| 0 无 token `device.info` | PASS |
| 1 登录 / 受保护命令 | PASS；`expires_in=600`，token 32 位，受保护命令成功 |
| 2 错误口令 | PASS；v1 `unauthorized`，**旧 token 仍有效** |
| 3 同来源重登 | PASS；旧 token `unauthorized`，新 token 有效 |
| 4 断连重连 | PASS；旧 token `unauthorized`，重新登录后新 token 有效 |
| 5 改密 | PASS；响应 `sessions_invalidated=true`；旧 token `unauthorized`；旧口令 `unauthorized`；新口令可登录；恢复原口令后临时 token `unauthorized`、原口令可登录 |
| 6 大小写 | PASS；大写有效；小写 v1 `unauthorized`、v0 `auth_required`；大写 v0 有效 |

首轮 `a_token_board_20261026_185208.json` 在 v0 小写用例超时：测试器给 v0 用了
字符串 id，而固件 v0 只回显数字 id（`id=-1`），属于测试器缺陷；改用数字 id 后
重跑 `185258` 全 PASS。该失败不计为固件缺陷。

TTL 专项（临时 `APP_TOKEN_TTL_MS=3000`，独立构建/烧录）：

- `a_token_board_ttl_20261026_185425.json`：登录 `expires_in=3`；等待 4.2s 后
  受保护命令返回 `unauthorized`，**PASS**。
- 临时构建/烧录：`build_a_token_ttl3000_20261026.log`、`flash_a_token_ttl3000_20261026.log`。
- 恢复 `APP_TOKEN_TTL_MS=600000` 后**重新构建并重新烧录**：
  `build_a_token_final600_20261026.log`、`flash_a_token_final600_20261026.log`；
  源码宏实测为 600000。
- 最终固件抽验 `a_token_final_smoke_20261026.json`：登录 `expires_in=600`、
  token 32 位、受保护命令成功、MQTT connected、STA has IP，**PASS**。

回归旁证：

- UART1/BLE 数据面 `t3_uart1_20261026_185634.json`：U2B 256B/2048B、B2U
  200B/1000B 全部逐字节一致，**PASS**。
- 最终固件串口 `a_token_final_serial_20261026.log`（SSID/MQTT host 已脱敏）：`diag` 采样
  `heap free=48908 min=44864 largest=43008 level=normal`，MQTT connected，
  `adv=1`，无 panic/重启；串口打开可能触发一次复位，日志以该次启动为起点。
- 板上测试脚本：`tools/ble_p0/a_token_board_test.py`；原口令从
  `IOTHUB_ADMIN_PASSWORD` 读取，临时口令在内存生成并在 finally 中恢复，
  结果不含 token/口令。

边界：600s 完整等待未在板上实等；TTL 语义用 3000ms 临时构建验证，最终 600s
固件只做登录/`expires_in` 抽验。两者是不同构建，已分别记录。

### 6.5 仍需人工确认

- 小程序侧识别 `auth.login` 失败的新 `internal_error` 出口（此前是空 token）。
- 协议文档已更新：T4 §5.2 token 大写/大小写敏感；§6.11 改密
  `sessions_invalidated:true` 及响应顺序。

---

## 7. 已知边界

- `token_format_ok()` 按 C 字符串语义工作：要求调用方传入 NUL 结尾的字符串。
  当前两个调用点都是（cJSON 的 `valuestring` / 局部缓冲）。若将来有人把
  32 字节非 NUL 结尾的数组塞进来，会在 `tok[32]` 处越界读 1 字节。
- 临界区覆盖的是「槽位读写」，不覆盖「比对结果的使用」。也就是说
  `validate` 返回 `true` 之后、业务动作执行之前，token 仍可能被 `clear`
  （断连）。这是可接受的——断连后命令本就该失败。
- `clear_all()` 会连串口槽一起清。这一行为在改密场景是对的，但串口通道
  接入后需要确认：清掉串口 token 是否应该同时终止其正在进行的 operation。
  见 §5 的 B/C 遗留项。
