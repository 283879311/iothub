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
| CMD-02 | P0 | `s_net_async` 逐字段所有权与读点收口 | DESIGN_READY | 7 处读点的最小补丁设计已写入 `CMD-02_03_并发静态审查_2026-11-15.md` §1.3；逐项列明读写者、J 锁快照、行为影响与实施约束；`main/cmd.c` 尚未修改，待用户批准 |
| CMD-03 | P0 | operation/job 锁序、callback 时序及 expiring 窄窗口 | IN_PROGRESS | Phase A host 测试已实施：S1–S5 ×50 ×10 轮全过（A1 100/100、A2 100/100、A3 100/100、A4/A5/A7 50/50），变异体在 A1 失败（可证伪）；见 `CMD-03_窄窗口竞态_测试设计_2026-11-15.md` 附录 A。Phase B（cmd 层 store_trial 守卫）需代码授权；Phase C 需设备授权 |
| CMD-04 | P1 | network/device status 改为快照后构造 JSON | TODO | 锁持有期间无 cJSON 分配；字段兼容 |
| CMD-05 | P1 | 复核 config lock 与 Wi-Fi profile lock 全局锁序 | TODO | 覆盖 profile 保存、删除、Wi-Fi 事件回写路径 |
| CMD-06 | P1 | 将网络异步事务整体迁移到 `cmd_net_txn.c` | TODO | 阶段 A 关闭后再实施；事务语义不变；接口不暴露可写全局状态 |
| CMD-07 | P1 | 普通网络命令迁移到 `cmd_net.c` | TODO | scan/profile/reconnect/status 兼容 |
| CMD-08 | P1 | 状态构造函数迁移到 `cmd_status.c` | TODO | device/network/gpio/uart JSON 字段一致 |
| CMD-09 | P2 | GPIO 控制分支抽取共享执行 helper | TODO | 五个旧命令全部保持原行为 |
| CMD-10 | P2 | 配置命令迁移到 `cmd_config.c` | TODO | domain/patch/effect/权限/事务 guard 一致 |
| CMD-11 | P2 | UART/IO/data 命令迁移到 `cmd_device.c` | TODO | 命令协议与错误码兼容 |
| CMD-12 | P2 | 全仓检索 HTTP source/owner、旧 wrapper、测试 hook 及命名清理 | TODO | 逐项列调用者；确认后才删除；包含三项 owner/domain/password helper 收敛评估 |
| CMD-13 | P2 | 收敛 `cmd.c` 为入口、policy、分发和统一收尾 | TODO | 无业务状态全局变量泄漏；入口可读且测试通过 |
| CMD-14 | P1 | 记录 `apply_async` 持 job 锁跨 NVS 的持锁预算，评估是否需要缩短锁区 | TODO | 先形成测量方法和证据；**未完成评估前不得缩短锁区** |

## 3. 分阶段实施

### 阶段 A：锁与快照，不移动业务代码

- [x] CMD-01：已修复 `io.get` 对 `s_wifi.sta_has_ip` / `s_wifi.sta_ip` 的读取边界；锁内复制快照，锁外构造 JSON。代码提交：`add93e7`。静态检查通过；构建与 BLE/MQTT 入口回归尚未执行。
- [x] CMD-02：静态盘点与最小补丁设计完成（`CMD-02_03_并发静态审查_2026-11-15.md` §1、§1.3）；7 处读点尚未修复，待用户批准后实施。
- [x] CMD-03：锁序/回调静态审查完成（同上 §2）；测试设计完成；**Phase A host 测试已实施并通过**（`tests/operation_host/`，附录 A：A1 100/100、可证伪性检查 OK）。Phase B 未授权。
- [ ] CMD-05：审查 config/profile 锁序。
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
2. **CMD-02：设计已完成，待批准。**7 处读点逐项补丁方案见专项审查 §1.3；下一步需用户明确批准后，才修改 main/cmd.c 并做静态回归。不得顺手重构网络事务。
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
