# C15-2 hook/defer 诊断实现与 Host 验证（2026-11-27）

> 文档状态：PENDING（实施审查）。结果等级：`HOST_VERIFIED_WITH_NOT_COVERED`。
> 设计基线：`ea0825a`（review/c15-hook-defer-design-2026-11-27）。
> 实施分支：`feat/c15-hook-defer-diag-2026-11-27`；未合入 `main`，未操作设备。
> 当前状态：C15-2 `BUSY_REJECTED / NOT_COVERED`，CMD-15 `TEST_PENDING`，P0 `CLOSED`，P2 未启动。
>
> rev2 修订（2026-11-27）：修复 `request_finish` 解锁后读取 `finish_target_state`；H15 改为真实 expire pause + pause 窗口内 clear/notify/arm 交错，并在 teardown 前断言；D3/D4 重新判定。
> rev3 修订（2026-11-27）：H15 增加 barrier 对齐的 arm/clear/notify 三线程并发竞争 O 锁阶段（200 轮，含每轮不变量检查）；A1–A7 按本轮 `OPS_REPS=20` 实测口径记录（A1–A3 每轮 2 断言为 40/40，A4–A7 为 20/20），早期 50 次仅作基线。

## 1. 实施范围

| 文件 | 变更 |
|---|---|
| `main/operation.c/.h` | 目标 hook、O 锁同步、按值终态通知、start per-call diag、test-only 状态/构造入口 |
| `main/cmd.c` | `diag.testhook arm` 支持 `operation_id`、默认 hold、deadline/remaining 响应 |
| `main/cmd_net_job.c/.h` | `s_begin_diag` 增加 defer/active 字段；per-call diag 局部变量复制 |
| `main/CMakeLists.txt` | 新增 `APP_C15_HOOK_TARGET`，要求 `APP_C1_TEST_HOOKS=ON` |
| `tests/c15_hook/` | H1–H27 Host 矩阵与测试驱动 |
| `tests/operation_host/rtos_shim.*` | defer begin 返回值控制、hook 事件日志捕获 |
| `tests/README.md` | 增加 `tests/c15_hook` 入口 |

生产构建默认不定义三个实验宏；未修改生产状态机、Contract 或设备配置。

## 2. 实现要点（对照 ea0825a）

- 旧全局 hook 与目标 hook 共用状态，统一由 `s_ops_mux` 保护；O 内只做有界字段复制/清除，日志、probe、延时在 O 外。
- 目标 arm 只接受 `GOT_IP`、未 finish/expiring、deadline 未到；按 `operation_id` 捕获 generation；校验 `hold_ms >= remaining + 10000`，默认 310000，上限 360000。
- arm 前处理旧 hook：超时清除；目标失效清除；有效或 pending expire 拒绝覆盖。
- pause 支持 cancel/expire 触发类型；目标 cancel 清除 `target_cancelled`，不暂停；非目标 expire 不消费。
- 终态通知改为按值：`notify_target_inactive(op_id, generation)`；调用点覆盖 `set_state` terminal、`request_finish` apply 成功（有/无回调）、`app_operation_cancel` 终态、worker 失败终态；PENDING/STALE/apply 失败不通知。
- `app_operation_cleanup_complete()` 未作为终态通知点。
- `app_operation_start_with_finish_impl()` 增加 `void *diag_raw`；生产入口传 NULL，`app_operation_start_with_finish_diag()` 只在 `APP_C15_BUSY_DIAG` 构建存在；所有返回路径写 `start_result`。
- `cmd_net_job` 使用栈上 `app_operation_start_diag_t`，返回后在 J 内复制到 `s_begin_diag`；没有把全局快照地址传入 operation 层。
- `diag.testhook arm` 目标参数：默认 310000，100–360000；返回 `generation/deadline_ms/remaining_ms/hold_ms`；错误映射 `not_found/not_active/invalid_param`。

## 3. Host 结果

执行命令：

```bash
make -C tests/c15_hook run
make -C tests/operation_host run
make -C tests/cmd_net_job run
```

结果：

| 项 | 结果 |
|---|---|
| `tests/c15_hook` | `PASS_WITH_NOT_COVERED`（H21/H27 NOT_COVERED）；H15 并发阶段连续 10 次运行一致 |
| `tests/operation_host` | `RESULT: PASS`；本轮 `OPS_REPS=20`：A1–A3 各 40/40（每轮 2 条断言），A4/A5/A5h/A7 各 20/20；早期 50 次基线单列，不与本轮混用 |
| `tests/cmd_net_job` | `summary: 0 failed`，T01–T23、D01–D05、D06（begin diag 复制）通过 |
| 语法检查 | `operation.c` / `cmd.c` / `cmd_net_job.c`，3/3 OK（strict） |

