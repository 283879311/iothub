# CMD 当前任务台账（固件 1.3）

> 文档状态：CURRENT。源码核对基线见 [固件 1.3 文档维护基线](../baseline/固件1.3文档基线_2026-11-18.md)；任务台账只保留证据提交/短 SHA，不维护全局源码基线 SHA。
>
> 本文只维护 CMD 当前任务状态、未完成事项、关键实现约束和必要验证证据。历史交接、申请修订过程和重复审查叙述不在此累计。当前命令行为以源码及 `contract/v1/` 为准；Contract 仍为 `1.1-draft`。
> **状态唯一性：** 本文是 CMD 任务状态、优先级和关闭条件的唯一维护入口。CMD-02/03、CMD-04 系列、CMD-05 等专项文件只保存其对应的设计/审查/验证证据，不得另行维护当前任务状态；如与本文状态描述冲突，以本文为准。

## 1. 当前任务状态

状态含义：`TODO` 未开始；`REVIEWED` 静态审查完成；`IN_PROGRESS` 有未关闭的实施/验证工作；`TEST_PENDING` 代码已改但指定验证未完成；`PASS` 必须有对应代码、测试和验收证据；`DEFERRED` 经明确决策暂缓。

| ID | 优先级 | 当前事项 | 状态 | 当前结论 / 关闭条件 |
|---|---|---|---|---|
| CMD-01 | P0 | `io.get` Wi-Fi runtime 读取快照 | PASS | 修复 `add93e7`；构建通过；BLE/MQTT `io.get` 与 `network.status` 一致性对读通过（[复测记录](../reviews/WIFI-OP-RESOURCE-RACE_CMD-01_设备复测记录_2026-11-22.md)，设备 65D8 / `1.3-1ff92db`） |
| CMD-02 | P0 | `s_net_async` 字段所有权与读点 | PASS | 七处读点已实施（`28f9090`）；阶段 1 ESP-IDF fullclean/build 和阶段 2 host tests 按执行报告通过；阶段 3 于 2026-11-26 执行 r1–r7：C2-1、C2-2、C2-3 absent、C2-4 每轮 PASS，C2-3 `got_ip` NOT_COVERED（整体 PARTIAL，按授权接受），判定 PASS（[报告](../reports/CMD-02_15_阶段3设备回归结果_2026-11-26.md)，[证据](../evidence/CMD-02_15_stage3_2026-11-26/README.md)） |
| CMD-03 | P0 | operation 锁序、回调时序及 expiring 窄窗口 | PASS | H1-P1 已进入 `main`：本轮执行基线 `0e4d49c`（PR #4 merge；全局源码维护基线见 `docs/baseline/`），证据文档 `956afcd`。Host `tests/cmd_net_job` 为 24 个可执行用例（T09 拆为 T09a/T09b）：23 PASS + T09a EXPECTED、0 failed；`tests/operation_host` S1–S5 各 50/50；断言 A1/A2/A3/A4/A5/A5h/A7 均 PASS（无独立 A6 断言）；ESP-IDF build 证据来自 `0e4d49c`（PASS），`956afcd` 仅证据文档合并。设备 65D8 / `1.3-0e4d49c` / `build_sha=659cf7f1c` 的外部可触发 cancel、重复 cancel、BLE 断连 cleanup、持久恢复、网络/MQTT 恢复 PASS（[证据](../evidence/H1-P1_device_main_2026-11-24/README.md)）。阶段 A v2 clean-cycle（测试专用 helper 分支 `407be43`，未合入 `main`）设备侧 T13/T19 PASS，NVS 前后一致，生产 `main` `956afcd` 已恢复并核验（[证据](../evidence/CMD-03_phase_a_clean_2026-11-25/README.md)）。阶段 B WAIT200 于 2026-11-25 完成：空闲 10/10 + BLE 负载 10/10 PASS，全调用墙钟 200–210 ms，`T_max=250 ms` 已冻结；NVS 整分区不一致（仅 `phy/cal_data`），按 D1 记为系统管理数据例外，持久配置一致（[证据](../evidence/CMD-03_phase_b_wait200_2026-11-25/README.md)，[验收规范](../reviews/CMD/CMD-03_WAIT200与NVS验收规范_2026-11-25.md)）。阶段 B 已按定义 A 关闭（限 WAIT200 测量与验收决策，见[规范 §4](../reviews/CMD/CMD-03_WAIT200与NVS验收规范_2026-11-25.md)）。CMD-03 已按路径 A 关闭为 `PASS`（2026-11-25 独立评审，依据上述 Host/设备证据与路径 A 设备适用性口径）；P0 已按路径 A 关闭（2026-11-26，CMD-02 阶段 3 PASS）。设备侧 T01–T12、T14–T18、T20–T23 按路径 A 以 Host 证据验收，不纳入设备补测（设备侧 NOT_COVERED）。P0 处置为 `OPEN`（见 §2.1）。 |
| CMD-04 | P1 | status 响应快照化 | IN_PROGRESS | `main/cmd.c` 中已存在 04a `app_cmd_net_operation_status` / `app_cmd_net_op_status_capture` 与 04b `app_cmd_build_network_status` / `app_cmd_network_status_snap_t` 实现入口；目前仅确认源码存在，尚未完成对冻结设计的逐分支静态复核及完整验收。04c 已实施部分仍以专项审查记录为准。
| CMD-05 | P1 | config/profile 全局锁序 | REVIEWED | 静态审查确认偏序 `J → C → P`；未测持锁时长，CMD-06 必须遵守递归锁和锁序约束 |
| CMD-06 | P1 | 网络异步事务迁移到 `cmd_net_txn.c` | TODO | 等待阶段 A 相关风险与验证收口；迁移不得改变状态机、owner、generation、cleanup 和锁序 |
| CMD-07 | P1 | 普通网络命令迁移到 `cmd_net.c` | TODO | scan/profile/reconnect/status 行为及权限保持一致 |
| CMD-08 | P1 | 状态构造迁移到 `cmd_status.c` | TODO | device/network/gpio/uart 字段及快照语义保持一致 |
| CMD-09 | P2 | GPIO 控制共享 helper | TODO | 旧命令行为保持一致 |
| CMD-10 | P2 | 配置命令迁移到 `cmd_config.c` | TODO | domain/patch/effect/权限/事务 guard 保持一致 |
| CMD-11 | P2 | UART/IO/data 命令迁移到 `cmd_device.c` | TODO | 命令协议、错误码和来源权限保持一致 |
| CMD-12 | P2 | CMD 内部重复 helper 与旧 wrapper 清理 | TODO | 先核对全仓调用者；再评估 owner 映射、domain 白名单、管理员密码校验命名等重复点；无调用证据前不删除 |
| CMD-13 | P2 | 收敛 `cmd.c` 入口职责 | TODO | 仅保留 context、policy、分发和统一收尾；以实际代码结构为准更新 README |
| CMD-14 | P1 | 测量 `apply_async` 持 job 锁跨 NVS 的时间 | TODO | 先定义测量方法并取得相应授权；测量前不得缩短锁区 |
| CMD-15 | P1 | MQTT 运行态读取 `s_config` 的一致性 | TEST_PENDING | R1/R2（`999db00`）及 R4（`8d7d768`）已实施；联合构建和 host tests 按执行报告通过；阶段 3 于 2026-11-26：C15-1 PASS（r3/r5），C15-2 NOT_COVERED（attributes/telemetry 字段来源冲突、时序抖动；v9 退役，需专项修正后重测）；整体保持 TEST_PENDING（[报告](../reports/CMD-02_15_阶段3设备回归结果_2026-11-26.md)）；R3/R5 未授权 |

