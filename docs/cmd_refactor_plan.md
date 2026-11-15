# `cmd.c` 优化跟进计划

> 状态基线：2026-11-14。本文是持续维护的执行清单；每项完成后更新状态、证据和对应提交/PR。不得将“代码已移动”直接视为“行为等价已验收”。

## 1. 原则与验收规则

- 保持 ESP-IDF/FreeRTOS/NimBLE/ThingsBoard MQTT 现有技术栈，不引入新框架。
- 小步提交；先锁与并发边界，后模块迁移，再做重复逻辑合并。
- 拆分提交与行为变化提交分开。
- 命令名称、v1 envelope、参数兼容、响应字段、错误码和权限规则默认保持不变。
- 网络事务的 generation、owner、幂等、TTL、cleanup、rollback 和 restart 顺序是保护契约。
- 每次迁移前后对照 diff；完成构建和对应回归后才能标记 PASS。

## 2. 跟进总表

状态：`TODO` 未开始；`IN_PROGRESS` 进行中；`BLOCKED` 受阻；`PASS` 已验收；`DEFERRED` 暂缓。

| ID | 优先级 | 项目 | 状态 | 验收要求 |
|---|---|---|---|---|
| CMD-01 | P0 | `io.get` Wi-Fi runtime 状态读取加锁快照 | IN_PROGRESS | 静态检查通过；待 ESP-IDF 构建与入口回归 |
| CMD-02 | P0 | 全面盘点 `s_net_async` 读写点并统一快照接口 | IN_PROGRESS | 每个字段有明确锁保护/单任务所有权说明；静态盘点见 `CMD-02_03_并发静态审查_2026-11-15.md` §1（7 处读点待收口） |
| CMD-03 | P0 | 复核 operation/job 锁序及 callback 时序 | IN_PROGRESS | operation 临界区不回调 cmd；cleanup/run/cancel 时序有证据；静态结论见同上 §2（锁序 J→O 成立，1 个窄窗口待动态验证） |
| CMD-04 | P1 | network/device status 改为快照后构造 JSON | TODO | 锁持有期间无 cJSON 分配；字段兼容 |
| CMD-05 | P1 | 复核 config lock 与 Wi-Fi profile lock 全局锁序 | TODO | 覆盖 profile 保存、删除、Wi-Fi 事件回写路径 |
| CMD-06 | P1 | 将网络异步事务整体迁移到 `cmd_net_txn.c` | TODO | 事务语义不变；接口不暴露可写全局状态 |
| CMD-07 | P1 | 普通网络命令迁移到 `cmd_net.c` | TODO | scan/profile/reconnect/status 兼容 |
| CMD-08 | P1 | 状态构造函数迁移到 `cmd_status.c` | TODO | device/network/gpio/uart JSON 字段一致 |
| CMD-09 | P2 | GPIO 控制分支抽取共享执行 helper | TODO | 五个旧命令全部保持原行为 |
| CMD-10 | P2 | 配置命令迁移到 `cmd_config.c` | TODO | domain/patch/effect/权限/事务 guard 一致 |
| CMD-11 | P2 | UART/IO/data 命令迁移到 `cmd_device.c` | TODO | 命令协议与错误码兼容 |
| CMD-12 | P2 | 全仓检索 HTTP source/owner、旧 wrapper、测试 hook | TODO | 逐项列调用者；确认后才删除 |
| CMD-13 | P2 | 收敛 `cmd.c` 为入口、policy、分发和统一收尾 | TODO | 无业务状态全局变量泄漏；入口可读且测试通过 |
| CMD-14 | P1 | 记录 `apply_async` 持 job 锁跨 NVS 的持锁预算，评估是否需要缩短锁区 | TODO | 有实测持锁时长数据与并发设计；**未完成前不得缩短锁区** |

## 3. 分阶段实施

### 阶段 A：锁与快照，不移动业务代码

- [x] CMD-01：已修复 `io.get` 对 `s_wifi.sta_has_ip` / `s_wifi.sta_ip` 的读取边界；锁内复制快照，锁外构造 JSON。代码提交：`add93e7`。静态检查通过；构建与 BLE/MQTT 入口回归尚未执行。
- [x] CMD-02：静态盘点完成（`CMD-02_03_并发静态审查_2026-11-15.md` §1）；7 处读点的收口修复待排期。
- [x] CMD-03：静态审查完成（同上 §2）；`expiring` 窄窗口待动态验证。
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
- [ ] 建立代码级并发专项审查证据。
- [ ] 阶段 A 尚未整体完成（CMD-01 已实施；CMD-02/03 静态审查完成、修复待排期；CMD-05 未开始）。
- [x] `cmd.c` 已有 CMD-01 最小代码修改（`add93e7`）；尚未执行 ESP-IDF 编译或真机验收。

---

## 5. 2026-11-15 复核增量（基线 d09f355）

- 附录 A（`cmd.c_review_2026-11-14.md`）补齐 48 个函数的行号锚点与锁清单，CMD-02 的"读写点盘点"已有第一版材料，仍缺逐字段任务上下文标注。
- 新增 CMD-14：`apply_async` 持 `s_net_job_lock` 跨内层 `execute_method`（含 NVS 写），属 R2-P0 的刻意设计；先测持锁时长，再讨论是否缩短。
- 新增待办（未编号，随 CMD-12 一并处理）：`app_cmd_verify_web_password` 改名；`k_config_domains` 与 `app_cmd_domain_known` 合并；`cmd.c:43` 与 `cmd_policy.c:13` 的 owner 映射合并。
- 本文档中的阶段 A/B/C 划分不变；本轮未移动任何代码。

- 2026-11-15：CMD-02/CMD-03 静态审查完成（`CMD-02_03_并发静态审查_2026-11-15.md`）；未改代码，修复项待排期。

- 2026-11-15：窄窗口测试设计已出（`CMD-03_窄窗口竞态_测试设计_2026-11-15.md`）；Phase A（operation 层 host）不涉产品代码，Phase B 需代码授权。
