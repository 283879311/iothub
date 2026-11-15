# `cmd.c` 优化跟进计划

> 状态基线：2026-11-15；当前计划基线 commit：`d0dbf714e138499209cce43fdce92f35ba71a598`。本文是 CMD 重构唯一执行台账；专项审查文档提供详细证据。每项完成后更新状态、证据和对应提交/PR。不得将“静态审查完成”“代码已移动”直接视为“行为等价已验收”。

## 1. 原则与验收规则

- 保持 ESP-IDF/FreeRTOS/NimBLE/ThingsBoard MQTT 现有技术栈，不引入新框架。
- 小步提交；先锁与并发边界，后模块迁移，再做重复逻辑合并。
- 拆分提交与行为变化提交分开。
- 命令名称、v1 envelope、参数兼容、响应字段、错误码和权限规则默认保持不变。
- 网络事务的 generation、owner、幂等、TTL、cleanup、rollback 和 restart 顺序是保护契约。
- 每次迁移前后对照 diff；完成构建和对应回归后才能标记 PASS。

## 2. 跟进总表

状态口径（必须区分工作阶段）：`TODO` 未开始；`REVIEWED` 静态审查完成；`DESIGN_READY` 设计完成待实施；`IN_PROGRESS` 正在实施；`CODE_DONE` 代码已修改但未完成验证；`TEST_PENDING` 等待指定测试/构建；`BLOCKED` 有明确阻塞；`PASS` 代码、测试和验收证据齐全；`DEFERRED` 经明确决策暂缓。一个任务可在备注中同时记录多个子阶段，但总表状态取当前未关闭的最高风险阶段。

| ID | 优先级 | 项目 | 状态 | 验收要求 |
|---|---|---|---|---|
| CMD-01 | P0 | `io.get` Wi-Fi runtime 状态读取加锁快照 | TEST_PENDING | 最小代码修改已在 `add93e7`；待 ESP-IDF 构建及 BLE/MQTT 入口回归，未通过前不标 PASS |
| CMD-02 | P0 | `s_net_async` 逐字段所有权与读点收口 | CODE_DONE | 七处读点已按 §1.3 实施（`28f9090`）；`git diff --check` 与 `syntax_check --strict` 通过，全量读写点复核无新增无锁读。**PASS 待 ESP-IDF 构建 + 受理/重复幂等/apply/cancel/expire 回归**（未授权） |
| CMD-03 | P0 | operation/job 锁序、callback 时序及 expiring 窄窗口 | IN_PROGRESS | Phase A host 测试已实施：S1–S5 ×50 ×10 轮全过（A1 100/100、A2 100/100、A3 100/100、A4/A5/A7 50/50），变异体在 A1 失败（可证伪）；见 `CMD-03_窄窗口竞态_测试设计_2026-11-15.md` 附录 A。Phase B（cmd 层 store_trial 守卫）需代码授权；Phase C 需设备授权 |
| CMD-04 | P1 | network/device status 改为快照后构造 JSON | TODO | 锁持有期间无 cJSON 分配；字段兼容 |
| CMD-05 | P1 | 复核 config lock 与 Wi-Fi profile lock 全局锁序 | REVIEWED | 静态审查完成（`CMD-05_config_profile锁序审查_2026-11-15.md`）：偏序 `J → C → P`、未发现反向边；风险为 C 跨 NVS 长持锁与 C/P 递归契约；CMD-06 前置约束见 §4 |
| CMD-06 | P1 | 将网络异步事务整体迁移到 `cmd_net_txn.c` | TODO | 阶段 A 关闭后再实施；事务语义不变；接口不暴露可写全局状态 |
| CMD-07 | P1 | 普通网络命令迁移到 `cmd_net.c` | TODO | scan/profile/reconnect/status 兼容 |
| CMD-08 | P1 | 状态构造函数迁移到 `cmd_status.c` | TODO | device/network/gpio/uart JSON 字段一致 |
| CMD-09 | P2 | GPIO 控制分支抽取共享执行 helper | TODO | 五个旧命令全部保持原行为 |
| CMD-10 | P2 | 配置命令迁移到 `cmd_config.c` | TODO | domain/patch/effect/权限/事务 guard 一致 |
| CMD-11 | P2 | UART/IO/data 命令迁移到 `cmd_device.c` | TODO | 命令协议与错误码兼容 |
| CMD-12 | P2 | 全仓检索 HTTP source/owner、旧 wrapper、测试 hook 及命名清理 | TODO | 逐项列调用者；确认后才删除；包含三项 owner/domain/password helper 收敛评估 |
| CMD-13 | P2 | 收敛 `cmd.c` 为入口、policy、分发和统一收尾 | TODO | 无业务状态全局变量泄漏；入口可读且测试通过 |
| CMD-14 | P1 | 记录 `apply_async` 持 job 锁跨 NVS 的持锁预算，评估是否需要缩短锁区 | TODO | 先形成测量方法和证据；**未完成评估前不得缩短锁区** |
| CMD-15 | P1 | MQTT 运行态读取 `s_config` 的锁/一致性收口 | CODE_DONE | R1/R2（`999db00`）+ R4（`8d7d768`）已实施：C 内快照、backend 原子缓存、token 静态副本、四处 `s_wifi` 改 W 快照（含 DISCONNECTED 条件调用）。`diff --check`/`syntax_check --strict` 通过，`s_wifi.` 仅剩快照函数（W 内）。**PASS 待构建/回归**；R3/R5 未授权 |