## 2. 当前未关闭事项与推进依赖

1. **CMD-02 / CMD-15**：阶段 1 ESP-IDF fullclean/build 和阶段 2 host tests 于 2026-11-16 按执行报告通过。阶段 3 于 2026-11-26 执行 r1–r7：CMD-02 的 C2-1/C2-2/C2-3 absent/C2-4 每轮 PASS，C2-3 `got_ip` NOT_COVERED（整体 PARTIAL，按授权接受），判定 PASS；CMD-15 的 C15-1 在 r3/r5 PASS，C15-2 NOT_COVERED，整体保持 TEST_PENDING。证据：[阶段 3 报告](../reports/CMD-02_15_阶段3设备回归结果_2026-11-26.md)、[证据目录](../evidence/CMD-02_15_stage3_2026-11-26/README.md)。
2. **CMD-01**：已闭环（`add93e7` + 构建 + BLE/MQTT 回归），状态转 PASS；证据见 [复测记录](../reviews/WIFI-OP-RESOURCE-RACE_CMD-01_设备复测记录_2026-11-22.md)。不能用 CMD-02/15 的联合 host 结果替代该结论。
3. **CMD-03**：H1-P1 已合入 `main`（本轮执行基线 `0e4d49c`；全局源码维护基线见 `docs/baseline/`，证据归档 `956afcd`）。Host `tests/cmd_net_job` 为 24 个可执行用例（T09 拆为 T09a/T09b）：23 PASS + T09a EXPECTED、0 failed，`tests/operation_host` S1–S5 各 50/50、断言 A1/A2/A3/A4/A5/A5h/A7 均 PASS（无独立 A6 断言）；ESP-IDF build 证据来自 `0e4d49c`（PASS），`956afcd` 仅证据文档合并。设备 65D8 / `1.3-0e4d49c` 的外部可触发 cancel、重复 cancel、BLE 断连 cleanup、持久恢复、网络/MQTT 恢复 PASS。阶段 A v2 clean-cycle 使用测试专用 helper 分支 `407be43`（未合入 `main`）完成设备侧 T13/T19 PASS、NVS 前后一致和生产 `main` `956afcd` 恢复核验（[证据](../evidence/CMD-03_phase_a_clean_2026-11-25/README.md)）。阶段 B WAIT200 于 2026-11-25 完成：空闲 10/10 + BLE 负载 10/10 PASS，全调用墙钟 200–210 ms，`T_max=250 ms` 已冻结；NVS 整分区不一致（仅 `phy/cal_data`），按 D1 记为系统管理数据例外，持久配置一致（[证据](../evidence/CMD-03_phase_b_wait200_2026-11-25/README.md)，[验收规范](../reviews/CMD/CMD-03_WAIT200与NVS验收规范_2026-11-25.md)）。T01–T12、T14–T18、T20–T23 按路径 A 以 Host 证据验收，设备侧 NOT_COVERED，不纳入设备补测。因此 CMD-03 已按路径 A 关闭为 `PASS`（2026-11-25 独立评审）；阶段 B 已按定义 A 关闭（限 WAIT200 测量与验收决策，见[规范 §4](../reviews/CMD/CMD-03_WAIT200与NVS验收规范_2026-11-25.md)）。P0 已按路径 A 关闭（2026-11-26，CMD-02 阶段 3 PASS）。
4. **CMD-04**：04a/04b 的实现入口已存在于当前 `main/cmd.c`，但尚未完成按设计清单逐分支核对，不能仅凭函数存在标记完成或 PASS；对应设计文件仍标记 PENDING，表示设计材料本身的生命周期，不替代本台账中的实施状态。后续需核对 wire 字段/分支、失败路径、锁内外边界、资源预算及回归证据。04c 已实施部分不代表整个 CMD-04 关闭；其中同步 tryconnect 分支已确认不属于当前 v1 wire 可达路径，不得写成 wire 真机验收 PASS；apply dispatcher 的 trial patch 路径仍需按 async apply 全流程理解。
5. **CMD-05 / CMD-14**：锁序静态审查完成，但长持锁尚未实测。CMD-14 测量前不得因“看起来持锁过久”直接缩短临界区。
6. **CMD-06 及后续模块迁移**：须等待相关锁契约、所有权边界和必要验证收口，再单独评审实施；不得把文件迁移与行为变化混在同一增量中。

