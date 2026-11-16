# iothub 文档体系重构方案（Firmware 1.3）定稿

Nov 16, 2026 · @Someone  
**修订状态**：设计签收通过（五项终审全部通过；设计签收 ≠ P0 执行授权）

## 1. 本版说明

本稿为 **Firmware 1.3 文档体系重构方案 · 设计签收通过版**。五项终审重点（动作词表与布尔一致性、BLOCKED 须非空 attempted、patch_manifest 可机检、DOC-G08 事前审批、APR 第 7 项含 `execution.source_commit`）均已通过。

**授权边界（不变）**：设计签收 **不等于** P0 启动授权，也 **不等于** 仓库修改、Git 写操作或设备操作授权。P0 须按第 14、16 章另行申请并获批后才可开始；在此之前不得修改仓库、不得执行写 Git、不得进行设备操作。

**签收标准**：每个规则有对应字段；每个条件必填有明确适用范围；每种状态组合有唯一判定结果；每项授权例外有明确数据表达；每项自动校验有可执行输入、输出与失败条件。

| 编号 | 本版新增修订 | 落点 |
| --- | --- | --- |
| R1–R41 | 此前各轮规则闭环 | 见对应章节 |
| R42 | APR 拆分「当前状态」与「执行时点有效性」；第 12 节同步修正 | 第 5.7、12 节 |
| R43 | TEST 显式引用 `target`（Firmware/Host Target ID）；integration 按组合必填 | 第 5.5、7.3 节 |
| R44 | TEST.scope 拆为 `firmware` 与 `execution` 两个独立字段 | 第 5.5、7.4、7.5 节 |
| R45 | `acceptance_waiver` 统一为「无 required TEST 覆盖时必填」 | 第 5.3、6、12 节 |
| R46 | `blocked_no_authorization` 强制 `attempted_actions` 非空 + 门禁证据 | 第 5.6、7.2 节 |
| R47 | DOC-G07 区分不可变原始证据与可维护索引/映射 | 第 9.1、15 节 |
| R48 | RUN 跨越 `withdrawn_at` 整次无效；`build_inputs_manifest.completeness`；execution_paths 优先级；补丁清单统一结构；G06 扫描覆盖；16.4 表述 | 第 5.6、5.7、9、15、16.4 节 |
| R49 | `authorization=APR-<id>` 仅表示引用记录存在；授权有效性按执行时点判定，不得仅凭当前 status | 第 5.6、5.7 节 |
| R50 | `scope.execution_paths` 最小匹配语义：目录递归、约定 glob、删除/重命名双路径、符号链接解析目标、大小写按规范化规则 | 第 5.7 节 |
| R51 | 涉及设备且执行环境影响结果的 integration：必须同时登记 Firmware Target 与 Host Target | 第 5.5、7.3 节 |
| R52 | 门禁拦截证据最低结构 | 第 5.6 节 |
| R53 | 增加 `execution_inputs_manifest`，与 build 输入快照对称；completeness→有效性映射 | 第 5.6、7.5 节 |
| R54 | Target ID 一经 RUN 引用即不可变身份字段；变更须新建 ID | 第 5.5、7.3 节 |
| R55 | 统一 Integration：涉及设备 ⇒ Host+Firmware+device.id（删除条件性限定） | 第 5.5、7.3 节 |
| R56 | 受控动作词表；`gate_evidence` 改为逐动作列表 | 第 5.5、5.6、5.7 节 |
| R57 | `completeness` 判定规则：possible/dynamic 非空不得 CLEAN；INCOMPLETE/UNKNOWN 均不可 VALID | 第 5.6、7.5 节 |
| R58 | 统一路径匹配规范（glob、**、符号链接、规范化）适用于全部 scope/补丁/基线路径 | 第 5.7、7.4 节 |
| R59 | 阶段批准路径清单 Schema（DOC-G08 输入） | 第 14、15 节 |
| R60 | R100、waiver、INDEX 计数、devices、回滚演练 | 对应章节 |
| R61 | 执行快照纳入 `execution_inputs_manifest`；completeness→VALID/INVALID/UNKNOWN 映射表 | 第 5.6、7.5、7.6 节 |
| R62 | RUN 创建时固化 `target_ids`；历史分组以 RUN 记录为准 | 第 5.6、7.3 节 |
| R63 | 重命名记录含 `old_path`/`new_path`/`old_sha256`/`new_sha256`；删除允许 content 空、须有 baseline | 第 5.6、5.7 节 |
| R64 | `gate_evidence` 仅记录被拦截动作；与 attempted_actions 的对应关系 | 第 5.6 节 |
| R65 | DOC-G08 清单：审批主体、时间顺序、基线锁定、排除优先、事前批准 | 第 14、15 节 |
| R66 | §12 waiver 与 deferred/rejected 对齐 | 第 12 节 |
| R67 | 路径规范化确定性（NFC、大小写、冲突报错、glob 白名单） | 第 5.7 节 |
| R68 | 回滚演练最终要求：至少一次独立副本演练 | 第 14.5、16.4 节 |
| R69 | 动作词表与 `authorization_required`/`device_required` 强制一致性 | 第 5.5、12 节 |
| R70 | 所有 `result=BLOCKED` 均要求 `attempted_actions` 非空 | 第 5.6、7.1、7.2 节 |
| R71 | deleted 使用 `path` 表示原路径；补丁字段语义表 | 第 5.6 节 |
| R72 | DOC-G08 增加 `phase_started_at`/`phase_started_by`；事前批准可机检 | 第 14、15 节 |
| R73 | APR 七项条件显式编号；`execution.source_commit` 纳入 baseline 检查 | 第 5.7 节 |
| R74 | `discovery.method` 须有覆盖能力说明与完整性证明；无法证明不得 CLEAN | 第 5.6、7.5 节 |
| R75 | 路径大小写规则写死（非建议）；本地与仓库冲突报错 | 第 5.7 节 |
| R76 | glob 边界：`**/` 零级、`a/**/b` 匹配 `a/b`、固定语义 | 第 5.7 节 |
| R77 | gate 证据：gate_id 唯一、timestamp 在 RUN 区间、哈希算法固定 | 第 5.6 节 |
| R78 | P0「独立分析克隆」命名 | 第 14.2 节 |
| R79 | 动作词表校验扩展到 required/attempted/executed；`is_device_action⇒requires_apr` | 第 5.5、5.6 节 |
| R80 | APR 绑定 `firmware_target_ids`；可选 `execution_baseline` | 第 5.7 节 |
| R81 | 统一时间：RFC3339 UTC；区间端点与 null 规则 | 第 5.6、5.7、12、15 节 |
| R82 | G08：`approved_by`；`ignored_baseline_manifest` Schema | 第 14、15 节 |
| R83 | inputs manifest 可执行结构 + 唯一序列化/哈希算法 | 第 5.6 节 |
| R84 | 路径**大小写敏感**；外部依赖登记结构 | 第 5.7 节 |
| R85 | 纯设备测试：无 Host Target 时 environment 独立规则 | 第 5.5、5.6、7.3 节 |
| R86 | 无固件产物：`build_inputs_manifest=NOT_APPLICABLE`；缺 execution 清单→INVALID | 第 5.6、7.5 节 |
| R87 | G07 路径分类清单：immutable vs maintainable | 第 9、15 节 |
| R88 | 正式验收：post-generate 后经授权提交再 diff | 第 12 节 |
| R89 | G09 固定题库版本、提示词、输入、模型、判分 | 第 15 节 |
| R90 | `gate_id` 明确为 unique_within_run | 第 5.6 节 |

此前已纳入的修订与决策：

| 编号 | 修订内容 | 落点 |
| --- | --- | --- |
| 1 | 引用关系单向权威，反向关系由脚本推导 | 第 5 章 |
| 2 | 任务状态删除 `verified`，最高到 `code_done` | 第 5、6 章 |
| 3 | 测试状态拆为“运行结果”和“有效性”两个维度 | 第 7 章 |
| 4 | RUN 增加构建变体、构建开关、dirty、补丁哈希、设备实际固件身份 | 第 7 章 |
| 5 | 真机测试 scope 默认为固件全集，缩小必须说明理由 | 第 7 章 |
| 6 | 引入 `APR-*` 授权记录，RUN 引用可核验的授权 | 第 5 章 |
| 7 | 历史编号不重新编号，用 `legacy_ids` 建立映射 | 第 5 章 |
| 8 | 敏感信息扫描扩大到整个仓库及全部 Git 历史，明确处置顺序 | 第 9 章 |
| 9 | 文档增加 `reviewed_against` 与适用固件版本 | 第 10 章 |
| 决策 A | 垂直切片拆成 Wi-Fi 事务、CMD-15（归 MQTT）两组，另加 Token Store 主机测试链路 | 第 13 章 |
| 决策 B | 新增 IO 模块规范（继电器、LED、触摸输入、`state.c`） | 第 4 章 |

四份设计附件合并在本文中：**附件 A** 字段定义表（第 5 章）、**附件 B** 测试状态组合表（第 7 章）、**附件 C** INDEX 布局草图（第 11 章）、**附件 D** P0 执行与验收清单（第 14 章）。

## 2. 目标与基本原则

目标是建立一条可验证的链路：`需求 REQ → 开发任务 TASK → 测试定义 TEST → 测试运行 RUN → 文档索引 INDEX`，每层只维护自己的事实，不通过复制状态制造一致性。重构的重点不是减少文件数，而是让当前有效信息可以被人和 AI 一眼找到，并且能被自动检查。

**当前要解决的问题**

- 文档按时间和阶段散落，难以判断哪篇现行有效。
- 同一事实在 README、规范、专题文档中多处维护，容易漂移（例如 Contract 记录的基线与 README 基线不一致）。
- 需求、任务、测试和测试结果之间没有统一关联，历史 PASS 容易被误当作当前结论。
- 证据分散在多个目录，引用关系不清，且可能含有 NVS 备份、Wi-Fi 密码、MQTT Token 等敏感信息。

**五条基本原则**

| 原则 | 要求 |
| --- | --- |
| A 单一事实源 | 同一事实只有一个权威位置，其他文档只引用、解释或展示它；Markdown 视图不反向成为数据源 |
| B 实现与验证分离 | 以下八件事互相独立，不得相互推导：代码已实现、开发任务完成、主机测试通过、设备测试通过、证据完整、当前固件仍然有效、产品验收通过、设备操作已获授权 |
| C 历史不可篡改 | 审批、授权、事故和测试运行记录保留原文；允许增加摘要、索引和引用，不改写事实 |
| D 安全优先 | 发现真实凭据暴露时，先处置暴露风险，再继续整理文档，不得继续复制敏感文件 |
| E 不改产品行为 | 重构不修改产品源码、协议语义、固件行为和设备状态 |

**各类内容的唯一事实源**

| 内容 | 唯一事实源 |
| --- | --- |
| 当前产品能力 | Spec，并以源码核实 |
| 命令、参数、错误码、状态机 | `contract/` |
| 产品需求 | `docs/requirements.yaml` |
| 开发任务状态 | `docs/status/tasks.yaml` |
| 测试定义 | `docs/status/tests.yaml` |
| 测试运行结果 | `evidence/runs/` 下的 RUN 记录 |
| 授权记录 | `docs/history/approvals/` 中带元数据的 APR 文档 |
| 当前问题 | `docs/status/known-issues.yaml` |
| 设计决策 | `docs/adr/` |
| 文档导航与状态展示 | 自动生成的 `docs/INDEX.md` |

## 3. 版本管理规则

文档体系与固件统一使用 **Firmware 1.3** 这一个版本标识，不再另设文档版本号；但“版本系列”不能代替“具体构建”，每次测试必须分开记录下面五种身份。

| 身份 | 含义 | 来源 |
| --- | --- | --- |
| 版本系列 | 如 `1.3`，只表示文档适用的固件系列 | 文档元数据 `applies_to_firmware` |
| 源码提交 | 测试所依据的完整 Git SHA | RUN `source.commit` |
| 构建信息 | 构建提交、是否 dirty、构建开关、补丁哈希 | RUN `build.*` |
| 固件文件哈希 | 实际测试所用固件文件的 SHA256 | RUN `firmware.sha256` |
| 设备实际身份 | 设备 `device.info` 返回的 `firmware`（形如 `1.3-<短SHA>[-dirty]`）与 `build_sha` | RUN `device.reported.*` |

规则：

- 设备实际身份必须与预期构建核对一致；无法核实时只能标 `UNKNOWN`，不能猜测设备运行的是哪个版本。
- 同一版本系列下存在不同源码提交时，分别记录，不得仅凭版本号认定相同。
- 历史文档保留原有版本标识，不因本次重构改标为 1.3；跨版本引用必须写明原始适用版本。
- 固件升到下一个系列（如 1.4）时，每份现行规范重新核对后更新 `applies_to_firmware`，校验器报告不一致项。
- 文档内容修订通过 Git 提交、文件变更记录和修订日期追踪。

## 4. 目录结构与模块规范清单

文档树按“文档类型”组织，规范按“功能模块”组织；目录是目标形态，P0 盘点后再定最终路径，不要求一次建齐。`contract/` 保持原位和权威地位。