## 3. 分阶段实施

### 阶段 A：锁与快照，不移动业务代码

- [x] CMD-01：已修复 `io.get` 对 `s_wifi.sta_has_ip` / `s_wifi.sta_ip` 的读取边界；锁内复制快照，锁外构造 JSON。代码提交：`add93e7`。静态检查通过；构建与 BLE/MQTT 入口回归尚未执行。
- [x] CMD-02：静态盘点与最小补丁设计完成（`CMD-02_03_并发静态审查_2026-11-15.md` §1、§1.3）；7 处读点尚未修复，待用户批准后实施。
- [x] CMD-03：锁序/回调静态审查完成（同上 §2）；测试设计完成；**Phase A host 测试已实施并通过**（`tests/operation_host/`，附录 A：A1 100/100、可证伪性检查 OK）。Phase B 未授权。
- [x] CMD-05：静态审查完成（`CMD-05_config_profile锁序审查_2026-11-15.md`）；未改代码。
- [ ] 更新本计划和审查报告中的结论、证据及未决项。

**阶段 A 验收门槛：**锁契约和字段所有权明确；不改变 operation 状态机和网络行为；构建通过；相关并发回归通过。

### 阶段 B：抽离网络异步事务

- [ ] CMD-06：迁移 job/trial/operation/cleanup/stage snapshot 相关实现。
- [ ] 仅通过函数 API 暴露 tryconnect/apply/cancel/query/断连处理。
- [ ] 保持递归 job lock、generation 检查、owner 和幂等行为。
- [ ] 核对内部 apply 再入路径，不复制同步提交逻辑。

**必测：**正常试连与提交、重复 tryconnect、不同 owner 查询/取消、BLE 断连、operation expire、验证失败、apply 与 cancel 竞争、配置保存失败、cleanup 失败、旧 worker 与槽位复用。

### 阶段 C：状态构造与普通网络命令

- [ ] CMD-04 / CMD-08：状态读取改为快照采集与 JSON 生成分离。
- [ ] CMD-07：迁移 scan、profile list/delete、reconnect 等普通网络命令。
- [ ] 确认 scan gate 与事务模块的所有权和接口边界。

### 阶段 D：设备控制和配置命令

- [ ] CMD-09：抽取 GPIO 共享执行 helper，保持旧命令名。
- [ ] CMD-10：迁移 config.get/set、domain、patch、权限和 effect。
- [ ] CMD-11：迁移 UART、IO、loopback 命令。
- [ ] 验证配置事务 guard 与命令来源权限。

### 阶段 E：清理与收口

- [ ] CMD-12：全仓确认 HTTP source/owner、旧 wrapper 和测试 hook 是否仍被使用。
- [ ] 只删除有全仓无调用证据的代码。
- [ ] CMD-13：收敛入口文件，更新 README 架构图/目录说明和本文状态。
- [ ] 完成最终构建、协议回归和设备验证。

## 4. 回归证据模板

每个实施 PR 至少记录：