### 2.1 P0 正式状态与关闭路径

> 本小节是 P0 正式状态、关闭路径、处置词表和关闭条件的唯一维护入口；阶段性基线 §2 仅提供审核依据和技术解释。

当前 P0 处置：`CLOSED`（2026-11-26 按路径 A 关闭）。

**2026-11-26 关闭记录（路径 A）**

- CMD-02 阶段 3 PASS：r1–r7 的 C2-1/C2-2/C2-3 absent/C2-4 均 PASS；C2-3 `got_ip` NOT_COVERED，整体 PARTIAL 按授权接受；
- CMD-03 PASS：2026-11-25 独立评审，见 CMD-03 行；
- P0 处置改为 `CLOSED`；CMD-15 仍 `TEST_PENDING`（P1，非路径 A 前置条件），不得隐式带过。

处置值：`OPEN` / `CLOSED` / `CLOSED_WITH_RISK_ACCEPTANCE`。

**路径 A：测试闭环**

1. CMD-02 阶段 3 设备回归完成并 PASS；
2. CMD-03 约定验收范围完成并 PASS：
   - Host T01–T23：23 PASS + T09a EXPECTED，0 failed；
   - 设备侧 200 ms：完成并通过；
   - 设备适用性/可复现性：T01–T12、T14–T18、T20–T23 按 Host 证据验收；设备侧明确标记为不可完整复现，不要求设备实测，也不得计为设备 PASS；这些项目不列入路径 A 的设备补测清单；
   - 路径 A 的设备侧补测要求：设备侧 200 ms 验收通过。T13/T19 已完成 2026-11-25 clean-cycle 设备侧验收，不再列为剩余补测项；
3. 台账记录 P0 处置为 `CLOSED`。

**路径 B：正式风险接受**