```text
iothub/
├── README.md                  当前真实能力、限制、构建、入口
├── AGENTS.md                  30~40 行，仓库协作与阅读规则
├── contract/v1/               命令、错误码、状态机（协议事实源，不移动）
├── docs/
│   ├── INDEX.md               自动生成状态区 + 导航
│   ├── spec/
│   │   ├── architecture.md  lifecycle.md  resource-management.md  protocol.md
│   │   └── modules/           ble wifi mqtt cmd config data-pipe ota uart io diagnostics security
│   ├── requirements.yaml
│   ├── status/                tasks.yaml  tests.yaml  known-issues.yaml
│   ├── adr/                   一个决策一个文件
│   ├── runbooks/              build flash ble-validation factory-reset recovery
│   ├── history/               reviews approvals incidents development test-reports
│   └── schemas/               各 YAML 的 JSON Schema
├── evidence/
│   ├── runs/  reviews/  incidents/  releases/
│   └── manifests/             migration-map.yaml  evidence-index.yaml
└── tools/docs/                generate_index.py  validate_docs.py  scan_sensitive.py
```

**模块规范共 11 份**，每个模块一份主规范；Architecture、Lifecycle、Resource Management、Protocol 是跨模块规范，不计入模块。

| 模块 | 范围 | 主要源码 |
| --- | --- | --- |
| BLE | NimBLE 外设、NUS 的 CMD/DATA 特征、会话与限频 | `bt*.c`、`ble_cmd.c`、`transport_ble.c` |
| Wi-Fi | STA、扫描、profile、换网事务、联网验证 | `wifi.c`、`wifi_op_owner.c`、`netcheck*.c` |
| MQTT | ThingsBoard 连接、遥测、属性、RPC、数据面主题 | `mqtt.c` |
| CMD | v1 命令分发、来源上下文、策略、错误码 | `cmd*.c` |
| Config | 配置存储、读写、domain、事务 guard | `config.c`、`config_apply.c`、`cmd_cfg_txn.c` |
| Data Pipe | 三端口路由、有界队列、丢帧统计 | `data_pipe.c` |
| OTA | 分区、启动确认、回滚（传输未实现，见第 8 章） | `ota.c` |
| UART | UART1 工业数据口、参数与运行态 | `uart.c` |
| IO | 继电器、LED、触摸输入、上电策略及相关命令 | `state.c`、`bootstrap.c` 中的输出控制 |
| Diagnostics | 堆水位、内存分级、诊断输出 | `diag.c`、`mem_guard.c` |
| Security | 鉴权、token、配网 PIN、权限边界、凭据存储 | `auth*.c`、`prov_pin.c` |

`operation.c`、`netcheck.c` 是跨模块的事务基础设施，归属在 P0 模块映射后决定，不提前划分。

**每份模块规范的约束**

- 统一章节：功能定位、对外接口、内部状态、生命周期、并发关系、资源约束、错误处理、安全约束、与其他模块的交互、当前实现状态、已知限制、对应需求与测试。模板可裁剪，空节写“不适用”，不得为凑模板虚构内容。
- 原则上不超过 400 行；超过时先判断是否存在独立生命周期、接口边界或状态机，再决定是否拆分，不为凑行数机械拆分。
- 必须区分“当前已实现”“当前未实现”“已知限制”“计划能力”，不能把设计目标写成实现事实。
- Web、SoftAP、Classic SPP 在 Spec 中只作为已废弃功能记录，并链接对应 ADR。

## 5. 附件 A：最终字段定义表

七类记录共同遵守两条规则：**引用只写一个方向**，反向关系由脚本推导；**状态只在一处维护**，展示状态由脚本计算。YAML 是结构化数据的唯一来源，Markdown 只是视图，生成区域禁止手工修改。

### 5.1 引用方向（权威方向）

| 关系 | 权威字段（只在这里写） | 反向关系 |
| --- | --- | --- |
| 任务实现哪些需求 | `TASK.requirements` | 需求的任务清单由脚本推导 |
| 测试验证哪些需求 | `TEST.covers` | 需求的验收测试清单由脚本推导 |
| 运行属于哪个测试 | `RUN.test_id`（在 `evidence/runs/*/metadata.yaml`） | 测试的运行列表由脚本推导 |
| 运行依据哪次授权 | `RUN.authorization` | 授权下的运行由脚本推导 |
| 问题关联哪些需求和任务 | `ISSUE.related_requirements`、`ISSUE.related_tasks` | 需求和任务的相关问题由脚本推导 |
| 需求对应哪些规范与命令 | `REQ.spec`、`REQ.contract` | 规范和命令的关联需求由脚本推导 |

因此 `REQ` 里不再写 `tasks` 和 `acceptance_tests`，`TEST` 里不再写 `runs`。

### 5.2 ID 规则

| 类型 | 格式 | 示例 |
| --- | --- | --- |
| 需求 | `REQ-<模块>-<三位序号>` | `REQ-WIFI-001` |
| 任务 | `TASK-<模块>-<三位序号>` | `TASK-MQTT-005` |
| 测试 | `TEST-<模块>-<三位序号>` | `TEST-SEC-001` |
| 运行 | `RUN-<模块>-<测试序号>-<三位运行序号>` | `RUN-WIFI-015-001` |
| 授权 | `APR-<年>-<三位序号>` | `APR-2026-012` |
| 问题 | `ISSUE-<模块>-<三位序号>` | `ISSUE-SEC-001` |

模块代码：`BLE WIFI MQTT CMD CFG DATA OTA UART IO DIAG SEC`。ID 全局唯一、一经分配不得复用或改号。

### 5.3 REQ：`docs/requirements.yaml`

| 字段 | 必填 | 说明与校验 |
| --- | --- | --- |
| `id` `module` `title` `priority` | 是 | priority 取 `P0 P1 P2`；module 必须在模块代码内 |
| `status` | 是 | **只允许** `planned / deferred / rejected`，表示需求意图；`implemented / verified` 是派生展示状态，不得写入 |
| `description` | 是 | 要求与验收条件的文字描述 |
| `spec` | 是（deferred/rejected 除外） | 规范文件路径，必须存在 |
| `contract` | 否 | 命令名列表，必须存在于 `contract/` |
| `acceptance_waiver` | 条件 | **`status=planned` 且无任何 `kind: required` 的 TEST 覆盖时必填**（R45/R60）。`supplementary` 不构成验收覆盖。`deferred`/`rejected` 不要求 waiver（spec 已允许不填）。若已有 required TEST 覆盖，**禁止**再填 waiver。有 waiver 时最高 `implemented`，不得 `verified` |
| `legacy_ids` | 否 | 历史编号别名 |

### 5.4 TASK：`docs/status/tasks.yaml`

| 字段 | 必填 | 说明与校验 |
| --- | --- | --- |
| `id` `module` `title` | 是 |  |
| `requirements` | 是 | 至少一个 REQ，必须存在 |
| `state` | 是 | `todo / in_progress / partial / code_done / blocked / deferred / cancelled`，**没有 `verified`** |
| `completed` `pending` | 是 | 已完成和未完成子项；`code_done` 要求 `pending` 为空 |
| `blocked_by` | 条件 | `blocked` 时必填，可引用状态为 `pending` 的 APR（授权申请待批）或 ISSUE，并写明阻塞原因 |
| `source_commit` | 条件 | `code_done` 时必填，且必须是仓库中存在的提交 |
| `legacy_ids` | 否 | 例如 `CMD-15` |

`code_done` 只表示开发完成，不代表测试通过、设备通过、验收通过或已授权。

### 5.5 TEST：`docs/status/tests.yaml`

| 字段 | 必填 | 说明与校验 |
| --- | --- | --- |
| `id` `module` `title` | 是 |  |
| `covers` | 是 | 至少一个 REQ |
| `level` | 是 | `static / host / build / integration / device / field` |
| `kind` | 是 | `required`（计入需求验证）或 `supplementary`（仅供参考） |
| `required_variant` | 条件 | build/device/field：必填。**host/static 允许 `not_applicable`**。integration 若涉及固件产物则必填，否则可 `not_applicable`（R30/R43） |
| `target` | 是 | 显式引用 `target_builds` 中的身份 ID（R43/R51）。条件：static/host → 必填 `target.host`；build → 必填 `target.firmware`；device/field → 必填 `target.firmware`（不额外要求 Host Target）；integration → 见下。引用必须存在且 ID 全局唯一 |

**integration / 设备等级的 target 条件（R51/R55，全文唯一口径）**：

| 场景 | 必需 target |
| --- | --- |
| 不涉及设备、无固件产物 | Host Target |
| 不涉及设备、有固件产物 | Host Target + Firmware Target |
| **涉及设备的 integration** | **Host Target + Firmware Target + device.id**（不得省略 Host） |
| 纯设备操作（`level=device\|field`） | Firmware Target + device.id |

**Target 不可变（R54）**：Target ID 一经任意 RUN 引用，其身份字段（commit、flags_hash、toolchain、inputs_hash、firmware_sha256、environment_hash 等）**不得修改**。身份变化必须创建新 Target ID；已有条目仅允许追加非身份性说明。`target_builds` 须有 Schema：ID 格式、字段类型、唯一性、身份完整性。
| `scope.firmware` | 条件 | 有固件相关判定时必填；路径/通配符列表。device/field/integration（有固件）默认 `firmware_all`（第 7.4 节）。缩小须写 `scope_reason`（R44） |
| `scope.execution` | 是 | 测试脚本、Runbook、夹具及执行依赖路径；procedure 引用及可静态识别传递依赖自动并入（R44） |
| `procedure` | 是 | runbook 路径、脚本路径或命令；其引用自动并入 `scope.execution` |
| `execution_requirements` | 是 | `authorization_required`、`device_required`。**等级组合硬约束（R33/R37）**见下表；违反即为 Schema 校验错误。`device_required=true` 时 `required_actions` 不得为空 |
| `required_actions` | 条件 | `authorization_required` 或 `device_required` 为真时必填且非空；动作 ID 取自**受控动作词表**（R56） |
| `legacy_ids` | 否 |  |

**受控动作词表（R56/R69/R79）**：`docs/status/action_catalog.yaml`。每项：`action_id`、类别、`is_device_action`、`requires_apr`、执行证据要求。**硬约束：`is_device_action=true` ⇒ `requires_apr=true`**（无“无需 APR 的设备动作”例外，除非另行立项设计）。动作目录是唯一事实源。

**required_actions 与布尔字段映射表（R69，机器校验）**：

| 检查字段 | 必须满足 |
| --- | --- |
| `device_required=true` | `required_actions` **至少包含一个** `is_device_action=true` 的动作 |
| `device_required=false` | `required_actions` **不得包含** `is_device_action=true` 的动作 |
| `authorization_required=true` | `required_actions` **至少包含一个** `requires_apr=true` 的动作 |
| `authorization_required=false` | `required_actions` **不得包含** `requires_apr=true` 的动作 |

`authorization_required=true` 且 `device_required=false` 表示非设备但需 APR 的受控动作（如主机侧敏感操作），不是设备动作例外。

**三集合校验（R79，不得与 R69 存在性映射混用）**：

| 集合 | 校验规则 |
| --- | --- |
| `required_actions` | **完整执行**上表（R69）：按两个布尔字段检查类别存在性与禁止项 |
| `attempted_actions` | 全部 action_id ∈ 动作目录；不得要求 TEST 禁止的类别（`device_required=false` 不得含设备动作；`authorization_required=false` 不得含 `requires_apr=true`）。**允许空集合**；但 `result=BLOCKED` 时必须非空（R70） |
| `executed_actions` | 全部 action_id ∈ 动作目录；不得包含未实际执行的动作；其中的设备/APR 动作必须有有效授权。**允许空集合** |

**优先规则**：`authorization=blocked_no_authorization` 时，`executed_actions=[]` 为**强制条件**，**不得**因 `device_required=true` 或 `authorization_required=true` 而要求 executed 非空或判违反动作映射。该 RUN 仍须 `attempted_actions` 非空、`gate_evidence` 可核验，且不得作为 PASS/verified 或有效设备证据。

**测试等级 × device_required × authorization_required（R37）**：

| 测试等级 | device_required | authorization_required | 规则 |
| --- | --- | --- | --- |
| `static` | 必须 `false` | `false` 或 `true` | 不得出现设备动作（`executed_actions` 中不得含设备操作） |
| `host` | 必须 `false` | `false` 或 `true` | 同上 |
| `build` | 必须 `false` | `false` 或 `true` | 同上 |
| `device` | 必须 `true` | 必须 `true` | 必须 APR；必须有设备身份 |
| `field` | 必须 `true` | 必须 `true` | 必须 APR；必须有设备身份 |
| `integration` | 按实际 | 涉及设备时必须 `true` | `device_required=true` ⇒ 必须 APR；`device_required=false` ⇒ 不得出现设备动作 |

`device_required=true` 且 `authorization_required=false` 一律非法。`level=device|field` 且任一为 false 一律非法。

### 5.6 RUN：`evidence/runs/<RUN-ID>/metadata.yaml`

运行记录只追加、不覆盖；重测创建新的 RUN ID；失败记录必须保留。