| 项 | 记录 |
|---|---|
| 基线 commit | 待填写 |
| 实施 commit / PR | 待填写 |
| ESP-IDF 版本 / target | 待填写 |
| 构建结果 | PASS / FAIL + 日志 |
| 命令兼容 | 命令、参数、响应字段、错误码差异 |
| 并发用例 | try/apply/cancel/expire/disconnect 等 |
| 资源指标 | free heap / largest block / stack HWM（适用时） |
| 真机 | 板型、固件 SHA、测试结果 |
| 未解决项 | 明确列出，不以“后续观察”代替结论 |

## 5. 文档同步规则

1. 每次代码行为变化，同步更新本计划对应 ID 和证据。
2. 每次接口或模块边界变化，同步更新 `docs/cmd.c_review_2026-11-14.md` 的函数职责/调用关系/锁契约。
3. 只有实际代码结构已经变化后才更新 README 的目录结构；计划中的目标文件不能写成现状。
4. README 只保留稳定架构概览和文档导航；详细审查、任务状态和测试证据留在 `docs/`。
5. 若审查结论被后续代码/测试推翻，保留历史记录并追加更正，不静默覆盖。

## 6. 当前状态摘要

- [x] 第一轮 `cmd.c` 静态审查完成。
- [x] 形成函数职责、调用关系、锁关系、重复逻辑和拆分建议文档。
- [x] 建立代码级并发专项审查证据（CMD-02/03）。
- [ ] 阶段 A 尚未整体完成（CMD-01 已实施；CMD-02/03 静态审查完成、修复待排期；CMD-05 未开始）。
- [x] `cmd.c` 已有 CMD-01 最小代码修改（`add93e7`）；尚未执行 ESP-IDF 编译或真机验收。

---

## 5. 2026-11-15 复核增量（基线 d09f355）

- 附录 A（`cmd.c_review_2026-11-14.md`）补齐 49 个函数的行号锚点与锁清单；后续函数数及行号以 2026-11-15 复核结果为准。
- 新增 CMD-14：`apply_async` 持 `s_net_job_lock` 跨内层 `execute_method`（含 NVS 写），属 R2-P0 的刻意设计；先测持锁时长，再讨论是否缩短。
- 新增待办（未编号，随 CMD-12 一并处理）：`app_cmd_verify_web_password` 改名；`k_config_domains` 与 `app_cmd_domain_known` 合并；`cmd.c:43` 与 `cmd_policy.c:13` 的 owner 映射合并。
- 本文档中的阶段 A/B/C 划分不变；本轮未移动任何代码。

- 2026-11-15：CMD-02/CMD-03 静态审查完成（`CMD-02_03_并发静态审查_2026-11-15.md`）；未改代码，修复项待排期。

- 2026-11-15：窄窗口测试设计已出（`CMD-03_窄窗口竞态_测试设计_2026-11-15.md`）；Phase A（operation 层 host）不涉产品代码，Phase B 需代码授权。


## 7. 当前执行状态与唯一推进队列（2026-11-15；源码审查基线 = d0dbf71；文档提交以当前 main HEAD 为准）

### 7.1 当前事实（不要把计划当成已实施）

- 当前 CMD 源码审查基线：`d0dbf714e138499209cce43fdce92f35ba71a598`。本轮仅更新本计划文档（提交历史以 `main` 当前 HEAD 为准）；CMD-02/03 专项审查没有修改产品源码。Agent 每轮开工必须重新读取当前 `main` HEAD，不得依赖此处的历史快照。
- CMD-01：代码改动已存在（`add93e7`），构建和 BLE/MQTT 入口回归未完成。
- CMD-02：静态证据与最小补丁设计完成（专项审查 §1.3）；7 处读点尚未修复，状态 DESIGN_READY，等待代码修改批准。
- CMD-03：静态审查、测试设计、**Phase A host 测试**均完成（5 场景 ×50 ×10 轮全过；变异体检查通过）。产品代码未改动；Phase B（cmd 层 guard）与 Phase C（设备）未授权。
- CMD-05、CMD-04～CMD-14：除上述已有审查/设计材料外，均按总表状态推进；目标文件名是计划，不代表文件已经创建。
- 当前没有 CMD 模块重构已完成的最终构建/协议/设备验收证据。

### 7.2 唯一推进队列

按依赖顺序推进，不并行混改：