### 3.1 H1–H27 明细

| 编号 | 结果 | 说明 |
|---|---|---|
| H1 | PASS | 目标 GOT_IP、expire 命中；hook 消费，op EXPIRED |
| H2 | PASS | 错误 ID → `ESP_ERR_NOT_FOUND` |
| H3 | PASS | 非 GOT_IP → `ESP_ERR_INVALID_STATE` |
| H4 | PASS | finish 已登记、callback 在飞时 arm 拒绝（阻塞 callback 构造） |
| H5 | PASS | 未超时旧全局 hook 阻止目标 arm |
| H6 | PASS | `request_finish(CANCELLED)` apply 后按值通知，不 pause |
| H7 | PASS | `app_operation_cancel` cancel pause 清除 `target_cancelled` |
| H8 | PASS | cancel 后新目标 GOT_IP 可立即 arm |
| H9 | PARTIAL | 用非目标 `notify` 验证不消费；未构造真实“其他 operation expire” |
| H10 | PASS（注入构造） | 用 test-only force 将旧目标置为 terminal，再 arm 新目标，验证 `target_stale` 清理；不是自然槽位复用 |
| H11 | PASS | `set_state(CANCELLED)` 终态通知清除 |
| H12 | PASS | clear 后 disarmed |
| H13 | PASS | 将 hold 设为 remaining+11 s，跳过 deadline 后 pause 走 timeout；未 pause |
| H14 | PASS | 旧全局 hook 超时后 arm 清除并成功 |
| H15 | PASS | 真实 expire pause；pause 进入后、显式 clear 前断言 disarmed；finish 后再断言；另加 barrier 对齐的 3 工作线程 arm/clear/notify 并发竞争 O 锁 200 轮，主线程逐轮检查 disarmed 或 armed 且指向有效目标；pause 计数=1 |
| H16 | PARTIAL | 两个并发 `request_finish(EXPIRED)`：一个 DONE、一个 PENDING、hook 只命中一次；未覆盖 op_worker/expire 任务身份交错 |
| H17 | PASS | defer `INVALID_STATE`：attempted/记结果/未进 active 检查 |
| H18 | PASS | defer OK + active operation：记录 operation 来源 |
| H19 | PASS | defer `NO_MEM`：原错误返回，不映射 busy |
| H20 | PASS | INTERNAL owner：`defer_attempted=false` |
| H21 | NOT_COVERED | 静态审查项：request_finish 已改用登记时快照 `pause_expire`；全路径复核无解锁后 `op->...` 读取，无运行时可观测点 |
| H22 | PASS | hold < remaining+10 s → `INVALID_ARG`，不武装 |
| H23 | PASS | `request_finish` PENDING：不 notify，hook 保留 |
| H24 | PASS | `request_finish` STALE：不 notify，hook 保留 |
| H25 | PARTIAL | 按值 notify 的错误 generation 不命中、正确 ID/gen 命中；未构造真实 notify 前槽位复用 |
| H26 | PASS | `cleanup_complete` 不发布终态，hook 保留 |
| H27 | NOT_COVERED | apply 失败无法通过现有公开 API 构造；静态审查为本路径不 notify |

## 4. 验收条件映射

| 编号 | 状态 | 证据 |
|---|---|---|
| D1 目标 cancel/terminal/stale 清理 | PASS（限定） | H6/H7/H10/H11/H14/H25；H10 为注入构造 |
| D2 非目标 expire 不消费 | PARTIAL | H9 用非目标 notify；真实其他 operation expire 不可构造 |
| D3 解锁后仅快照 | PASS（静态） | P1 修复后复核：request_finish 使用 `pause_expire` 快照；hook 路径无 op 字段解锁读取 |
| D4 O 保护 | PASS | H15 真实 pause + barrier 三线程 O 锁竞争 + 静态审查 |
| D5 `s_begin_diag` J 同步 | PASS | D06 begin-diag-copy + 静态审查；写/读均在 J |
| D6 defer 锁路径 | PASS（静态） | §3.4 设计表；源码逐路径复核 |
| D7 H1–H27 | PARTIAL | H21/H27 NOT_COVERED；H9/H10/H16/H25 为限定构造 |
| D8 生产符号隔离 | PASS | 生产 ELF 测试符号 0；测试 ELF 有符号 |