| 字段组 | 必填内容 |
| --- | --- |
| `run` | `id`、`test_id`、`result`：只允许 `PASS / FAIL / BLOCKED / ABORTED`。**没有 `NOT_RUN`**：测试没有任何 RUN 时由脚本派生为未运行。未授权尝试处理见 R21/R26 |
| `source` | `commit`：测试运行时对照的源码基线（判定“测试对照什么”） |
| `build` | **条件必填（R30）**：build/device/field 必填；host/static 若无固件产物则省略整组。含 `commit`、`dirty`、`variant`、`flags`、`idf_version`、`toolchain`、`build_inputs_manifest`（见下）。`source.commit` 与 `build.commit` 不同时必须写 `commit_differs_reason` |
| `build`（dirty 时） | `dirty: true` 时必填 `patch_manifest` 与 `patch_set_sha256`；清单与补丁存本 RUN 证据目录，**先通过敏感扫描**；无法安全保存原始内容时记路径、变更类型、内容哈希与受控存储引用。补丁路径纳入 APR 路径检查 |
| `build_inputs_manifest` / `execution_inputs_manifest` | **可执行结构（R53/R74/R83）**：`schema_version`；`generator.name/version`；`discovery.method` / `coverage` / `limitations` / `proof`（可重复扫描摘要或计数）；`dependency_lock`；规范化与 **哈希算法固定为 SHA-256**；`inputs.confirmed[]` / `possible[]` / `dynamic[]`；`completeness`；**`manifest_sha256`**（清单自身规范化哈希）。序列化规则唯一：编码 UTF-8、字段顺序固定、数组按 path 排序、null 保留、换行 LF。无法证明完整 → 不得 CLEAN。无固件产物时 `build_inputs_manifest` 可标 **`NOT_APPLICABLE`**，不参与固件侧比较；**缺 `execution_inputs_manifest` → INVALID**（R86） |
| `completeness` 判定（R57/R61） | 见映射表：`CLEAN` → 可进入 VALID/STALE 比较；`INCOMPLETE` → **INVALID**（已确认快照缺项）；`UNKNOWN` → **UNKNOWN**（无法确认完整性）。`CLEAN` 要求能证明输入集合完整且哈希一致，且 `possible[]`/`dynamic[]` 为空 |
| `patch_manifest`（build/execution 统一，R48/R63/R71） | 字段语义见下表。`patch_set_sha256`：固定排序、序列化、规范化；**null 保留参与哈希** |

| change_type | 必填路径 | 禁止字段 | 哈希要求 |
| --- | --- | --- | --- |
| added | `path` | `old_path`、`new_path` | `content_sha256` 必填（非 null） |
| modified | `path` | `old_path`、`new_path` | `content_sha256` 与 `baseline_sha256` 均可核验（均非 null） |
| deleted | **`path`=被删除的原路径** | `old_path`、`new_path` | `content_sha256` **必须为 null**；`baseline_sha256` 必填（非 null） |
| renamed | `old_path`、`new_path` | 单独的 `path` | `old_sha256`、`new_sha256` 必填（均非 null） |

**Schema 硬约束**：`change_type` 仅允许 `added` / `modified` / `deleted` / `renamed` 四枚举。按上表互斥：禁止缺少必填字段、出现禁止字段、或以 null 替代非 null 必填哈希。路径为非空字符串；哈希字段为 SHA-256 十六进制小写 64 字符（或明确允许的 null）。结构校验失败 → Schema 错误；已入证据的记录结构违规 → `INVALID`。路径检查：deleted 检查 `path`；renamed 分别检查 `old_path` 与 `new_path`。

路径检查：deleted 检查 `path`；renamed 分别检查 `old_path` 与 `new_path`。
| `target_ids`（RUN 固化，R62） | 创建 RUN 时写入当时实际使用的 `firmware_target_id` / `host_target_id`（适用时）。历史分组与有效性以 **RUN 内固化 ID** 为准；TEST 定义后来改绑定不影响已有 RUN |
| `firmware` / `expected` | **条件必填（R30）**：仅当存在固件产物时（build/device/field）。含 `version`、`sha256`、`size`；`expected.firmware_version`、`expected.build_sha`。host/static 无固件产物时省略 |
| `device` | **条件必填（R30）**：仅 `device_required=true` 或 device/field 等级。脱敏 `id`、`reported.*`、`identity_check`。host/static 省略 |
| `environment` | 所有等级必填（R34/R85）。有 Host Target 时，规范化身份须与 Host Target 一致。**纯 device/field（无 Host Target）**：仍记录 environment，但不做 Host Target 比对；环境变化不单独改变固件身份分组，但可影响 execution 快照/STALE（按 `scope.execution`） |
| `authorization` | 枚举（R38/R49）：`APR-<id>`（**仅表示引用一条存在的 APR 记录**；其是否在执行时点授权有效，按 `approved_at`/`valid_until`/`withdrawn_at` 与 RUN 执行时间判定，**不得仅凭 APR 当前 status**）/ `not_required` / `blocked_no_authorization`。组合约束见下 |
| `blocked_reason` | **条件必填**：所有 `result=BLOCKED` 必填。**不能替代**门禁证据 |
| `attempted_actions` | **所有 `result=BLOCKED` 必须非空（R70）**；无操作尝试时不产生 BLOCKED，测试保持 NOT_RUN |
| `gate_evidence` | **条件必填（R52/R64/R77）**：仅 `blocked_no_authorization` 时必填，为列表。语义：attempted=全部尝试；gate_evidence=**仅被拦截动作**。约束：每条 action ∈ attempted；需授权且被拦截的动作有且仅有一条证据；至少一条有效证据；executed 不含被拦截项。每项：`gate_id`（**unique_within_run，R90**）、`attempted_action`、`decision`、`timestamp`（∈ RUN 执行区间，R81）、`evidence_ref`、`evidence_sha256`（**SHA-256**）。同一动作不得有矛盾 decision。证据须存在且哈希匹配 |
| `attempted_actions` | 尝试执行的操作类型列表（含被门禁拒绝、未真正落到设备上的操作） |
| `executed_actions` | **实际发生**的操作类型列表。不得把仅被尝试、未实际发生的动作写入本字段 |
| `execution` | `started_at`、`finished_at`；以及测试执行环境自身的身份（与固件构建 dirty 分离） |
| `execution`（测试环境） | `source_commit`、`dirty`、`patch_manifest`、`patch_set_sha256`。**`execution.patch_manifest` 与 `build.patch_manifest` 同样必须先经过敏感扫描**（R36）；无法安全保存原始内容时用路径、变更类型、内容哈希与受控存储引用。`execution.dirty=true` 时清单/哈希缺失或不符 → `INVALID`；无法恢复 → `UNKNOWN`；相对执行快照的后续变化与 execution scope 相交 → `STALE` |
| `evidence` | 文件清单和 `SHA256SUMS` |
| `sensitive_scan` | `CLEAN / REVIEW_REQUIRED / EXPOSURE_CONFIRMED / ROTATION_REQUIRED` |

**固件身份字段约定（消除歧义）**：

- `firmware.version` / `expected.firmware_version`：版本串（如 `1.3-9e1d6c5`），不是文件哈希。
- `firmware.sha256`：固件文件内容哈希。
- `expected.build_sha` / `device.reported.build_sha`：ELF 派生短哈希，与版本串中的短 SHA 不同源。
- 运行分组键按测试等级使用对应身份（见第 7.3 节），不使用含义模糊的单一 `firmware` 字符串。

**authorization 字段枚举与约束（R20/R31/R38）**：

| authorization 值 | 使用场景 | 结果约束 |
| --- | --- | --- |
| `APR-<id>` | 引用一条存在的 APR 记录（R49） | 授权是否在**执行时点**有效，按第 5.7 节时间字段与七项条件判定；**不得**仅凭当前 status=approved 判断历史 RUN |
| `not_required` | 测试不要求授权（`authorization_required=false`） | 不执行设备授权校验 |
| `blocked_no_authorization` | 缺有效 APR，门禁在设备动作前拦截 | **硬约束（R46/R52）**：`result=BLOCKED`；`attempted_actions` 非空；`executed_actions=[]`；`blocked_reason` 必填；**`gate_evidence` 完整且可核验**（不得仅靠执行者自填 blocked_reason）。不得与 PASS/FAIL/ABORTED 同时出现。不得作为 PASS/verified 或有效设备测试结果 |

| 测试条件 | RUN 授权要求 |
| --- | --- |
| `authorization_required=true` 且意图作为有效测试证据 | 必须 `authorization=APR-<id>` 且通过七项条件 |
| `authorization_required=false` | `authorization=not_required` |
| 设备测试未获授权且未发生操作尝试 | 不产生设备 RUN |
| 缺有效 APR、有尝试、门禁拦截、`executed_actions=[]` | `authorization=blocked_no_authorization`，`result=BLOCKED` |
| 未获有效授权但实际发生设备操作 | 异常 RUN，固定 `INVALID`（不得使用 `blocked_no_authorization`） |

**未授权尝试的三种处理（R21/R26，全文唯一口径）**：

| 情况 | RUN 处理 |
| --- | --- |
| 没有操作尝试，授权未批准 | **不产生**设备 RUN；测试显示 NOT_RUN |
| 有操作尝试，门禁在设备动作前拦截（`executed_actions=[]`，证据充分） | **可记录** `BLOCKED` + `blocked_no_authorization`；不得作为 PASS/verified |
| 未授权，但设备动作实际发生 | **异常 RUN**，固定 `INVALID` |

已授权场景：操作被其他条件阻止 → `BLOCKED`（可引用有效 APR，可有部分 `executed_actions`，但 **`attempted_actions` 必须非空，R70**）；正常完成 → `PASS`/`FAIL`。无操作尝试 → 不产生 BLOCKED，测试保持 NOT_RUN。

**测试执行环境 vs 固件构建（R11）**：固件构建干净不代表测试执行环境干净。最终验收必须同时检查 `build.dirty` 与 `execution.dirty`。

### 5.7 APR：`docs/history/approvals/` 中授权文档（原文 + 侧车元数据）

**存储原则**：授权原文保持原样，不改写。结构化元数据放在同目录侧车文件（`<apr-id>.meta.yaml`），或统一索引中。这样既满足可检索，又符合历史不可篡改与 DOC-G07。

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `id` `title` | 是 |  |
| `status` | 是 | `pending / approved / rejected / expired / withdrawn`。`pending` 是尚未批准的**申请记录**，不是已生效授权 |
| `approved_at` `valid_until` | **曾进入 approved 后永久必填（R39）** | 与当前 `status` 解耦。即使后来变为 `expired`/`withdrawn`，批准时间与原有效期仍保留，供历史 RUN 按执行时点重建授权状态 |
| `withdrawn_at` | 条件 | 曾批准后撤销时必填；撤销**生效**时间。不得早于 `approved_at`。历史 RUN 合法性按执行时点是否落在 `[approved_at, min(valid_until, withdrawn_at)]` 判断，**不以当前 status 追溯否定**（R27/R39） |
| `withdrawal_recorded_at` | 否 | 撤销事实的登记时间；与撤销生效时间分离 |
| `scope.tests` | 是 | 允许执行的 TEST 列表 |
| `scope.devices` | 是 | 脱敏设备标识列表。无设备对象的 APR 允许 `[]`；**若所覆盖 TEST 存在 `device_required=true`，则不得为空**，且 RUN 脱敏 `device.id` 必须匹配（R60） |
| `scope.actions` | 是 | 允许的操作类型（取自动作词表） |
| `firmware_target_ids` | 条件 | 涉及固件产物的授权必填（R80）；RUN 的 `target_ids.firmware_target_id` 必须 ∈ 本列表。绑定完整构建身份，不仅 baseline_commit |
| `execution_baseline` | 否 | 若声明，则第 7 条用本字段检查 `execution.source_commit`；未声明则统一用 `baseline_commit`（R80）。不得使用未建模的例外 |
| `scope.execution_paths` | 否 | 允许的测试执行环境路径。优先级：未声明 = 所授权 TEST 的 procedure/Runbook/明确执行依赖；声明后以本字段为准。匹配规则见下「统一路径匹配（R50/R58）」 |
| `baseline_commit` | 是 | 授权所依据的源码基线 |
| `baseline_policy` | 是 | `exact`：只允许该提交；`descendants_within_paths`：允许其后继提交，但后继相对 `baseline_commit` 的变更路径必须全部落在 `baseline_paths` 内 |
| `baseline_paths` | 条件 | `baseline_policy` 为 `descendants_within_paths` 时必填；列出允许变化的路径（支持目录前缀） |
| `allowed_patch_manifest` | 条件 | **仅当 `baseline_policy=exact` 且需要允许特定 dirty 补丁时**使用（R32）。每项含路径、变更类型、内容哈希；可绑定 `patch_set_sha256`。校验时与 RUN 实际补丁清单逐项匹配。若无此字段，exact 模式**不允许任何**额外 dirty 或偏离基线输入 |

**路径比较规则**（`descendants_within_paths`、dirty 补丁与 ignored 构建输入，R9/R25）：

- 必须能证明 `build.commit`（及需要时的 `source.commit`）是 `baseline_commit` 的后继。
- 相对 `baseline_commit` 的全部变更路径（含删除、重命名、目录移动）必须落在 `baseline_paths` 内。
- **所有实际参与构建且偏离基线的输入**（不论是否被 Git 忽略）必须进入构建差异清单并接受路径检查：`patch_manifest` 记录 staged/unstaged/untracked；`build_inputs_manifest` 证明实际构建输入及其内容哈希。校验器须能回答：相对 `baseline_commit`，该输入是新增、修改还是与基线一致；无法取得基线内容或无法可靠比较时，不得默认在授权范围内。ignored 文件不得成为绕过授权路径限制的入口。
- **`build.dirty=true` 或存在 ignored 偏离输入时**：上述路径必须全部落在 `baseline_paths` 内；无法证明则 RUN 判 `INVALID`。
- **`baseline_policy=exact` 时（R32/R40）**：任何偏离 `baseline_commit` 的输入（含 staged / unstaged / untracked / **ignored** / 动态生成且实际参与构建的文件）必须满足其一：① 与 baseline 完全一致；② 出现在 `allowed_patch_manifest` 中且路径、变更类型、内容哈希全部匹配。否则一律 `INVALID`。
- **execution 路径授权（R40）**：`execution.patch_manifest` 路径须落在 `scope.execution_paths`（若声明）或 TEST procedure/脚本范围；execution ignored 输入须纳入 `execution_inputs_manifest`。仅 build 满足而 execution 越权 → `INVALID`。