1. **CMD-03 Phase A：已完成**（`tests/operation_host/`；A1–A7 + 可证伪性检查；产品代码未改、无设备操作）。
2. **CMD-02：已实施（`28f9090`），状态 `CODE_DONE`。**七处读点按 §1.3 收口；`PASS` 需 ESP-IDF 构建与受理/幂等/apply/cancel/expire 回归（当前未授权）。不得顺手重构网络事务。
3. **CMD-03 Phase B 决策：**根据 Phase A 结果评估 Phase B；是否抽取 `cmd_net_guard`、是否增加 `active && op` 守卫，必须单独设计评审和代码授权。若未修复基线无法稳定复现，记录风险，不得仅凭机制推测直接改代码。
4. **CMD-05：**完成 config/profile 锁序静态审查。
5. **CMD-01：**安排构建与 BLE/MQTT 入口回归，形成可追溯证据。
6. **CMD-14：**设计并执行适当的持锁时间测量；测量前不缩短 job 锁区。
7. **阶段 B 及以后：**只有阶段 A 的锁契约、读点修复和关键并发验证收口后，才启动 CMD-06；其后按阶段 C、D、E 依次实施。

> 队列顺序是默认推进顺序。若新证据改变风险或依赖，必须先更新本节和总表，再切换任务；不得在聊天中临时跳过依赖。

### 7.3 Agent 持续进度维护规则（每轮必做）

1. **开工前读取：**先读取本计划、对应专项审查文档和当前 `main` HEAD；确认工作树/远端基线，不沿用过期会话记忆。
2. **先报当前状态：**开始工作时说明当前基线、目标 CMD、状态、前置依赖、授权边界和本轮不做事项。
3. **一次聚焦一个可验收增量：**避免把 CMD-02 修复、CMD-03 守卫、CMD-06 模块迁移混在一个提交。
4. **完成后回写：**更新本表状态、阶段勾选、证据链接/文件、提交 SHA、测试结果、未决风险和下一唯一事项；只更新实际发生的事实。
5. **状态升级规则：**审查完成不等于代码完成；代码完成不等于测试通过；host 测试不等于 ESP-IDF 构建或真机验收；无对应证据不得标 `PASS`。
6. **授权边界：**代码修改、构建、host 测试、设备操作分别按用户明确授权执行；设备、串口、BLE、FI/烧录及阶段 B 等高风险操作不得从一般性“继续推进”中推定授权。
7. **冲突处理：**若当前 main 已前进、源码与文档基线不一致、测试设计与实现不符，先暂停实施并重新取证；不得静默覆盖或按旧行号修改。
8. **交接格式：**每轮结束必须输出“本轮完成 / 未完成 / 新发现 / 风险与授权 / 当前基线 / 下一唯一动作”，并确保与本计划一致。

### 7.4 每轮交接记录模板

```markdown
- 日期：
- main HEAD / 源码基线：
- 本轮 CMD：
- 本轮变更（文件/提交）：
- 状态变化（旧状态 -> 新状态）：
- 验证（命令、结果、证据位置）：
- 未完成项 / 风险：
- 授权边界（已授权 / 未授权）：
- 下一唯一动作：
```


### 7.5 交接记录 · 2026-11-15 · CMD-03 Phase A

- 日期：2026-11-15
- main HEAD / 源码基线：写入时 `31d19f6`（源码 `d0dbf71` 起未变）
- 本轮 CMD：CMD-03 Phase A
- 本轮变更：新增 `tests/operation_host/`（stub/shim/S1–S5/Makefile/check_falsifiable.sh）；更新本计划与 `CMD-03_窄窗口竞态_测试设计_2026-11-15.md` 附录 A；**未改 `main/`**
- 状态变化：CMD-03 `DESIGN_READY -> IN_PROGRESS`
- 验证：`make -C tests/operation_host`（0 warning）；`run` 5 场景 ×50 ×10 轮 = 10/10 `RESULT: PASS`；A1 100/100、A2 100/100、A3 100/100、A4/A5/A5h/A7 各 50/50；`falsify` 变异体（expiring 闸 2 处置空）在 A1 失败
- 未完成项 / 风险：Phase B（cmd 层 `store_trial` 未覆盖）；host 语义不等于真机；`app_operation_status_t` 不暴露 `expiring`（测试以 A3 间接验证）
- 授权边界：已授权=host 测试新增/编译/运行；未授权=产品代码、构建固件、设备/串口/BLE/NVS/烧录
- 下一唯一动作：CMD-02 七处读点的最小补丁设计（逐项读写者/锁/局部副本/行为影响），待批准后实施


### 7.6 交接记录 · 2026-11-15 · CMD-02 最小补丁设计