## 5. 构建与符号隔离

| 构建 | 命令/配置 | 结果 |
|---|---|---|
| 生产 | `idf.py -B build-prod -DIDF_TARGET=esp32 build` | 成功；`iothub.bin` `09561eef2e8919619eb617e976de64316cd5ee5f986d72d3cf746afad16dd776` |
| 全实验宏 | `-DAPP_C1_TEST_HOOKS=ON -DAPP_C15_HOOK_TARGET=ON -DAPP_C15_BUSY_DIAG=ON -DAPP_C15_BUSY_DIAG_TAG=C15-BUSY-DIAG-20261127-01` | 成功；`iothub.bin` `a9012ecaa26d9d9271aef60b29e891e283b75e1daf19f8270a911fccde2762b4` |
| C1+目标 hook | `-DAPP_C1_TEST_HOOKS=ON -DAPP_C15_HOOK_TARGET=ON`（BUSY OFF） | 成功；验证 cmd.c 目标 arm 分支在无 BUSY 构建可编译 |
| 非法组合 | `APP_C15_HOOK_TARGET=ON` 且 C1 OFF | CMake 按预期拒绝：`APP_C15_HOOK_TARGET=ON requires APP_C1_TEST_HOOKS=ON` |

- IDF：`ESP-IDF v6.0.3-707-gdac489c6718`。
- `nm -g build-prod/iothub.elf` 中 `app_operation_test_hook*`、`app_operation_start_with_finish_diag`、`app_c15_busy_diag*` 计数 0。
- 测试 ELF 对应符号计数 10。
- Host 侧额外编译了 `operation.c` 的 6 种宏组合（无宏、C1、BUSY、C1+TARGET、C1+BUSY、C1+TARGET+BUSY），均通过 `-Wall -Wextra -Werror`。

## 6. 静态审查结论

- 新增 hook 状态全部由 O 保护；O 内无日志、无延时、无阻塞调用；probe `set_state` 在 O 外调用。
- `request_finish` 的 expire pause 判定使用登记时的 `pause_expire` 快照；callback 返回后不读取 `op->finish_target_state`。
- 终态通知按值匹配，不依赖 operation 指针；槽位复用/找不到目标只清旧 hook。
- PENDING/STALE/apply 失败不冒充终态；hook 保留到 timeout 或后续失效检查。
- `app_operation_cleanup_complete()` 不发布终态，未通知。
- `s_begin_diag` 写/读都在 J 临界区；per-call diag 不再共享。
- H15 在真实 expire pause 进入后、显式 clear 前断言 hook 已被 pause 消费；另加 barrier 对齐的 3 工作线程竞争 O 锁 200 轮，主线程逐轮检查 hook 状态不变量。
- 未发现 O→J、O→M、M→O 嵌套；生产 defer 调用顺序未改变。

## 7. 未覆盖与限制

- H21 是静态审查项，无运行时断言。
- H27 apply 失败路径未构造；当前实现按设计不 notify。
- H9 未构造真实其他 operation expire；用非目标 notify 验证 skip。
- H10 使用 test-only force 构造 terminal 目标，不是自然槽位复用。
- H16 是两个并发 `request_finish`，不是 op_worker 与 expire 任务的身份级交错。
- H25 直接调用按值 notify 验证身份匹配，未制造真实 notify 前槽位复用窗口。
- `tests/c15_hook/test_c15_hook.c` 为 953 行的一次性矩阵，超过项目测试文件建议阈值，未在本次拆分；作为后续维护项保留。
- 未进行设备烧录、设备实验、main 合入或 P2。

## 8. 边界

本次结果只证明 Host 控制流、构建和符号隔离；不证明设备行为、Wi-Fi/MQTT 真机时序或 C15-2 覆盖率提升。C15-2 仍为 `BUSY_REJECTED / NOT_COVERED`，CMD-15 仍为 `TEST_PENDING`。

## 9. 后续更新（2026-11-28）

本节为追加更新，不改变上文限定时点结论。2026-11-28 已完成 C15-2 **hook 路径**设备验证（A0 + A1），结果见 [C15-2 hook 路径设备验证（2026-11-28）](C15-2_hook路径设备验证_2026-11-28.md)。该验证仅覆盖 hook 路径判据，不关闭 C15-2 原始 MQTT attributes/telemetry 来源冲突的 `NOT_COVERED`；CMD-15 仍为 `TEST_PENDING`。