**统一路径匹配规范（R50/R58）**——适用于 `scope.firmware`、`scope.execution`、`baseline_paths`、`execution_paths`、`allowed_patch_manifest`、各类 patch_manifest：

1. 路径为仓库相对路径，分隔符 `/`；禁止 `.`、`..`、绝对路径。
2. 目录前缀按**递归**匹配其子路径。
3. 通配符仅允许：`*`（单段）、`**`（跨目录）、`?`（单字符）。**边界（R76）**：`**/` 匹配零级目录；`a/**/b` **能**匹配 `a/b`；禁止其他 glob/转义；语义固定为一种实现（如 pathlib 等价规则）并写入工具契约。
4. 默认匹配隐藏文件。
5. 删除检查 `path`；重命名分别检查 `old_path` 与 `new_path`。
6. 符号链接：解析目标再检查；仓库外目标默认越权，除非登记为可验证外部依赖。
7. 规范化（R67/R75/R84）：Unicode **NFC**；路径比较与匹配采用**大小写敏感**；不以本地 `core.ignorecase` 为准；本地 FS 与仓库规则冲突 → **报错**；规范化冲突 → **报错**；禁止静默合并；目录前缀以 `/` 结尾区分文件路径。

**外部依赖登记（R84）**：仓库外符号链接目标须在受控清单中登记：`id`、规范化位置、内容哈希或版本身份、允许用途、可重复验证方法。未登记 → 越权。

**统一时间语义（R81）**：所有时间字段为 **RFC 3339、带时区、存储与比较统一换算为 UTC**。`started_at <= finished_at`。`withdrawn_at=null` 视为无撤销上界（仅用 `valid_until`）。授权有效区间端点**闭区间**（含等于时刻）。`gate_evidence.timestamp` ∈ `[started_at, finished_at]`。DOC-G08 时间字段同一规范。
- 无法证明属于授权范围时，判定 `INVALID`，不能仅凭授权时间有效就放行。

**有效授权校验（编号 1–7，R73）**（仅当 `authorization_required=true` 且 RUN 意图作为有效测试证据时适用）：须同时满足下列 **1–7**，任一条不满足则 `INVALID`。`not_required` 不进入本校验。**例外**：`blocked_no_authorization`+BLOCKED+executed=[]+证据充分时不要求通过 1–7，但不得 PASS/verified。异常 RUN（实际设备操作无有效授权）固定 `INVALID`。

1. **执行时点授权有效**：`APR-<id>` 仅表示引用存在；有效性按执行时间与 `approved_at`/`valid_until`/`withdrawn_at` 判定，不得仅凭当前 status。新执行另须当前可用。
2. **执行区间覆盖**：`approved_at <= started_at` 且 `finished_at <= min(valid_until, withdrawn_at)`。跨越 `withdrawn_at` 的 RUN 不得作为有效证据。
3. **测试与设备在范围内**：`test_id` ∈ scope.tests；`device_required` 时设备 ∈ scope.devices。
4. **动作范围**（正常有效 RUN）：`executed_actions` ⊆ scope.actions，且覆盖 `required_actions`。**例外**：`blocked_no_authorization` 不执行本条覆盖要求，强制 `executed_actions=[]`（见第 12 节分支）。
5. **`source.commit` 符合 `baseline_policy`**（默认 required）。
6. **构建身份与 build 提交**：`build.commit` 符合 `baseline_policy`；且 RUN 的 `target_ids.firmware_target_id` ∈ APR.`firmware_target_ids`（有固件时）；固件 SHA256 与设备 reported 与该 Target 身份一致，否则 mismatch → INVALID。
7. **执行环境提交与偏离输入**：`execution.source_commit` 符合 `execution_baseline`（若声明）否则 `baseline_commit`；build/execution 的 dirty 与 ignored 偏离路径全部落在 `baseline_paths` / `execution_paths` / `allowed_patch_manifest` 内。

当 `source.commit` ≠ `build.commit` 时，第 5、6 条仍须同时成立。

授权尚未批准且**未发生设备操作尝试**时：不产生设备执行 RUN；任务和 APR 记录阻塞原因，TEST 因没有有效 RUN 显示为未验证。若实际发生了操作，则按 R14/R21 记录异常 RUN（INVALID）。

### 5.8 ISSUE：`docs/status/known-issues.yaml`

| 字段 | 必填 | 说明与校验 |
| --- | --- | --- |
| `id` `module` `title` `severity` | 是 | severity：`P0 P1 P2 P3` |
| `status` | 是 | `open / investigating / fixing / resolved / deferred / closed` |
| `related_requirements` `related_tasks` | 否 | 引用必须存在 |
| `evidence` | 否 | RUN ID 或证据路径 |
| `resolution` | 条件 | `resolved / closed` 时必填，写明解决依据（提交或 RUN），不能只改状态 |
| `legacy_ids` | 否 |  |

### 5.9 文档元数据（spec、adr、runbook 的头部；history 使用侧车）

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `id` `type` | 是 | type：`spec / adr / history / runbook` |
| `status` | 是 | `current / superseded / archived`，**只描述文档本身是否有效，不代表功能是否完成** |
| `module` | 是 | 跨模块文档填 `null` 并写 `null_reason` |
| `applies_to_firmware` | 是 | 如 `1.3`；历史文档保留原版本 |
| `reviewed_against` | 是 | spec 与 runbook：最近一次对照源码核对时的提交 SHA，参与“规范过期”检测；adr：文档形成时所依据的提交，保持不变，不参与过期检测。**history 类型不在此字段强制要求（见下）** |
| `updated` | 是 | 修订日期 |
| `supersedes` | 是 | 被替代文档的 ID 列表，没有则为空列表；`superseded_by` 可选 |
| `null_reason` | 条件 | 任何字段取 `null` 时必填，写明为什么不适用 |

**History 元数据规则（R8）**：历史文档（审查、审批、授权、事故、开发过程、测试报告）的原文必须保持原貌，禁止为添加元数据而修改正文。结构化元数据统一放在侧车文件（如 `original-review.meta.yaml`）或统一索引中。侧车文件可新建，原文只允许纯重命名（R100）或新增。这样同时满足可检索、Git 可追溯与 DOC-G07。

### 5.10 历史编号映射

- 不重新编号。现有编号（`CMD-01~15`、`1.3-01~80`、`DEF-NET-001`、`DEF-MQTT-001`、`S6` 各轮次等）作为 `legacy_ids` 别名保留，历史文档里的原引用保持有效，原文不改写。
- **新 ID 全局唯一；旧编号不要求全局唯一**，因为同一简称（如 `S6`）可能被多份文档重复使用。映射记录用复合定位 `legacy_id + source_path + source_section`，要求能唯一定位到原始来源。
- 映射记录在 `evidence/manifests/migration-map.yaml`。
- 例：`CMD-15`（MQTT 运行态与 config 同步）归 MQTT 模块，新条目为 `TASK-MQTT-*`，带 `legacy_ids: [CMD-15]`。
- `问题与版本优化.md` 中的历史条目：只有仍活跃的问题提取进 `known-issues.yaml`；已解决的在历史索引中保留“旧编号 → 状态 → 解决依据”，一条不丢。

## 6. 需求派生状态规则

需求在 YAML 里只记录意图（`planned / deferred / rejected`）；开发进展和验证结论由脚本从任务和测试运行计算，**不回写 YAML**。展示状态按下表自上而下取第一条匹配的规则。

| 顺序 | 条件 | 展示状态 |
| --- | --- | --- |
| 1 | 需求意图为 `rejected` | rejected |
| 2 | 需求意图为 `deferred` | deferred |
| 3 | 没有关联任务，或关联任务全部为 `cancelled` | planned，关注清单提示“无有效任务” |
| 4 | 全部非 `cancelled` 任务为 `todo` | planned |
| 5 | 存在 `blocked` 的非 `cancelled` 任务 | blocked |
| 6 | 存在未完成的非 `cancelled` 任务（`in_progress / partial / todo / deferred`） | in\_progress（已有任务 `code_done` 时标注 partial） |
| 7 | 至少有一个非 `cancelled` 任务，且全部非 `cancelled` 任务为 `code_done` | implemented，继续判第 8–14 条 |
| 8 | 已 implemented，且任一必需测试的最新运行 result=BLOCKED 且 validity ∈ {VALID, STALE, UNKNOWN} | blocked |
| 9 | 已 implemented，且任一必需测试的最新运行 result=FAIL 且 validity=VALID | implemented，标注 verification failed |
| 10 | 已 implemented，且任一必需测试的最新运行 result=ABORTED 且 validity ∈ {VALID, STALE, UNKNOWN} | unknown，标注未完成 |
| 11 | 已 implemented，且任一必需测试的最新运行 validity=STALE（且不存在更高优先级的有效 BLOCKED、FAIL(VALID)、ABORTED） | stale |
| 12 | 已 implemented，但最新运行 validity=INVALID、UNKNOWN，或没有运行 | unknown（没有运行时标注“未验证”） |
| 13 | 已 implemented，且最新运行 result=BLOCKED/ABORTED 但 validity=INVALID | 按无效运行处理，落入规则 12（不因 result 获得更高优先级） |
| 14 | 满足下面的 verified 全部条件 | verified |

**implemented 的条件**：至少有一个非 `cancelled` 的关联任务（排除取消任务后集合为空时不成立，不能按“空集合全部完成”处理）；所有非 `cancelled` 任务都是 `code_done`；不存在 `deferred`、`blocked`、`todo`、`in_progress`、`partial` 任务；任务与需求的关联完整。不增加新的需求状态，“无有效任务”用展示状态 planned 加关注项提示。

**verified 的条件**（全部满足）：

1. 已满足 implemented。
2. 每个 `kind: required` 的覆盖测试在**目标身份组**内都有运行记录（不是历史上任意身份组）；`supplementary` 测试不计入。多个必需测试按“全部通过”处理。
3. 每个必需测试按第 7 章规则选出的“最近运行”为 `PASS` 且有效性为 `VALID`。
4. 运行的构建变体符合测试的 `required_variant`，设备实际身份核对为 `match`（主机测试无设备则不适用）。
5. 证据完整，哈希校验通过。
6. 测试要求授权时，授权记录有效且覆盖该运行（含 `build.commit` 与 `source.commit` 均符合 APR `baseline_policy`）。

只要有一项不满足，就不显示 verified。没有任何 `kind: required` 测试覆盖且没有 `acceptance_waiver` 的需求是校验错误；有 waiver 的需求最高显示 implemented，不得显示 verified（R45）。

**多测试冲突时的优先级**（已体现在上表顺序中，且与第 7.2 节组合表对齐）：  
有效 BLOCKED（validity≠INVALID） > FAIL(VALID) > 有效 ABORTED（validity≠INVALID） > STALE > UNKNOWN/INVALID/缺失 > verified。  
**INVALID 的 RUN 无论 result 是 BLOCKED 还是 ABORTED，都不获得比 INVALID 更高的派生优先级**（R16）。实现时不得临时调整顺序。

**边界说明**

- 主机测试通过不等于真机测试通过；真机测试通过不等于产品验收通过。
- 缺少证据的 PASS、授权不满足的设备运行，都不能算有效 PASS。
- 展示状态是计算结果，不是人工维护的字段，也不能被测试结果反向覆盖需求意图。
- 举例（仅示意）：某任务 `partial`、`blocked_by` 为“授权待批”，则对应需求显示 `blocked`，不会显示 implemented。
- 举例：必需测试最新有效运行为 BLOCKED，即使其他测试为 PASS，需求仍显示 blocked。
- 举例：最新 RUN 为 `BLOCKED + INVALID` 时，按无效运行处理，不显示 blocked。

## 7. 附件 B：测试状态双维度组合表与有效性规则

测试状态拆成两个互相独立的维度：**运行结果**是已发生的事实，**有效性**是这份证据对当前源码的适用性。`FAIL` 和 `STALE` 不是同一类状态，INDEX 的展示状态由两者组合得出。

### 7.1 两个维度

| 维度 | 取值 | 含义 |
| --- | --- | --- |
| 运行结果（RUN 记录中的取值） | `PASS` | 测试按步骤执行并满足通过条件 |
|  | `FAIL` | 执行完成但未满足通过条件 |
|  | `BLOCKED` | 实际发生了尝试，但被明确条件阻止（必须写 `blocked_reason`）；未发生尝试时不产生 RUN |
|  | `ABORTED` | 执行中途中断，结果不完整 |
| 派生状态（不是 RUN 的取值） | `NOT_RUN` | 该测试在目标构建身份下没有任何 RUN，由脚本派生 |
| 有效性 | `VALID` | 证据完整，且 scope 内源码相对运行时没有变化 |
|  | `STALE` | scope 内的文件在运行之后发生了变化，结果不能直接代表当前源码 |
|  | `UNKNOWN` | 无法确定：提交不存在、scope 缺失、设备身份未核实、工具链或依赖变化无法比较 |
|  | `INVALID` | 证据本身不可信：证据不完整或哈希不符、授权条件不满足（含 `executed_actions` 超出授权、基线不匹配）、dirty 构建缺补丁清单或哈希不符、设备身份 mismatch |