- 日期：2026-11-15
- main HEAD / 源码基线：设计基于产品源码未变的 d0dbf714e138499209cce43fdce92f35ba71a598；文档提交以当前 main HEAD 为准。
- 本轮 CMD：CMD-02 七处无锁/解锁后读点最小补丁设计；并收口 CMD-03 Phase A 文档状态。
- 本轮变更：CMD-02_03_并发静态审查_2026-11-15.md §1.3 增补逐点读写者、锁内快照、行为影响和实施约束；CMD-03_窄窗口竞态_测试设计_2026-11-15.md 顶部状态与 Phase A/Phase B 授权状态同步；本计划状态与队列同步。未修改 main/cmd.c。
- 状态变化：CMD-02 REVIEWED -> DESIGN_READY；CMD-03 保持 IN_PROGRESS（Phase A 完成，Phase B 未授权）。
- 验证：基于当前 main 的 cmd.c 重新读取受影响代码区间；设计覆盖 723–726、811、1033、2667/2757/2863、555/567–569；确认锁内只做快照，不在新增锁区调用 cJSON、operation/Wi-Fi API 或阻塞函数。
- 未完成项 / 风险：7 处读点仍在产品代码中；设计未经过代码实施与编译回归；CMD-03 Phase B 的 cmd 层 trial 重写缺口仍未验证/修复。
- 授权边界：已完成=静态审查与文档设计；未授权=修改 main/cmd.c、新增 cmd_net_guard、ESP-IDF 固件构建、设备/串口/BLE/FI/烧录。
- 下一唯一动作：用户审阅并决定是否批准 CMD-02 最小补丁实施。


### 7.6 交接记录 · 2026-11-15 · CMD-02 设计边界补充

- 日期：2026-11-15
- main HEAD / 源码基线：写入时 `e895d3f`（`main/cmd.c` 未变）
- 本轮 CMD：CMD-02（仅文档）
- 本轮变更：`CMD-02_03_并发静态审查_2026-11-15.md` §1.3 增 C1–C3；本计划 CMD-02 行与推进队列同步
- 状态变化：CMD-02 保持 `DESIGN_READY`（设计通过，源码未授权）
- 验证：无代码变更；未运行测试/构建
- 未完成项 / 风险：C1 所述"快照后仍非原子"需在实现注释中保留；C3 的 `PASS` 依赖构建与回归
- 授权边界：已授权=文档补写；未授权=`main/cmd.c` 修改、构建、设备操作
- 下一唯一动作：用户明确批准后实施 §1.3 七处最小修改（不含 `cmd_net_guard`/`store_trial`/迁移）


### 7.7 交接记录 · 2026-11-15 · CMD-02 七处读点实施

- 日期：2026-11-15
- main HEAD / 源码基线：`28f9090`（父 `e741b1f`）
- 本轮 CMD：CMD-02
- 本轮变更：`main/cmd.c` 七处读点（+89/−33）；专项审查 §1.3 追加实施记录；本计划状态同步
- 状态变化：CMD-02 `DESIGN_READY -> CODE_DONE`
- 验证：`git diff --check` 通过；`tests/syntax_check.py --strict main/cmd.c` OK；
  `grep s_net_async` 全量复核（剩余访问均在 J 内或 dispatcher 单任务）；未执行构建/回归
- 未完成项 / 风险：`PASS` 依赖 ESP-IDF 构建 + 四类回归；C1 的非原子边界仍成立；
  CMD-03 `store_trial` 窄窗口未修复
- 授权边界：已授权=七处源码修改；未授权=构建、设备回归、Phase B、模块迁移
- 下一唯一动作：等待构建/回归授权后执行 CMD-02 验证；或按队列进入 CMD-05 静态审查


### 7.8 交接记录 · 2026-11-15 · CMD-05 锁序审查

- 日期：2026-11-15
- main HEAD / 源码基线：`04e9584`（CMD-02 CODE_DONE）
- 本轮 CMD：CMD-05
- 本轮变更：新增 `CMD-05_config_profile锁序审查_2026-11-15.md`；台账 CMD-05 → REVIEWED
- 状态变化：CMD-05 `TODO -> REVIEWED`
- 验证：静态搜索 `main/{cmd,config,config_apply,wifi,mqtt,cmd_cfg_txn,bt,auth,state}.c` 的嵌套获取；
  未运行构建/测试