1. CMD-02 阶段 3 设备回归完成并 PASS；CMD-02 不在本路径豁免；
2. CMD-03 未覆盖项逐项记录为“范围豁免/风险接受”，包括缺口、影响、责任人、期限和重新开启条件；
3. 风险接受尚未完成审批和记录时，CMD-03 保持 `IN_PROGRESS`；正式批准并完成记录后，CMD-03 标记 `DEFERRED` 并关联风险接受记录；CMD-03 不得标记 `PASS`；
4. 台账记录 P0 处置为 `CLOSED_WITH_RISK_ACCEPTANCE`。

**状态约束**

- 路径 A/B 互斥，不得混用。
- `PASS` 只适用于路径 A 中完成的测试闭环。
- `CLOSED_WITH_RISK_ACCEPTANCE` 是 P0 处置结果，不是 CMD 任务状态，也不是测试 PASS。
- 当前 P0 处置为 `CLOSED`（2026-11-26 路径 A 已记录）。

## 3. 必须保持的实现约束

- 命令名称、v1 envelope、参数、响应字段、错误码、来源白名单及权限行为默认不变；Contract 差异先报告，单独评审和授权。
- 网络事务的 owner、generation、幂等、TTL、cleanup、rollback、restart 顺序和状态迁移是保护契约。
- operation 的 `s_ops_mux` 临界区保持短小；回调在锁外执行；`expiring`、generation 和 running 回收闸不得在普通重构中改变。
- 网络 job 锁 `J`、配置锁 `C`、profile 锁 `P` 的已知偏序为 `J → C → P`（另有 `J → P`）；禁止引入 `C/P → J` 反向边。
- `J`、`C`、`P` 的递归属性是现有代码依赖；不得直接替换为普通 mutex。
- `config_apply_domain_apply()` 必须在配置锁释放后执行；不得在持有 profile 锁时调用会获取 config 锁的函数。
- `apply_async` 持 job 锁跨 NVS 写的持锁预算尚未测量；测量前不缩短锁区。
- 快照改造必须同时检查锁内分配、锁外字段完整性、失败路径、敏感字段生命周期、栈/堆资源和协议兼容；“锁内零 cJSON”不能替代完整响应验收。
- `device.status` 顶层字段与内嵌 network 段来自独立快照时，不承诺同一时刻原子一致；不得在文档中暗示更强保证。

## 4. 必要证据入口

- [CMD 架构与权限边界](../architecture/CMD共享命令架构与权限边界.md)：当前模块职责、生产注册表、来源白名单及认证/授权边界。
- [CMD-02/03 并发静态审查](../reviews/CMD/CMD-02_03_并发静态审查_2026-11-15.md)：字段所有权、operation 并发模型、expiring 窄窗口及历史未决项。历史 §2.4 的 `store_trial` 窄窗口发现保留原文；当前实现已由 `app_cmd_net_trial_store()` 所有权校验及 T01/T02 覆盖，不能再作为当前未决缺陷引用。
- [CMD-04 快照方案](../design/CMD/CMD-04_status快照方案_2026-11-14.md)：04a/04b/04c 的范围和总体约束。
- [CMD-04a operation_status 快照清单](../design/CMD/CMD-04a_operation_status快照清单与判定表_2026-11-14.md)：待实施设计和分支/资源验收要求。
- [CMD-04b network_status 快照清单](../design/CMD/CMD-04b_network_status快照清单与判定表_2026-11-14.md)：待实施设计、skew 和资源验收要求。
- [CMD-04c execute_method 快照记录](../reviews/CMD/CMD-04c_execute_method内联快照清单与判定表_2026-11-14.md)：已实施部分的字段/失败语义、敏感副本擦除及 P0 回归修正证据。
- [CMD-05 config/profile 锁序审查](../reviews/CMD/CMD-05_config_profile锁序审查_2026-11-15.md)：CMD-06 必须遵守的锁序和递归锁约束。
- [P0/P1 阶段性基线（2026-11-24）](../reviews/CMD/P0-P1阶段性基线_2026-11-24.md)：P0 关闭评估、H1-P1 设备覆盖分层及客户端 cleanup 审查输入。
- [MQTT 运行态与 config 同步复核](../reviews/MQTT/MQTT运行态与config同步复核_2026-11-15.md)：CMD-15 R1/R2/R4 及未决项。

## 5. 授权与验证边界

- 静态审查、代码修改、host tests、ESP-IDF 构建、设备/串口/BLE、烧录和 Flash 操作是不同授权范围；不得从“继续推进”推定高风险操作获批。
- 静态审查不等于行为验证；host tests 不等于 ESP-IDF 构建或真机验收；构建通过不等于设备回归通过。
- 每次实施只做一个可验收增量；提交前核对当前 `main`、源码基线、调用者和工作区。
- 只有对应范围的原始证据齐全，才能把任务标为 PASS。执行方报告与仓库可复核日志应明确区分。
- 文档整理不修改源码、Contract、测试代码、设备状态或既有测试结论。