有效性按 `INVALID → UNKNOWN → STALE → VALID` 的顺序判断，先命中的生效。STALE 只表示历史结果不能直接当作当前结论，不删除也不否定它。

### 7.2 组合表（INDEX 展示状态）

| 运行结果 ＼ 有效性 | VALID | STALE | UNKNOWN | INVALID |
| --- | --- | --- | --- | --- |
| PASS | 通过 | 需重测 | 待核实 | 无效，不计入 |
| FAIL | 失败 | 需重测（历史失败，不代表当前） | 待核实 | 无效，不计入 |
| BLOCKED | 阻塞 | 阻塞（条件需重新确认） | 阻塞 | 无效，不计入 |
| ABORTED | 中断，按未验证处理 | 按未验证处理 | 按未验证处理 | 无效，不计入 |
| 无 RUN（派生 NOT\_RUN） | 未运行 | — | — | — |

只有“通过”才能为需求的 `verified` 加分；“失败”会让需求显示 verification failed；“需重测”显示 stale；“待核实”显示 unknown。未授权尝试的处理与第 5.6 节 R21/R26 **全文唯一口径**一致：

- 无操作尝试、授权未批准 → **不产生**设备 RUN，测试显示未运行；
- 有操作尝试、门禁在设备动作前拦截 → **可记录** `BLOCKED`（`executed_actions` 为空）；
- 未授权但设备动作实际发生 → **异常 RUN，固定 INVALID**。

### 7.3 取哪一次运行

1. **按测试等级使用不同身份分组键**（R22/R28）：

| 测试等级 | 分组身份键 |
| --- | --- |
| `static` | `TEST ID` + **Host Target** 身份 |
| `host` | `TEST ID` + **Host Target** + `execution` 身份 |
| `build` | `TEST ID` + **Firmware Target** |
| `device` / `field` | **Firmware Target** + 脱敏 `device.id` |
| `integration` | 见下：组合身份（R41） |

**目标身份两类（R41）**：

| 类型 | 必须登记 |
| --- | --- |
| **Firmware Target** | `variant`、`commit`、`flags_hash`、`idf_version`、`toolchain_hash`、`inputs_hash`、`firmware_sha256`（有产物时） |
| **Host Target** | `environment_hash`、OS/架构、运行时版本、依赖清单哈希、测试脚本哈希 |

`expected.firmware_version` / `expected.build_sha` 仅用于设备身份核对，**不得代替** Firmware Target。Host/Static 不得登记虚构 firmware SHA；不得用固件变体名代替 Host Target。RUN 的 `environment` 规范化身份须与对应 Host Target 一致。

**integration 组合身份（R41/R55，与第 5.5 节一致）**：

- 不涉及设备、无固件产物 → Host Target  
- 不涉及设备、有固件产物 → Host Target + Firmware Target  
- **涉及设备** → Host Target + Firmware Target + 脱敏 device.id（**不得省略 Host**）  
- 仅 `level=device|field` 的纯设备操作 → Firmware Target + device.id  

Target ID 不可变规则见第 5.5 节 R54。RUN 创建时固化 `target_ids`（R62），历史分组以 RUN 内记录为准，不随 TEST 定义事后改绑定而迁移。

2. INDEX 的目标身份在 `tests.yaml` 顶部的 `target_builds` 中按上表登记（Firmware Target 或 Host Target），更新是评审动作。当前状态只用目标身份组内的运行判定。目标身份下无运行则显示“未运行”。

3. **多设备策略**：默认每台设备分别判定，不得跨设备覆盖。若 TEST 声明“任意一台通过即可”，须在定义中写明；否则按设备分组。

4. **组内排序**（R22）：因分组键已含 `build.commit`（或等价身份），组内身份相同；**仅按 `finished_at` 取最新**。提交祖先关系用于比较不同构建身份或历史与目标身份的关系，**不用于同一组内排序**。

5. 以组内最新一次运行作结论。**最新一次即使是 FAIL、BLOCKED、ABORTED，或有效性为 INVALID、UNKNOWN，也不能回退使用更早的 PASS。**

因此旧构建、其他固件身份或其他设备上的 PASS，既不能覆盖新结果，也不能在新运行无效时替它顶上去。`source.commit` 保留为测试对照基线，不与实际固件构建身份混淆。

### 7.4 scope 规则

- **`firmware_all` 的定义**：由实际构建依赖清单生成的完整输入集合（RUN 的 `build_inputs_manifest`，从构建的编译和依赖信息生成），并以下面的基础路径清单兜底，不只依靠人工维护的几个通配符：`main/**`、`components/**`、`managed_components/**`、`CMakeLists.txt`、`main/CMakeLists.txt`、`main/Kconfig.projbuild`、`sdkconfig*`（含 `sdkconfig.defaults`）、`partitions.csv`、`dependencies.lock`、`idf_component.yml`、`bootloader/` 与自定义 linker script、子模块及其固定版本、参与构建的脚本和生成配置。`host / static / build` 类测试显式列出被测源码、测试代码和构建脚本。
- **自动并入**：测试步骤用到的脚本（`tests/`、`tools/` 下）和 runbook 自动并入 **execution scope**；**直接引用及可静态识别的传递依赖**一并并入。无法自动识别的依赖由 TEST 显式声明；不能证明依赖集合完整时至少判 `UNKNOWN`。
- **缩小范围**：必须填写 `scope_reason`，评审确认后才生效；不得为减少 STALE 而缩小，也不得把无关的整个仓库路径加入。
- **固件 scope vs 执行 scope（R25/R44）**：TEST 使用独立字段 `scope.firmware` 与 `scope.execution`。`docs/`、`README.md`、`evidence/` 的变化**不会**使固件快照 STALE；`docs/runbooks/`、`tests/`、`tools/` 中被引用的文件属 **execution scope**，其修改可使执行快照 STALE。两范围分别生成快照、分别比较，最终有效性按第 7.1 节优先级统一判定。
- **工具链与依赖**：RUN 记录 ESP-IDF、编译器、工具链和组件依赖的精确版本，并与当前构建环境比较；只记录版本不够，必须能判断环境是否与测试时一致。无法比较的外部依赖或工具链差异判 `UNKNOWN`，不能默认 `VALID`。
- **重命名**：按实际 Git 差异（含重命名检测）处理。

### 7.5 判定方法

有效性判定比较的是 **RUN 实际执行快照** 与 **当前状态**，而不是裸提交与当前工作区（R19）。否则测试前已存在的 dirty 补丁会被误判为 STALE。

| 快照 | 构成 |
| --- | --- |
| 固件快照 | `build.commit` + `build.patch_manifest`（若 dirty）+ **`build_inputs_manifest`**（含 completeness） |
| 测试执行快照 | `execution.source_commit` + `execution.patch_manifest`（若 dirty）+ **`execution_inputs_manifest`**（含 completeness；R61） |
| 当前状态 | 当前 HEAD + staged + unstaged + untracked + 参与构建/执行的 ignored 文件 |

`source.commit` 保留为测试对照基线；若与 `build.commit` 不同，必须解释差异是否影响测试结论，不能只记 `commit_differs_reason` 就放行。

**判定规则（R61）**：

0. 先检查输入清单：`execution_inputs_manifest` **始终必填**，缺失 → **INVALID**。`build_inputs_manifest`：有固件产物时必填；无固件产物时可为 `NOT_APPLICABLE` 并跳过固件侧比较（R86）。`completeness`：`INCOMPLETE`→INVALID；`UNKNOWN`→UNKNOWN；仅 `CLEAN`（或 NOT_APPLICABLE 的固件侧）可进入后续比较。
1. 分别将「当前状态」与「固件快照」「执行快照」比较，生成规范化差异清单（含 committed/staged/unstaged/untracked/deleted/renamed，以及 **build 与 execution 两侧** ignored 参与输入的内容哈希变化）。
2. 固件差异与 `scope.firmware` 相交 → 固件侧 STALE；执行差异与 `scope.execution` 相交 → 执行侧 STALE；任一侧 STALE 则 RUN 为 `STALE`。
3. 无法还原 RUN 当时的实际快照 → `UNKNOWN`。
4. 快照哈希或补丁/输入清单本身不可信（缺失、不符）→ `INVALID`。
5. 仅存在 RUN 当时已记录在 patch_manifest / inputs_manifest 中、且此后未再变化的差异 → **不**因此判 STALE。

实现上可先应用 patch_manifest 得到“执行时树”，再与当前树做路径级比较；Git 看不到的 ignored 输入一律以对应 `*_inputs_manifest` 与补丁清单的哈希为准。

### 7.6 dirty 构建、构建变体与设备身份

- **dirty（固件构建）**：`build.dirty: true` 不自动无效，但必须有 `patch_manifest` 与 `patch_set_sha256`，证据在本 RUN 目录并先通过敏感扫描。清单缺失或哈希不符一律判 `INVALID`。补丁路径及参与构建的 ignored 偏离输入必须全部落在 APR `baseline_paths`（或 exact 下显式允许）内，否则 `INVALID`（R9/R25）。STALE 比较以「固件快照」为准（见第 7.5 节），不得把测试前已有且已记录的补丁误判为过期。
- **dirty（测试执行环境）**：与固件构建独立。通过 `execution.dirty` / `patch_manifest` / `patch_set_sha256` / **`execution_inputs_manifest`** 记录。清单/哈希缺失或不符 → `INVALID`；completeness 非 CLEAN 按 R61 映射；无法恢复 → `UNKNOWN`；相对执行快照的后续变化与 `scope.execution` 相交 → `STALE`。最终验收必须同时检查 build 与 execution 两侧 dirty 与 inputs。
- **`source.commit` 与 `build.commit`**：前者是测试对照的源码基线，后者是实际构建所基于的提交；两者不同必须记录原因，不让两个 SHA 含义重叠。**授权校验必须同时检查两者是否落在 APR 的 `baseline_policy` 范围内**（见第 5.7 节）。
- **构建变体**：`tests.yaml` 顶部维护变体登记表（名称、构建开关、用途），例如生产变体，以及带 `APP_WIFI_FAULT_INJECT`、`APP_C1_TEST_HOOKS`、`APP_CMD_MQTT_NET_TXN_TEST_BUILD` 等开关的测试构建。在测试构建上的 PASS 只验证要求同一变体的测试，不能当作生产镜像的通过。
- **设备身份**：预期值由构建产物生成并记录在 RUN 的 `expected.firmware_version` 与 `expected.build_sha`（版本串与 `build_sha` 不同源，不能假定相同），实际值取自 `device.info` 的 `reported.firmware` 与 `reported.build_sha`。逐项一致记 `match`；不一致记 `mismatch` 并判 `INVALID`；取不到记 `unverified` 并判 `UNKNOWN`。不能猜测设备运行的是哪个版本。

## 8. Contract 一致性与 OTA 现状规则

`contract/` 是协议事实源，Spec 不再重复定义其中的参数和错误码；Contract 与源码的一致性由第一期校验器自动检查。OTA 规范只能写已经由源码或证据确认的能力。

### 8.1 Contract 定位与已发现的基线差异

Contract 管理命令名、参数名与类型、返回结构、错误码、状态机和协议约束。当前已发现一处需要核实的差异：`commands.json` 记录的版本为 `1.1-draft`，固件基线为 `1.3-9e1d6c5`（build\_sha `8cc566bd0`，2026-11-10），而 README 记录的源码基线是 `5104a3b`。两个 SHA 不一致不自动说明命令定义有误，但说明基线需要重新核实。P0 必须确认：

1. 两个 SHA 各自的含义。
2. `commands.json` 是否对应旧固件。
3. 当前 `main/cmd_v1.c` 的命令表是否已有变化。
4. Contract 是否缺少未同步的命令。
5. 外部消费者（`wx-ble-provision`）是否依赖旧定义。

### 8.2 第一期必须有的自动检查

比较 `contract/v1/commands.json` 的命令名集合与 `main/cmd_v1.c` 中 `k_v1_commands[]` 的命令名集合，输出：源码独有命令、Contract 独有命令、重复项、无法解析项，并生成差异报告。

测试钩子只按**明确的排除清单**处理，不使用宽泛过滤。清单每项记录名称、排除原因、所属类别、是否只在测试构建中启用。已知的两项例子：

| 名称 | 受哪个开关控制 | 类别 |
| --- | --- | --- |
| `diag.m1hook` | `APP_WIFI_FAULT_INJECT` | 故障注入，仅测试构建 |
| `diag.testhook` | `APP_C1_TEST_HOOKS` | 状态机探针，仅实验构建 |
| （无额外命令名） | `APP_CMD_MQTT_NET_TXN_TEST_BUILD` | 策略开关：启用 `APP_CMD_MQTT_NET_TXN_ENABLE`，让 MQTT 来源能执行 `network.tryconnect / apply / cancel`。不改变命令名集合，但改变这些命令的来源授权 |