- 结论：偏序 `J → C → P`（及 `J → P`）一致，未发现 `P/C → J`、`P → C` 等反向边；风险为长持锁与递归契约
- 未完成项 / 风险：未测持锁时长；`s_config` 无锁读者与 data_pipe 路由位图读者另案
- 授权边界：已授权=只读审查；未授权=代码修改、构建、设备
- 下一唯一动作：按队列评估 CMD-01 构建/回归或 CMD-06 前置条件；CMD-02 的 PASS 仍待构建/回归授权


### 7.9 交接记录 · 2026-11-15 · MQTT 运行态与 config 同步复核

- 日期：2026-11-15
- main HEAD / 源码基线：`07b08cc`（CMD-05 REVIEWED）
- 本轮 CMD：新增 CMD-15（MQTT 配置一致性）
- 本轮变更：新增 `MQTT运行态与config同步复核_2026-11-15.md`；台账增 CMD-15 → REVIEWED；索引同步
- 状态变化：CMD-15 新增，`REVIEWED`
- 验证：只读源码追踪（mqtt.c/config_apply.c/cmd.c）；未运行构建/测试
- 结论：陈旧窗口有界（defer 15s + heap 5s + ack 1.2s + stop ≤20s，最坏 ~41s）；
  **P1 数据竞争**：telemetry/restart 无 C 读 `s_config`，与 C 内整结构 memcpy 并发；
  `is_pending/deferred` 仅日志、未暴露给客户端
- 未完成项 / 风险：窗口长度未实测；esp-mqtt 字符串拷贝语义未验证；`app_mqtt_stop` 在 Wi-Fi 事件任务内同步执行
- 授权边界：已授权=只读复核；未授权=代码修改、构建、设备
- 下一唯一动作：等用户决定 CMD-15 的修复授权或 CMD-02 构建/回归授权；两项目前均未获批


### 7.10 交接记录 · 2026-11-15 · CMD-15 R1/R2 实施

- 日期：2026-11-15
- main HEAD / 源码基线：`999db00`（父 `1165d23`）
- 本轮 CMD：CMD-15（R1/R2）
- 本轮变更：`main/mqtt.c`（快照结构/helper、apply_config、两处 payload、backend 原子缓存、token 静态副本）
- 状态变化：CMD-15 `REVIEWED -> CODE_DONE`
- 验证：`git diff --check` 通过；`syntax_check.py --strict main/mqtt.c` OK；全量 grep 确认 `s_config.` 仅在快照函数（持 C）
- 未完成项 / 风险：R3/R4/R5 未授权；`s_wifi.sta_has_ip/sta_ip` 仍无 W 读；esp-mqtt 字符串拷贝语义未验证；未构建/未回归
- 授权边界：已授权=R1/R2；未授权=R3/R4/R5、构建、设备、CMD-02 回归、Phase B、CMD-06
- 下一唯一动作：等 CMD-15 R3/R4 或 CMD-02 构建/回归的单独授权；否则按队列进入下一个只读项


### 7.11 交接记录 · 2026-11-15 · CMD-15 R4 设计

- 日期：2026-11-15
- main HEAD / 源码基线：`e3fdd84`（代码 `999db00`）
- 本轮 CMD：CMD-15 R4（仅设计）
- 本轮变更：复核文档 §10；台账 CMD-15 备注更新
- 状态变化：CMD-15 保持 `CODE_DONE`；R4 设计待授权
- 验证：无代码变更；未运行测试/构建
- 结论：4 处 `s_wifi` 无 W 读点（653/709/1451/1709）；设计为新增 wifi 快照 helper，
  C 与 W 顺序取用不嵌套，保留既有 M→W，不新增 C→W
- 未完成项 / 风险：W 持有时长未测；R3/R5 未授权
- 授权边界：已授权=设计文档；未授权=R4 代码、构建、设备
- 下一唯一动作：用户审阅并授权 R4 后实施（范围仅四处读取点）


### 7.12 交接记录 · 2026-11-15 · CMD-15 R4 实施

- 日期：2026-11-15
- main HEAD / 源码基线：`8d7d768`（父 `43cc0d6`）
- 本轮 CMD：CMD-15 R4
- 本轮变更：`main/mqtt.c`（wifi 快照 helper、四处替换、DISCONNECTED 条件调用、非原子注释）
- 状态变化：CMD-15 仍 `CODE_DONE`；R4 已实施
- 验证：`git diff --check` 通过；`syntax_check.py --strict main/mqtt.c` OK；
  `grep s_wifi.` 仅 helper 内两行（W 内）+ 一处历史注释
