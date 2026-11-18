# CMD 当前任务台账（固件 1.3）

> 当前源码基线：`5104a3b0b3266f6afe22404d24ca89d41253cb63`。文档提交可继续前进，不将某个文档 HEAD 固定为源码基线。
>
> 本文只维护 CMD 当前任务状态、未完成事项、关键实现约束和必要验证证据。历史交接、申请修订过程和重复审查叙述不在此累计。当前命令行为以源码及 `contract/v1/` 为准；Contract 仍为 `1.1-draft`。

## 1. 当前任务状态

状态含义：`TODO` 未开始；`REVIEWED` 静态审查完成；`IN_PROGRESS` 有未关闭的实施/验证工作；`TEST_PENDING` 代码已改但指定验证未完成；`PASS` 必须有对应代码、测试和验收证据；`DEFERRED` 经明确决策暂缓。

| ID | 优先级 | 当前事项 | 状态 | 当前结论 / 关闭条件 |
|---|---|---|---|---|
| CMD-01 | P0 | `io.get` Wi-Fi runtime 读取快照 | TEST_PENDING | 最小改动已实施（`add93e7`）；仍缺构建及 BLE/MQTT 入口回归 |
| CMD-02 | P0 | `s_net_async` 字段所有权与读点 | TEST_PENDING | 七处读点已实施（`28f9090`）；联合构建和 host tests 按执行报告通过；阶段 3 设备回归未执行、未授权 |
| CMD-03 | P0 | operation 锁序、回调时序及 expiring 窄窗口 | IN_PROGRESS | Phase A host 测试通过；worker 在 cleanup 窗口可能重新写入 trial 的风险仍待专门动态验证/设计决策；Phase B 代码修改和设备测试均未授权 |
| CMD-04 | P1 | status 响应快照化 | TODO | 04c 内联快照已实施并有代码审查/本地验证记录；04a operation_status 与 04b network_status 快照仍待实施和验收 |
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
| CMD-15 | P1 | MQTT 运行态读取 `s_config` 的一致性 | TEST_PENDING | R1/R2（`999db00`）及 R4（`8d7d768`）已实施；联合构建和 host tests 按执行报告通过；阶段 3 设备回归未执行、未授权；R3/R5 未授权 |

## 2. 当前未关闭事项与推进依赖

1. **CMD-02 / CMD-15**：阶段 1 ESP-IDF fullclean/build 和阶段 2 host tests 于 2026-11-16 按执行报告通过；阶段 3 设备回归没有执行，也没有授权。执行方报告及本地忽略目录中的证据未上传仓库，因此不能标记 PASS。
2. **CMD-01**：构建和 BLE/MQTT 入口回归仍缺；不能用 CMD-02/15 的联合 host 结果替代。
3. **CMD-03**：Phase A host 测试已通过，变异体检查可证伪；仍需决定是否设计并实施 cmd 层 `store_trial` 守卫。不得直接修改 `expiring` 状态语义。Phase B 和设备验证须单独授权。
4. **CMD-04**：04a/04b 设计材料仍是待实施方案；04c 已实施的部分不代表整个 CMD-04 关闭。04c 中同步 tryconnect 分支已确认不属于当前 v1 wire 可达路径，不得把它写成 wire 真机验收 PASS；apply dispatcher 的 trial patch 路径仍需按 async apply 全流程理解。
5. **CMD-05 / CMD-14**：锁序静态审查完成，但长持锁尚未实测。CMD-14 测量前不得因“看起来持锁过久”直接缩短临界区。
6. **CMD-06 及后续模块迁移**：须等待相关锁契约、所有权边界和必要验证收口，再单独评审实施；不得把文件迁移与行为变化混在同一增量中。

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
- [CMD-02/03 并发静态审查](CMD-02_03_并发静态审查_2026-11-15.md)：字段所有权、operation 并发模型、expiring 窄窗口及未决项。
- [CMD-04 快照方案](CMD-04_status快照方案_2026-11-14.md)：04a/04b/04c 的范围和总体约束。
- [CMD-04a operation_status 快照清单](CMD-04a_operation_status快照清单与判定表_2026-11-14.md)：待实施设计和分支/资源验收要求。
- [CMD-04b network_status 快照清单](CMD-04b_network_status快照清单与判定表_2026-11-14.md)：待实施设计、skew 和资源验收要求。
- [CMD-04c execute_method 快照记录](CMD-04c_execute_method内联快照清单与判定表_2026-11-14.md)：已实施部分的字段/失败语义、敏感副本擦除及 P0 回归修正证据。
- [CMD-05 config/profile 锁序审查](CMD-05_config_profile锁序审查_2026-11-15.md)：CMD-06 必须遵守的锁序和递归锁约束。
- [MQTT 运行态与 config 同步复核](MQTT运行态与config同步复核_2026-11-15.md)：CMD-15 R1/R2/R4 及未决项。

## 5. 授权与验证边界

- 静态审查、代码修改、host tests、ESP-IDF 构建、设备/串口/BLE、烧录和 Flash 操作是不同授权范围；不得从“继续推进”推定高风险操作获批。
- 静态审查不等于行为验证；host tests 不等于 ESP-IDF 构建或真机验收；构建通过不等于设备回归通过。
- 每次实施只做一个可验收增量；提交前核对当前 `main`、源码基线、调用者和工作区。
- 只有对应范围的原始证据齐全，才能把任务标为 PASS。执行方报告与仓库可复核日志应明确区分。
- 文档整理不修改源码、Contract、测试代码、设备状态或既有测试结论。