命令集合比较分别在“生产构建”和各个测试构建上进行：排除清单只用来从生产集合中去掉测试构建独有的命令，不把实验命令当成生产协议命令。“同一命令在不同构建下的来源授权是否一致”属于后续检查。

### 8.3 后续检查与边界

后续可增加参数字段、类型、错误码、状态机约束的一致性检查，以及外部协议消费者的兼容性检查。本次重构**不修改 Contract 的协议语义**；发现差异先报告，再单独评审、单独授权修订。`1.1-draft` 升为正式版本的条件另行评审。

### 8.4 OTA 规范的写法

按现有审查结论，当前固件具备双 OTA 分区、新镜像启动后的健康确认（在限定窗口内检查 STA 拿到 IP 或 MQTT 已连接）和失败回滚；没有固件下载或上传的传输实现。OTA 规范因此必须：

- 分开写：分区布局、分区选择、启动确认与回滚，以及当前实际存在的 OTA 接口。
- 明确写出：**当前固件不具备已验证的完整 OTA 传输实现**。
- 不把分区存在、回滚确认或某个 MQTT 命令描述成 OTA 已实现；以上结论在写规范时仍须对照当前源码核实。
- 未来的 OTA 传输能力作为需求写入 `requirements.yaml`，状态 `planned`，设计、实现和验收完成后再由脚本更新派生状态。

## 9. Evidence 体系与敏感信息处置

仓库只保存运行元数据和小型证据，大文件先扫描再决定去向；**公开仓库的 Release 同样是公开的**，含凭据或疑似含凭据的文件不能放进去。凭据一旦确认暴露，先轮换，再谈清理历史。

### 9.1 存储策略

| 内容 | 去向 |
| --- | --- |
| 运行元数据、`result.json`、摘要、`SHA256SUMS`、证据索引、迁移映射 | 仓库内 `evidence/` |
| 小型文本证据 | 仓库内 `evidence/`，先通过敏感扫描 |
| `.bin`、`.gz`、大型日志、NVS 备份、固件构建产物 | 不直接进 Git；扫描为 `CLEAN` 且有保留价值的，可放 Release 附件或受控存储 |
| 疑似或确认含凭据的文件 | 私有受控存储，或可靠脱敏后再保存；**不得进入公开 Release** |

仓库内保留文件名、SHA256、存储位置和关联 RUN。

每次运行一个独立目录，不可覆盖：

```text
evidence/runs/RUN-WIFI-015-001/
├── metadata.yaml   字段见第 5.6 节
├── result.json
├── SHA256SUMS
└── summary.md
```

### 9.2 历史证据：先建索引，不移动

`tools/ble_p0/results/`、`artifacts/`、`docs/evidence/`、`docs/archive/` 四个目录先完整盘点，不因目录整齐而批量搬迁。目前按文件系统粗略统计：`tools/ble_p0/results/` 约 263 个、`artifacts/` 约 156 个、`docs/evidence/` 与 `docs/archive/` 合计约 112 个（两个目录未分开统计）。P0 需要分别统计 Git 跟踪文件和工作区文件，并列出四个目录各自的数量。索引每条记录：原始路径、类型、大小、SHA256、对应测试或任务、是否被脚本引用、是否被文档引用、是否被外部流程引用、敏感扫描结果、建议处理方式、迁移状态。

大文件处理顺序：保留原路径 → 计算 SHA256 → 确认引用关系 → 敏感扫描 → 评估长期保留价值 → 评估 Git 历史体积增长 → 决定 Release 或受控存储 → 仓库内留索引和校验值。不因体积大就直接删除。

### 9.3 敏感信息扫描（P0 最高优先级）

**范围**：整个仓库和**全部 Git 历史**（含所有分支、标签和已删除的文件），不只是证据目录。重点包括：

- `tools/` 下的探测和测试脚本（从文件名看可能涉及 MQTT broker 配置，是否硬编码凭据需要核实）；
- `sdkconfig*`、被 Git 跟踪的构建目录；
- `nvs_backup_*.bin`、`current_config.json` 等配置导出；
- `docs/` 和 `问题与版本优化.md` 中的示例与日志。

凭据类别：MQTT Token、Wi-Fi 密码、管理口令、配网 PIN、设备密钥、API Key、证书私钥。**NVS 是否加密、BLE 链路加密、出厂口令与登录失败锁定等安全属性，均列为 P0 待核实的安全风险候选**；在未经源码与实际配置证据确认前，不写成既定事实。在被证明安全之前，NVS 备份一律按“可能含凭据”对待。

**扫描规则**：只读；不输出秘密原文；报告只含路径、文件类型、凭据类别、风险等级、是否确认、处置建议。二进制 NVS 文件不能凭扩展名判断，采用只读解析（按已知键名）加模式扫描，无法确认的标为待人工审查。

| 风险等级 | 含义 | 处理 |
| --- | --- | --- |
| `CLEAN` | 未发现疑似凭据 | 进入普通证据评估 |
| `REVIEW_REQUIRED` | 存在疑似敏感信息 | 暂停归档，人工核实 |
| `EXPOSURE_CONFIRMED` | 确认含真实凭据 | 暂停相关证据迁移 |
| `ROTATION_REQUIRED` | 确认已暴露或需撤销 | 优先安排凭据轮换 |

**扫描完整性证明**：“扫描了全部 Git 历史”必须有证据支撑。P0 报告至少记录：是否为 shallow clone；扫描了哪些本地分支、远端跟踪分支和标签；是否存在未纳入扫描的远端引用；扫描的提交和对象范围；是否有无法解析或无法扫描的二进制对象。如果本地不是完整历史，报告只能写“当前可见历史扫描完成”，不能写成“全部 Git 历史扫描完成”。P0 在独立分析克隆中执行，不动开发工作区，需要完整历史时在该克隆内获取。

**扫描器与报告自身的安全**：扫描器不把凭据原文写入终端日志、异常堆栈、临时文件或报告；报告只保存路径、类别、风险等级、是否确认和处置建议。

### 9.4 安全事件处置顺序（独立紧急流程）

确认公开仓库中存在真实有效凭据后，按以下顺序处理，不等待文档重构各阶段的评审节奏：

1. 暂停相关证据的迁移和归档，不再复制敏感文件。
2. 通知仓库负责人。
3. **一律按已泄露处理**，优先撤销或轮换凭据。
4. 评估影响范围：暴露时长、是否被克隆或 Fork、涉及的设备和平台。
5. 之后再评估是否清理 Git 历史。重写历史对已被克隆或 Fork 的副本无法消除泄露，并会影响协作者，必须单独决策。
6. 事件记入 `history/incidents/`，处理完成后恢复证据归档。

**P0 只负责识别、报告和提出处置建议**：不擅自修改设备配置，不登录外部平台，不执行凭据轮换。轮换属于独立安全处置，需要单独授权。

## 10. Spec、ADR、History、README 与 AGENTS

Spec 说明现在的设计是什么，ADR 说明为什么这样设计，History 记录某次具体发生了什么，三者不能混用。文档自身的状态（是否现行有效）和功能的完成状态也是两件事，分开记录。

| 类型 | 回答的问题 | 规则 |
| --- | --- | --- |
| Spec | 当前有效的设计 | 区分已实现、未实现、已知限制、计划能力；写明关联需求、Contract、源码位置；头部带 `reviewed_against`（指向当前核实的源码提交） |
| ADR | 为什么这样决定 | 一个决策一个文件，只增不改；记录背景、问题、决策、替代方案、理由、影响、状态、关联提交或证据；被替代的保留并标 `superseded` |
| History | 过去发生了什么 | 审查、审批、授权、事故、开发过程、测试报告；**原文原样保留**，只允许纯重命名或新增；元数据放在侧车 `.meta.yaml` 或统一索引，禁止修改正文添加头部 |
| Runbook | 如何执行某项操作 | 构建、烧录、BLE 验证、恢复出厂、故障恢复；头部带 `reviewed_against`（指向当前核实的源码提交），不放历史执行结果 |

**规范可能过期的检测**：校验器对每份 Spec 取其列出的源码文件，用与测试 STALE 相同的路径差异办法，比较 `reviewed_against` 与当前工作区；有变化就在 INDEX 的关注清单里提示“该规范需要重新核对”。

**初始 ADR 候选**：STA-only 网络架构、NimBLE 架构、移除 Classic SPP、移除 Web 与 SoftAP、ThingsBoard MQTT 方案、配置事务与回滚策略、资源管理原则。

**History 的处理**：用 `git mv` 做经批准的路径迁移，保留 Git 历史；当前规范从历史记录中提炼，并保留原始出处；历史 PASS 不自动成为当前 PASS。元数据一律使用侧车文件或索引，禁止改写原文内容。

**README** 只描述产品定位、当前真实功能、架构概览、当前限制、构建与开发方法、文档入口、适用固件版本；不承担开发任务跟踪、测试结果、审批流程、事故记录、完整命令参数和评审过程。所有能力描述必须对照当前源码核实。

**AGENTS.md** 控制在 30 到 40 行，只放阅读顺序、事实源和硬规则，不重复架构正文。草案如下：

```markdown
# AGENTS.md（适用固件 1.3）

## 阅读顺序
1. README.md：当前真实能力与限制
2. docs/INDEX.md：当前状态、需关注项、导航
3. docs/spec/modules/<模块>.md：设计与约束
4. docs/requirements.yaml 与 docs/status/*.yaml：需求、任务、测试、问题
5. 源码与 evidence/runs/：最终核对

## 事实源
- 命令、参数、错误码：contract/，不要在别处重复定义
- 需求、任务、测试、问题：docs 下的 YAML 是唯一来源
- docs/INDEX.md 的 GENERATED 区域由脚本生成，禁止手改

## 规则
- 固件版本系列与源码提交 SHA 分开记录，不能互相代替
- implemented / verified 由脚本计算，不写进 YAML
- 编译成功、日志里出现 PASS，都不等于验收通过
- 历史 RUN、审批、事故记录不得覆盖或删除；重测新建 RUN
- 设备操作（烧录、BLE、串口、NVS）必须有 APR 授权
- 凭据不得进入仓库；发现疑似泄露先停止并通知负责人
- 修改 main/ 源码或协议语义须走项目审查流程，文档重构不得顺带修改
- 不凭历史文档推断当前实现，先核对源码
```

## 11. 附件 C：INDEX 布局草图

`docs/INDEX.md` 固定为四个区域，由上到下是：当前基线、模块状态矩阵、需要关注的事项、文档导航。工程师和 AI 打开它，先看到“现在是什么版本、各模块到哪一步、哪里有问题”，再按导航进入细节。

| 区域 | 内容 | 数据来源 |
| --- | --- | --- |
| 1 当前基线 | 固件系列、当前 HEAD 短 SHA、Contract 版本、需求/任务/测试/问题总数 | Git、`contract/`、各 YAML |
| 2 模块状态矩阵 | 每个模块的需求数及各展示状态的个数 | 第 6、7 章的计算结果 |
| 3 需要关注 | 阻塞任务、失败测试、过期（stale）测试、待核实（unknown）、授权待批或过期、待处理的疑似敏感信息、需重新核对的规范、未覆盖需求、未关闭的 P0/P1 问题 | 校验器输出 |
| 4 文档导航 | 各模块 Spec、需求/任务/测试/问题文件、ADR、Runbook、History 与 Evidence 索引的链接 | 目录扫描 |

生成规则：

- 生成内容放在 `<!-- GENERATED:START -->` 与 `<!-- GENERATED:END -->` 之间；区域外只放一两句使用说明，由人工维护。
- 输出必须可重复：条目按固定顺序排列，**不写入当前时间**，同样的输入生成同样的文件。
- 初期只显示计数、阻塞项、未覆盖项和有效性，不显示百分比；条目和权重稳定之后再评估。

**矩阵计数口径（互斥，每条需求只进一个桶，R60）**

分类函数（自上而下，命中即停）：`rejected` → `deferred` → `blocked` → `stale` → `unknown`（含 INVALID/无有效 RUN）→ `verification failed`（FAIL+VALID，计入独立列或 implemented 旁注列，不得与 verified 同桶）→ `verified` → `implemented`（含有 waiver、无 required TEST 已完成实现但未验证）→ `planned`。有 waiver 的需求不得进 verified。多必需测试冲突时，按第 6 章优先级取展示状态后再入桶。

- “需求”列 = `requirements.yaml` 中该模块的条目总数，包含 deferred 和 rejected。
- 各状态列 = 该模块需求按第 6 章展示状态归类的条数，状态互斥，各列之和等于需求数；deferred 和 rejected 在实际输出中各有一列，下面的示意为省空间未列出。
- verified 是从 implemented 里进一步满足验证条件的部分，不与 implemented 重复计数；stale、unknown 同理。
- “需要关注”里的每一项都带有对应条目的 ID，可以直接定位。

下面是版式示意，**数据为虚构，不代表项目实际进度**：