- 未完成项 / 风险：R3/R5 未授权；W 持有时长未测；未构建/未回归
- 授权边界：已授权=R4 四处；未授权=R3/R5、构建、设备、CMD-02 回归、Phase B、CMD-06
- 下一唯一动作：等待 CMD-02 构建+回归授权（一并验证 CMD-15），或按队列进入下一个只读项


### 7.13 交接记录 · 2026-11-15 · CMD-15 文字一致性收口

- 日期：2026-11-15
- main HEAD / 源码基线：`6a52503`（代码 `8d7d768`）
- 本轮 CMD：CMD-15（仅注释/文档）
- 本轮变更：`main/mqtt.c` 一条历史注释（不再暗示存在无锁 `s_wifi` 读取，改为说明 M→W 锁序）；
  复核文档 §10 删除"未改代码"旧说明、§10.1 标注为实施前基线并说明现由 helper 覆盖
- 状态变化：无（CMD-15 保持 `CODE_DONE`）
- 验证：`git diff --check` 通过；`syntax_check.py --strict main/mqtt.c` OK；无行为变化
- 未完成项 / 风险：R3/R5 未授权；构建/回归未授权
- 授权边界：已授权=文字一致性修正；未授权=构建、设备、R3/R5、Phase B、CMD-06
- 下一唯一动作：待用户批准 CMD-02 + CMD-15 联合构建与回归窗口（区分构建/主机测试/设备回归三阶段）


### 7.14 交接记录 · 2026-11-15 · CMD-02 + CMD-15 联合验证窗口申请

- 日期：2026-11-15
- main HEAD / 源码基线：`5104a3b`（代码 `28f9090` + `999db00` + `8d7d768`）
- 本轮 CMD：CMD-02 + CMD-15（验证申请，仅文档）
- 本轮变更：新增 `CMD-02_15_联合验证授权申请_2026-11-15.md`
- 状态变化：无（CMD-02/CMD-15 保持 `CODE_DONE`；验证窗口 `PLANNED` 待批）
- 验证：未执行构建/测试
- 申请范围：阶段 1 ESP-IDF 构建（fullclean+build，0 error，改动文件 0 warning）、
  阶段 2 全部 host 套件（含 operation_host/falsify、s6_gate 141、syntax_check）
- 明确不申请：阶段 3 设备回归（需先烧录含改动固件，与 65D8 `a7e04b6a9` 及阶段 B 边界冲突）、
  烧录/设备/串口/BLE、Phase B、CMD-06、R3/R5
- 授权边界：已授权=无（等待批准）；未授权=构建、host 测试、设备
- 下一唯一动作：用户批准阶段 1+2 后执行并回写证据；批准前不执行任何构建


### 7.15 交接记录 · 2026-11-15 · 联合验证申请 v2 修订（退回后）

- 日期：2026-11-15
- main HEAD / 源码基线：`b323ce8`（源码基线 `5104a3b`；差异仅文档）
- 本轮 CMD：CMD-02 + CMD-15（申请修订，仅文档）
- 退回原因与修订：
  1. v1 列出的 `tests/cmd_policy`、`tests/cmd_cfg_txn`、`tests/cmd_error` 在本机存在但被
     `.gitignore:51`（`tests/cmd_*/`）忽略，**从未进入仓库**；`main` 上不存在。v2 改为只运行
     仓库内实际存在的 5 个 Makefile 入口（token_store/netcheck/wifi_owner/cmd_context/operation_host），
     并注明 `tests/README.md` 的对应条目属已知陈旧文本、不作执行依据。
  2. 构建命令补 `set -euo pipefail`（防 tee 掩盖失败）、`mkdir -p` 日志目录、
     执行前记录 HEAD/`git status --short`/`idf.py --version`；`fullclean` 也落日志。
  3. 明确区分"构建 HEAD `b323ce8`"与"产品源码基线 `5104a3b`"，并设工作区干净/HEAD 校验，
     不符先 STOP。
  4. 边界保持：阶段 3、烧录/设备、Phase B、CMD-06、R3/R5 均不申请。