```markdown
<!-- GENERATED:START -->
## 1. 当前基线
固件系列 1.3 · HEAD abc1234 · Contract 1.1-draft
需求 38 · 任务 52 · 测试 41 · 开放问题 9

## 2. 模块状态矩阵
| 模块  | 需求 | planned | in_progress | blocked | implemented | verified | stale | unknown |
| ----- | ---- | ------- | ----------- | ------- | ----------- | -------- | ----- | ------- |
| BLE   | 6    | 1       | 1           | 0       | 2           | 2        | 0     | 0       |
| Wi-Fi | 5    | 0       | 1           | 0       | 1           | 2        | 1     | 0       |
| MQTT  | 4    | 0       | 1           | 1       | 1           | 1        | 0     | 0       |

## 3. 需要关注
- 阻塞：TASK-MQTT-005（授权待批）
- 需重测：TEST-WIFI-015（scope 内 main/wifi.c 已变化）
- 待核实：RUN-BLE-007-002（设备身份未核实）
- 未覆盖需求：无
- 需重新核对的规范：docs/spec/modules/mqtt.md

## 4. 文档导航
Spec · 需求 · 任务 · 测试 · 问题 · ADR · Runbook · History · Evidence 索引
<!-- GENERATED:END -->
```

## 12. 自动化工具与禁止事项

第一期只做三个本地脚本，输出可审查、结果可重复；稳定之后才接 CI，且 CI 只执行只读的文档质量检查，不成为固件构建的强制依赖，也不自动改变需求状态。

| 脚本 | 职责 |
| --- | --- |
| `generate_index.py` | 读取 YAML 与 RUN 记录，计算派生状态，更新 `INDEX.md` 的生成区域；输出确定性排序、不含当前时间 |
| `validate_docs.py` | 校验数据、引用、有效性与一致性，见下表 |
| `scan_sensitive.py` | 扫描指定范围（含 Git 历史）的疑似凭据，只读，不输出秘密原文，不修改被扫描文件 |

**`validate_docs.py` 第一期检查项**

| 类别 | 检查内容 |
| --- | --- |
| 数据 | YAML 语法；Schema 与枚举；新 ID 格式与全局唯一；`legacy_ids` 映射能通过“旧编号 + 来源路径 + 章节”唯一定位到原始来源 |
| 引用 | REQ、TASK、TEST、RUN、APR、ISSUE 的引用必须存在；只允许写权威方向；Spec、Evidence、文档本地链接路径存在 |
| 需求覆盖 | `status=planned`：须有 required TEST 覆盖或 `acceptance_waiver`；`deferred`/`rejected`：不要求覆盖与 waiver（R66） |
| Contract | `commands.json` 与 `k_v1_commands[]` 的命令名集合比较，按生产构建和各测试构建分别进行，排除清单受控（第 8 章） |
| 测试有效性 | 默认 scope 与 `scope_reason`；规范化 `changed_paths` 与 scope 比较；`build_inputs_manifest` 哈希比较；运行分组键（测试 + 变体 + 固件身份）和“最新一次不回退”；dirty 补丁清单与哈希；`expected` 与 `reported` 身份核对；变体符合 `required_variant` |
| 授权 | **当前可用性**与**执行时点有效性**分判（R42）。**正常有效 RUN**：当前可用或按时间字段重建历史有效；测试与设备在范围内；`executed_actions` ⊆ `scope.actions` **且覆盖** `required_actions`；`source.commit`/`build.commit`/**`execution.source_commit`** 符合 baseline（§5.7 第 5–7 项）；有固件时 `target_ids.firmware_target_id` ∈ APR.`firmware_target_ids`。**`blocked_no_authorization` 分支（强制，优先于覆盖规则）**：不执行「executed 覆盖 required」；强制 `executed_actions=[]`、`attempted_actions` 非空、`gate_evidence` 可核验；不得计入 verified；不得因缺 APR 单独 INVALID |
| 证据 | 文件存在、SHA256 校验通过、运行记录未被改写；RUN 只含 `PASS / FAIL / BLOCKED / ABORTED` |
| 派生状态 | 重新计算需求展示状态，与 INDEX 生成区域逐字一致 |
| 文档元数据 | 头部字段完整，空值有 `null_reason`；`applies_to_firmware` 与当前系列一致；spec 与 runbook 是否需要重新核对；模块规范超过 400 行给出警告 |

**脚本不得做的事**

- 自动把 `implemented` 改成 `verified` 并写回需求 YAML。
- 因为编译成功或日志里出现 PASS，就认定功能或设备验收通过。
- 自动删除旧测试记录，或覆盖已有 RUN。
- 修改 Contract 的协议语义。
- 自动搬迁敏感证据。
- 连接设备或执行任何设备操作。
- 在输出中显示秘密原文。

后续可增加：参数、类型、错误码一致性检查，外部协议消费者兼容性检查，Release 附件索引检查。

**执行顺序（R88）**：正式验收必须执行「pre-generate → 生成 → post-generate → **经阶段授权提交 INDEX** → 确认已提交」。不得把“生成成功”与“已提交”混为一谈。

1. **pre-generate**：`validate_docs.py --mode pre-generate`（检查 YAML、Schema、引用、授权、证据与测试有效性；**不**要求 INDEX 生成区已与当前数据一致）。
2. **生成**：`generate_index.py` 写入生成区域。
3. **post-generate**：`validate_docs.py --mode post-generate`（完整校验，含 INDEX 与派生状态逐字一致）。
4. **经阶段授权提交**：将已通过 post-generate 的 INDEX（及必要侧车）按阶段批准提交。
5. **确认已提交**：`git diff HEAD --exit-code -- docs/INDEX.md`（工作区与暂存区相对 HEAD 均无未提交 INDEX 修改）。

**首次生成或仅修改数据模型时，允许先运行 generate 再校验**；进入阶段验收与 CI 时必须完整执行上述顺序。

## 13. 垂直切片验证

在铺开全部模块之前，先用三条完整链路验证数据模型和脚本；切片通过评审后才允许扩展到其他模块。每条链路都走完 `REQ → TASK → TEST → RUN → Evidence → INDEX`。

| 链路 | 归属 | 测试等级 | 用来验证什么 |
| --- | --- | --- | --- |
| Wi-Fi 换网事务 | `REQ-WIFI` | device，需授权 | 真机测试、授权、构建变体、设备身份核对、STALE 判定 |
| CMD-15：MQTT 运行态与 config 同步 | `REQ-MQTT`，`legacy_ids: [CMD-15]` | device，需授权 | 任务 `partial`、`blocked`、授权待批时需求的派生状态 |
| Token Store | `REQ-SEC` | host，无需设备 | 一条不依赖硬件的完整 PASS 链路（`make -C tests/token_store run` 与 `audit_critical_sections.py`） |

具体需求措辞、任务拆分、命令和源码位置，以 P0 对当前源码和现有文档的核实结果为准；CMD-15 目前文档记载的进度（部分子项代码完成、部分未获授权）也要在 P0 核实后再录入，不预设。

**历史运行的导入**：现有设备验收和回归报告可以作为历史 RUN 导入，但原报告不改写。缺少必填字段的，对应字段标 `unknown`，有效性判 `UNKNOWN`，不补造数据。

**夹具测试**：下面的场景用虚构的 YAML 夹具（放在 `tests/docs/fixtures/`）做校验器单元测试，不使用真实数据，也不需要设备：

| 场景 | 期望结果 |
| --- | --- |
| 任务 `code_done`，但测试未通过 | 需求不显示 verified |
| 需求只有一个任务且为 `cancelled` | 显示 planned，关注清单提示“无有效任务”，不显示 implemented |
| 任务全部 `code_done`，另有一个 `deferred` 任务 | 不显示 implemented |
| 测试 PASS，但 scope 内源码在运行之后变化 | stale |
| 只有 `docs/` 提交，scope 内源码未变 | 仍为 verified，不误判 stale |
| 被 `.gitignore` 忽略但参与构建的文件内容变化 | 按 `build_inputs_manifest` 判 stale，不显示有效 PASS |
| 需求没有任何 RUN | 不显示 verified，显示未验证 |
| 证据哈希与 `SHA256SUMS` 不一致 | invalid，不计入 |
| APR 为 `pending`，测试要求授权 | 不产生设备 RUN；任务显示 blocked，需求显示 blocked，测试显示未运行 |
| RUN 引用的 APR 已 approved，但 `finished_at` 超过 `valid_until` | invalid |
| `executed_actions` 超出 APR 的 `scope.actions` | invalid |
| `source.commit` 不符合 APR 的 `baseline_policy` | invalid |
| 旧提交上有 PASS，较新提交上有 FAIL | 以较新的为准显示失败，旧记录保留 |
| 最新一次运行是 INVALID，更早一次是有效 PASS | 不回退使用更早的 PASS，显示无效运行的结果 |
| 同一变体下两个不同固件身份各有一次 PASS 和 FAIL | 只用目标构建身份组的运行判定，其他组仅作历史 |
| `dirty` 构建没有补丁清单，或清单哈希不符 | invalid |
| `source.commit` 与 `build.commit` 不同且没有原因 | 校验错误 |
| 测试构建上的 PASS，而测试要求生产变体 | 不计入验证 |
| 设备身份核对为 mismatch / unverified | 分别为 invalid / unknown |
| RUN 的 `result` 写成 `NOT_RUN` | 校验错误 |
| 缩小 scope 没有 `scope_reason` | 校验错误 |
| 在非权威方向写了引用（如 REQ 里写 `tasks`） | 校验错误 |
| 同样输入重复生成 INDEX | 输出逐字一致 |
| `level=device` 但 `device_required=false` 或 `authorization_required=false` | Schema 校验错误（R37） |
| 缺 APR、门禁拦截、`attempted_actions` 非空、`executed_actions=[]`、`authorization=blocked_no_authorization`、`result=BLOCKED`、`gate_evidence` 完整 | 允许作为审计记录；Schema 通过；不因缺 APR 单独 INVALID；不得计入 verified；不得因「executed 未覆盖 required」判 INVALID |
| `result=BLOCKED` 且 `attempted_actions=[]` | Schema/校验错误，不允许作为有效审计记录 |
| `blocked_no_authorization` 但 `executed_actions` 非空 | 校验错误 / INVALID |
| `baseline_policy=exact`，ignored 构建输入偏离基线且不在 `allowed_patch_manifest` | INVALID |
| `execution.dirty` 修改测试脚本，路径不在 APR `scope.execution_paths` / procedure 范围 | INVALID（R40） |
| `authorization=blocked_no_authorization` 但 `result=PASS` | 校验错误 |
| 曾 approved 后 status=expired，但缺少 `approved_at`/`valid_until` | 校验错误（R39） |

三条链路加全部夹具通过，才算垂直切片验收通过。

## 14. 附件 D：分阶段计划与 P0 执行验收清单

实施分为六个阶段，每个阶段都有进入条件、退出条件和独立的提交集；前一阶段的退出条件没有满足，就不进入下一阶段。每个阶段的提交可以整体回退，P0 的报告不与后续任何仓库修改混在同一提交里。

### 14.1 阶段计划

| 阶段 | 进入条件 | 主要工作 | 退出条件 | 提交边界 |
| --- | --- | --- | --- | --- |
| P0 只读盘点 | 本方案终审通过，并批准启动 P0 | 在独立分析克隆中盘点全部文件、全部可见 Git 历史和 Evidence；识别风险；交付 Excel 报告（不提交仓库） | 14.3 节的完成门槛全部满足，仓库未被修改 | 无仓库提交 |
| P1 数据模型 | P0 报告审查通过，最终目录路径和模块归属已确认 | YAML Schema、ID 与 legacy 映射、scope 规则、元数据格式、目录骨架 | Schema 通过评审；不批量搬迁历史文档和证据；**不创建 `tests/docs/` 下内容** | 一个提交集：`docs/schemas/`、目录骨架（不含 `tests/docs/`） |
| P2 切片与规范 | P1 通过 | 三条垂直切片；用最小原型脚本验证模型；整理 Architecture、Lifecycle、Resource Management、Protocol 和 11 份模块规范；建立 ADR；更新 README；创建 `tests/docs/fixtures/` 与夹具 | 三条链路和第 13 章全部夹具通过；规范已对照源码核实 | 切片数据、原型脚本、`tests/docs/fixtures/`、规范分批提交 |
| P3 历史与证据 | P2 通过 | 历史文档分类与原样归档；Evidence 索引；敏感证据处置；旧编号映射落地 | 历史映射完整，原始证据不丢；所有归档都用 `git mv`，可追溯到原路径 | 每批归档一个提交 |
| P4 本地工具 | P3 通过 | 把原型脚本加固为正式的 `generate_index.py`、`validate_docs.py`、`scan_sensitive.py`；一致性检查 | 工具测试通过且输出可重复；DOC-G01～G09 都有可执行检查 | `tools/docs/` 与 `tests/docs/` |
| P5 切换与 CI | P4 通过 | 正式切换文档入口、AGENTS.md；接入 CI；“无文档影响”声明门禁 | DOC-G01～G09 全部通过；新入口启用，旧入口都有指向 | 切换提交与 CI 提交分开 |

说明：P2 的最小原型脚本只用来验证垂直切片的数据模型，放在 `tools/docs/` 并标明为原型；P4 再把它加固为正式工具。P0 使用的扫描脚本是会话里的一次性产物，不提交，`scan_sensitive.py` 到 P4 才正式入库。`tests/docs/fixtures/` 与夹具测试在 P2 创建，与目录开放时间（第 16.1 节）保持一致。

### 14.2 P0 的范围与边界

| 项目 | 要求 |
| --- | --- |
| 范围 | 全部工作区文件（含 Git 跟踪、未跟踪、被忽略）；全部可见 Git 历史（含分支、标签、已删除文件）；全部文档；全部 Evidence |
| 文件分类口径 | **三类分别处理**：① Git 跟踪文件必须 100% 分类；② 未跟踪且未被忽略的文件必须盘点并分类；③ 被 `.gitignore` 忽略的文件必须盘点，并按文件类型与风险决定逐项分类或按目录批量归类 |
| 敏感扫描范围 | 与上表三类一致，覆盖工作区内的未跟踪与被忽略文件，以及全部可见 Git 历史；不得只扫描跟踪文件 |
| 数据来源（R23） | **两部分**：① 独立分析克隆：负责 Git 历史、跟踪文件、提交与引用扫描；② 原开发工作区的只读文件清单或只读快照：负责未跟踪、ignored 文件及其敏感扫描。普通 clone 不会包含原工作区的 ignored/untracked 文件，必须额外采集 |
| 运行方式 | 在独立分析克隆中执行历史与跟踪文件盘点；对原工作区仅做只读列举与哈希，不写回、不清理 |
| Git 操作边界（R12） | **允许**在独立分析克隆中执行必要的 Git 读取操作：`clone`、`fetch`、`status`、`diff`、`log`、历史对象检查。**禁止**任何改变开发仓库或其历史的 Git 写操作：`add`、`commit`、`push`、`mv`、`reset`、`rebase`、`filter-repo` 等。其他可能修改仓库状态的操作须单独授权 |
| 只读边界 | 不改源码、不迁移或重命名文件、不清理 Git 历史、不连接或操作设备、不轮换凭据、不提交、不推送 |
| 发现真实凭据时 | 暂停相关证据迁移，按第 9.4 节独立紧急流程处理，不等待 P0 报告定稿 |

### 14.3 P0 交付物与完成门槛

交付物是一份 Excel 会话附件，不提交仓库。至少包含七个工作表：文档分类、Evidence 索引、模块映射、事实源冲突、敏感信息风险、Contract 差异、历史编号映射。

| 项目 | 完成门槛 | 证据 |
| --- | --- | --- |
| 文件分类 | ① 全部 Git 跟踪文件 100% 已有“动作 + 依据”，未归类数为 0；② 未跟踪未忽略文件已盘点并分类；③ 被忽略文件已盘点并按风险归类；每份文档记录被谁引用 | 文档分类表与未归类计数（分三类报告） |
| Evidence 索引 | 第 9.2 节要求的全部字段；四个历史目录分别统计 | Evidence 索引表 |
| 敏感扫描 | 覆盖全部跟踪文件、未跟踪文件、被忽略文件、全部可见历史和已删除文件；附带第 9.3 节的完整性证明；报告不含秘密原文 | 风险表与完整性记录 |
| 模块映射 | 11 个模块逐项对应源码、现有文档、Contract、需求、测试、历史证据；`operation.c`、`netcheck.c` 等跨模块基础设施提出归属建议 | 模块映射表 |
| Contract 差异 | `commands.json` 与 `k_v1_commands[]` 的差异；生产与各测试构建分开；第 8.1 节五项基线核实 | Contract 差异表 |
| 事实源冲突 | 至少检查：README 与源码、Contract 与源码、不同文档中的固件版本与源码 SHA、需求与任务记录、历史 PASS 与当前基线；每项给出待确认事项 | 冲突清单 |
| 历史编号映射 | 旧编号用“旧编号 + 来源路径 + 章节”唯一定位，并给出建议的新归属 | 映射表 |
| 仓库未被修改 | 原开发工作区与独立克隆均有 P0 开始前/结束后的状态快照；跟踪文件 HEAD 与 status 不变；untracked/ignored 按基线清单逐项比较，证明未被修改或清理 | 前后快照（分工作区与克隆） |
| 结论口径 | 每项区分“已扫描”“已核实”“未解决”，不以“已扫描”代替“已核实” | 报告中的状态列 |

### 14.4 P5 的源码变更与文档影响声明

CI 只报告和阻止不符合门禁的合并，不自动改任何数据：

- `main/` 发生变化时，检查是否同步更新了任务、测试或相关规范。
- 确实没有文档影响时，要求提交说明里有明确声明（例如 `Docs-Impact: none` 并写明理由）。
- CI 不自动编造任务或测试记录，也不自动修改需求状态。

### 14.5 回退机制与回滚演练标准（R60）

每个阶段的全部改动归入独立的提交集，可以整体回退而不影响其他阶段。进入下一阶段前，确认上一阶段的提交集已经通过评审并打上标记，回退以该标记为基准。

**回滚演练最小标准（R68）**：最终验收要求**至少一次**独立测试副本回滚演练；可在 P2 垂直切片或 P4 工具落地后、进入下一阶段前完成（两阶段都做亦可，但最终门槛为一次）。

- 环境：独立测试副本，不对正式仓库做破坏性试验。
- 对象：该阶段提交集（回退到阶段标记）。
- 证明：演练前后 HEAD SHA、文件清单与关键路径哈希、阶段标记是否恢复；其他阶段不受影响。
- 记录：演练报告存入 `docs/history/` 或阶段报告（时间、操作者、前后 SHA、结果）。

## 15. 质量门禁 DOC-G01～G09

九项门禁都要能用命令检查，不能只靠“人工检查没问题”。下表的命令是拟定形态，工具在 P4 实现时定稿；在 P4 之前，对应检查用原型脚本或人工按同样的标准执行，并记录执行方式。

| 编号 | 门禁 | 通过标准 | 检查方法 | 失败条件 |
| --- | --- | --- | --- | --- |
| DOC-G01 | 单一事实源 | 同一事实只有一个权威维护位置 | `python3 tools/docs/validate_docs.py --check refs`：输出 0 个非权威方向引用、 0 个 Spec 中重复定义的 Contract 字段 | 任一项大于 0 |
| DOC-G02 | 状态真实性 | 不伪造 implemented、verified 或 PASS | `python3 tools/docs/validate_docs.py --check state`，并运行第 13 章全部夹具测试 `python3 -m pytest tests/docs -q` | 有需求显示 verified 而不满足第 6 章条件；任一夹具失败 |
| DOC-G03 | 测试有效性 | STALE 按 scope 与 `changed_paths` 精确判定，分组键和“最新一次不回退”成立 | `python3 tools/docs/validate_docs.py --check validity` 与 `tests/docs` 中的有效性夹具 | 夹具结果与第 7 章期望不符 |
| DOC-G04 | Contract 一致性 | 命令差异可自动识别并解释 | `python3 tools/docs/validate_docs.py --check contract` | 存在未解释的源码独有或 Contract 独有命令 |
| DOC-G05 | Evidence 完整性 | RUN 可追溯，哈希可验证 | `python3 tools/docs/validate_docs.py --check evidence`（内部调用 `sha256sum -c`） | 任一 RUN 缺文件或哈希不符 |
| DOC-G06 | 敏感信息安全 | 未经处理的凭据不进入归档 | `python3 tools/docs/scan_sensitive.py --repo . --history`：`--history` 扫历史对象；`--repo` 扫当前工作区并**默认包含 tracked / untracked / ignored**（或显式参数等价）。报告须证明每类文件覆盖数量，不只输出 CLEAN。除 `CLEAN` 外每项有处置结论 | 存在未处置的风险项；或无法证明 ignored/untracked/历史覆盖 |
| DOC-G07 | 历史可追溯 | 历史审批、事故和执行记录保留原貌 | **路径分类清单（R47/R87）**明确两类：`immutable`（如 `evidence/runs/**`、审批/事故/历史报告原文）只允许 R100 与 A；`maintainable`（如 `evidence/manifests/**`、自动生成 INDEX）允许受控修改且可追踪/可重复生成。分类依据为受控清单或固定路径规则，校验器不得猜测 | 不可变路径出现 M/D；可维护路径无追踪；分类未知 |
| DOC-G08 | 变更路径边界 | 阶段内的变更路径完全落在批准清单内 | 正式清单（R59/R72/R82）：`schema_version`、`phase_id`、`baseline_head`、`allowed_paths`、`excluded_paths`（excluded 优先）、`ignored_baseline_manifest`（每项：path、type、size、sha256、collected_at、method；须证明覆盖完整性或附计数）、`approval_ref`（可解析的正式批准记录）、**`approved_by`**、`created_at`、`approved_at`、`phase_started_at`、`phase_started_by`。约束：`approved_at >= created_at` 且 `approved_at < phase_started_at`；时间 RFC3339 UTC；基线与时间禁止事后改写。结束时比对实际变更 | 未批准路径；事后补录；审批主体不可核验；ignored 无完整证明 |
| DOC-G09 | 可读性验收 | 没有读过仓库的 AI 能沿权威引用找到正确答案 | **固定验收条件（R89）**：题库版本 ID、系统提示词全文、输入文件清单、模型标识、答案比对规则（与脚本输出逐题比对）均写入验收记录并可重复执行。输入为 `AGENTS.md` + `docs/INDEX.md` + 声明的只读数据。至少 5 题（事实定位与导航）。不要求仅凭 AGENTS.md 复述动态事实 | 任一题答错；验收条件未固定或不可重复 |

**DOC-G09 题库示例**：CMD-15 现在归哪个模块、状态是什么；某条需求被哪些测试覆盖；命令表的事实源在哪里；当前固件的 OTA 能做什么、不能做什么；某个测试结果是否已经过期。

**每项门禁的验收记录**必须包括：检查方法（命令或执行方式）、实际结果、对应证据、通过或失败的结论、未通过时的处理方式。记录保存在 `evidence/reviews/<阶段>/gates/`。任一门禁失败，该阶段不算通过，不进入下一阶段。

## 16. 实施边界、授权与最终验收

**当前状态**：本方案仅为设计评审。P0 未启动，仓库未做任何修改。确认设计方向不等于仓库修改授权，下一版定稿通过且单独授权后，P0 才会开始。

### 16.1 允许修改的路径

| 路径 | 说明 | 何时开放 |
| --- | --- | --- |
| `README.md`、`AGENTS.md` | 入口与 AI 导航 | 对应阶段授权后 |
| `docs/` | Spec、ADR、History、Runbook | 同上 |
| `evidence/` | 归档证据与评审记录 | 同上 |
| `tools/docs/`、`tests/docs/` | 校验与扫描工具及其夹具 | P2 起 |
| CI workflow | 文档校验接入 | P5 |

### 16.2 默认禁止

`main/`、`sdkconfig*`、`partitions.csv`、协议语义、`contract/` 语义、NVS 与设备状态，在文档重构的任何阶段都不修改。设备操作每次都需要单独的 APR 审批，不在文档重构授权范围内。P0 仅允许在独立分析克隆中执行 Git 读取操作（`clone` / `fetch` / `status` / `diff` / `log` 等）；禁止任何 Git 写操作（`add`、`commit`、`push`、`mv`、`reset`、`rebase`、`filter-repo` 等），写操作均需单独授权（见第 14.2 节）。

仓库卫生问题（已跟踪的 `.venv-bleak`、`build-bluedroid`、`.sdkconfig.*` 等）作为独立事项处理，不并入文档重构。

### 16.3 需要单独决策的事项

- Contract 1.1-draft 是否升级为正式版本。
- 凭据轮换的授权与时机；必须先轮换，再考虑 Git 历史清理。
- Git 历史清理本身（会改写历史，影响所有克隆）。

### 16.4 最终验收标准

1. P0 的 Excel 报告含七张工作表，且不提交到仓库。
2. 旧编号映射按 legacy\_id、source\_path、source\_section 三元组唯一。
3. 文档分类、Evidence 索引、模块映射、事实源冲突、敏感信息风险、Contract 差异均有结论。
4. 固件基线冲突（`1.3-9e1d6c5` 与 `5104a3b`）已有处置结论。
5. DOC-G01～G09 全部通过，并留有验收记录。
6. 每个需求有唯一状态，implemented 与 verified 由脚本派生，而非手写。
7. `tests.yaml` 存在完整的 `target_builds` 登记表；每条 TEST 经 `target` 字段显式引用其适用的目标身份（引用必须存在且符合测试等级）；最新一次运行不回退到更早的 PASS（R43/R48）。
8. 所有 `build.dirty=true` 的 RUN 均有完整构建补丁清单及哈希；所有 `execution.dirty=true` 的 RUN 均有完整执行环境补丁清单及哈希；清单完整性、哈希一致性、可恢复性与 APR 路径范围均通过检查。
9. APR 的执行动作都在批准范围内，且整个执行区间处于有效期内。
10. Contract 与源码的命令差异均已解释，排除项有清单。
11. 全仓库及完整 Git 历史完成敏感扫描，扫描完整性有证明。
12. 公开发布的内容只含 CLEAN 文件。
13. 报告与日志中不出现任何明文凭据。
14. 历史、事故与执行记录原貌保留（G07）。
15. 阶段内变更路径全部在批准清单内（G08）。
16. 按 DOC-G09 规定，向 AI 提供 `AGENTS.md`、`docs/INDEX.md` 和仓库只读数据，完成固定题库测试；答案与脚本输出逐题比对，全部正确。若需额外验证“仅 AGENTS.md 的导航能力”，可单独增设导航规则测试，但不承担动态事实验收（R29）。
17. 每个模块的 Spec 都有元数据，且 `reviewed_against` 指向当前核实的源码提交（固件系列由 `applies_to_firmware` 表达）。
18. INDEX 的矩阵计数与脚本输出一致。
19. 回滚机制已演练一次。
20. P5 起，代码变更必须同步声明 Docs-Impact。
21. 每个阶段都留有进入条件、退出条件与提交边界的记录。
22. 上述各项均有证据位置，可被独立复核。