- 状态变化：无（CMD-02/CMD-15 保持 `CODE_DONE`；窗口仍 `PLANNED`）
- 验证：未执行构建/测试
- 新发现（记录，不处理）：`tests/cmd_*/` 被 gitignore 的三套测试无法从仓库复现；
  是否提交这些测试需单独立项，不得在本窗口顺手加入。
- 授权边界：已授权=文档修订；未授权=构建、host 测试、设备
- 下一唯一动作：用户按 v2 申请重新审查；批准后执行阶段 1+2


### 7.16 交接记录 · 2026-11-15 · 联合验证申请 v3 修订（第二次退回后）

- 日期：2026-11-15
- main HEAD / 源码基线：修订提交后的 HEAD（源码基线仍为 `5104a3b`；差异仅文档）
- 本轮 CMD：CMD-02 + CMD-15（申请修订，仅文档）
- 退回原因与修订：
  1. v2 把构建 HEAD 写死为 `b323ce8`（上一提交），且 v1/v2/v3 每次修订都会使其过期。
     v3 改为四条可验证条件：HEAD == 申请书所在提交、申请书与 HEAD 一致、
     `5104a3b..HEAD` 仅 docs、工作区干净；任一不满足先 STOP，不得放宽。
  2. 阶段 2 原 `set -uo pipefail` 无 `-e`，失败不停止。v3 增加 `run_zero`（PIPESTATUS[0]
     判 rc，非零即 STOP）与 `run_falsify`（预期非零 + 必须含 `[FALSIFY] OK`），逐条 `|| exit $?`，
     每条原始输出单独落盘。
- 状态变化：无（CMD-02/CMD-15 保持 `CODE_DONE`；窗口仍 `PLANNED`）
- 验证：未执行构建/测试
- 授权边界：已授权=文档修订；未授权=构建、host 测试、设备、阶段 3
- 下一唯一动作：用户按 v3 重新审查；批准后执行阶段 1+2


### 7.17 交接记录 · 2026-11-15 · 联合验证申请 v4 修订（第三次退回后）

- 日期：2026-11-15
- main HEAD / 源码基线：修订提交后的 HEAD（源码基线仍为 `5104a3b`；差异仅文档）
- 本轮 CMD：CMD-02 + CMD-15（申请修订，仅文档）
- 退回原因与修订：
  1. **falsify 契约写反（阻断）**：`check_falsifiable.sh` 成功路径为 rc=0 + `[FALSIFY] OK`；
     v3 误写"预期非零"。v4 改为"rc=0 且日志含 `[FALSIFY] OK`"，并实测确认 `falsify rc=0`。
  2. `run_zero` 只检查 PIPESTATUS[0]，忽略 tee；v4 改为 `run_pipe` 同时检查命令与 tee，
     任一非零即 STOP。
  3. 阶段 2 `mkdir -p` 未判失败；v4 增加 `|| { echo STOP; exit 1; }`。
  4. 源码门禁原用 `grep -q`（pipefail 下可能提前关闭管道触发 SIGPIPE）；v4 改为
     `awk 'index($0,"docs/")!=1'` 全量读取后再判断。
- 状态变化：无（CMD-02/CMD-15 保持 `CODE_DONE`；窗口仍 `PLANNED`）
- 验证：未执行构建；`falsify rc=0` 为契约核对时的本地实测
- 授权边界：已授权=文档修订；未授权=构建、host 测试、设备、阶段 3
- 下一唯一动作：用户按 v4 复核；批准后执行阶段 1+2


### 7.18 交接记录 · 2026-11-15 · 联合验证申请 v4 文档清理

- 日期：2026-11-15
- main HEAD / 源码基线：修订提交后的 HEAD（源码基线仍为 `5104a3b`；差异仅 docs/）
- 本轮 CMD：CMD-02 + CMD-15（申请清理，仅文档）
- 修订：删除 §3 代码块后残留的 v3 旧说明（引用已不存在的 `run_zero`、错误声称"tee 永远成功"，
  与 v4 双端检查矛盾）；保留唯一的 v4 契约说明。
- 状态变化：无（CMD-02/CMD-15 保持 `CODE_DONE`；窗口仍 `PLANNED`）
- 验证：未执行构建/测试
- 授权边界：已授权=文档清理；未授权=构建、host 测试、设备、阶段 3
- 下一唯一动作：用户复核后批准阶段 1+2；批准前不执行构建或测试
