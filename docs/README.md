# docs/ 文档索引与状态标定

> 标定日期：2026-11-14。**新文档必须在首条状态行声明状态**；改动语义时同步更新本索引。


## 状态定义

| 状态 | 含义 |
| --- | --- |
| **ACTIVE** | 当前有效：现行设计/规范/证据/在续文档 |
| **LEGACY** | 历史兼容：描述**已删除功能**（如 HTTP/Web）或已过时现状的文档——只作背景阅读，不得作为行为依据 |
| **ARCHIVED** | 历史记录：已实现设计的记录、已闭环缺陷、历史验收证据 |
| **PLANNED** | 尚未实现/未执行的计划 |

特殊：`contract/v1/*.json` 的状态以文件内 `contract_version`/`status` 字段为准（冻结基线，不在本索引展开）。

## ACTIVE（16）

- [A_token_store_统一鉴权_2026-10-26.md](A_token_store_统一鉴权_2026-10-26.md)——token 设计现状
- [BLE命令协议规范.md](BLE命令协议规范.md)——v1 wire 协议规范；语义以 contract/v1 为准
- [BLE常态化韧性验收_T5_2026-10-24.md](BLE常态化韧性验收_T5_2026-10-24.md)——T5 方案冻结版；soak 执行证据见 soak_6h 报告
- [BLE微信小程序交互协议_T4_2026-10-24.md](BLE微信小程序交互协议_T4_2026-10-24.md)——T4 交互协议；Web/HTTP 章节为 LEGACY
- [R-NET设备验收报告_2026-11-07.md](R-NET设备验收报告_2026-11-07.md)——在续写的总证据文档；§1-§17 按节归档
- [CMD-04b_network_status快照清单与判定表_2026-11-14.md](CMD-04b_network_status快照清单与判定表_2026-11-14.md)——network_status 四段快照化设计（已签收）
- [CMD-04c_execute_method内联快照清单与判定表_2026-11-14.md](CMD-04c_execute_method内联快照清单与判定表_2026-11-14.md)——execute_method 两段内联快照设计（已签收；段 A wire 验收重定界为不可达（v1 分发死代码），段 B 经 async apply 保持有效）
- [S6预采集解冻申请_2026-11-14.md](S6预采集解冻申请_2026-11-14.md)——密码分支四项补采解冻申请 v11（设计过审 2415983；执行口径唯一锁定）
- [S6执行授权与窗口申请_2026-11-14.md](S6执行授权与窗口申请_2026-11-14.md)——S6 现场执行操作授权申请 v2（§4 正常/异常双结束状态；uptime 证据链+双 console 单入口两项强制核验），待批
- [S6第二轮窄范围执行授权申请_2026-11-14.md](S6第二轮窄范围执行授权申请_2026-11-14.md)——仅 MQTT async expired[须 GOT_IP]+op-V（脚本 v3：九项意见收口——改指向状态机/快照恢复/cleanup 窗口/锚点恢复/台账≤3），待批
- [CMD-04系列_合并烧录窗口申请_2026-11-14.md](CMD-04系列_合并烧录窗口申请_2026-11-14.md)——fencing+04a/b/c 四项合并烧录申请（pre-golden 预采集→烧录→diff）
- [R_数据路由层架构_2026-10-26.md](R_数据路由层架构_2026-10-26.md)——data_pipe 架构现状
- [WIFI-OP-RESOURCE-RACE-fencing实现记录_2026-11-13.md](WIFI-OP-RESOURCE-RACE-fencing实现记录_2026-11-13.md)——fencing 已签收（终审通过+裁决(b)修正 ae8a3abb5）
- [WIFI-OP-RESOURCE-RACE-fencing复审材料_2026-11-14.md](WIFI-OP-RESOURCE-RACE-fencing复审材料_2026-11-14.md)——0eb30ce 复审包（终审通过；裁决(b)修正已合入，见 §5 构建证据）
- [WIFI-OP-RESOURCE-RACE-修订设计_B1-B5_2026-11-12.md](WIFI-OP-RESOURCE-RACE-修订设计_B1-B5_2026-11-12.md)——设计已签收；§14/§15.1 为唯一口径
- [WIFI-OP-RESOURCE-RACE-四路径调用关系图_2026-11-12.md](WIFI-OP-RESOURCE-RACE-四路径调用关系图_2026-11-12.md)——与修订设计同步
- [WIFI-OP-RESOURCE-RACE-补丁实现记录_2026-11-12.md](WIFI-OP-RESOURCE-RACE-补丁实现记录_2026-11-12.md)——185e029 实现记录
- [WIFI-OP-RESOURCE-RACE-补丁申请_2026-11-12.md](WIFI-OP-RESOURCE-RACE-补丁申请_2026-11-12.md)——含 R2 验收项；fencing 代码已另提交
- [协议契约整改方案_2026-11-10.md](协议契约整改方案_2026-11-10.md)——契约 v1.1 基线 + Phase 跟踪
- [小程序代码评审_2026-11-10.md](小程序代码评审_2026-11-10.md)——P1 已落；P2/P3 待客户端消化
- [小程序测试配合评审_2026-11-08.md](小程序测试配合评审_2026-11-08.md)——建议项部分待消化
- [待排期清单_2026-11-05.md](待排期清单_2026-11-05.md)——backlog
- [测试Broker迁移云主机_2026-11-06.md](测试Broker迁移云主机_2026-11-06.md)——现行测试 broker 139.9.93.245

## LEGACY（2）

- [BLE功能现状与目标.md](BLE功能现状与目标.md)——现状描述过时，目标已演进
- [soak_test_plan.md](soak_test_plan.md)——HTTP/Web 章节描述已删除功能；T5 BLE/soak 章节历史有效

## PLANNED（2）

- [T5_V2_EN硬复位测试计划_r2.2.md](T5_V2_EN硬复位测试计划_r2.2.md)——硬复位轮未执行
- [wifi分层重构_设计草案_2026-11-13.md](wifi分层重构_设计草案_2026-11-13.md)——wifi.c 四层重构（前置：RACE 补丁真机验证 + T4 签收）

## ARCHIVED（47）

- [BLE设备侧要求检查表.md](BLE设备侧要求检查表.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [BLE需求基线.md](BLE需求基线.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [DEF-BLE-001_A方案实现记录_2026-11-05.md](DEF-BLE-001_A方案实现记录_2026-11-05.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [DEF-BLE-001_A方案设备验收报告_2026-11-05.md](DEF-BLE-001_A方案设备验收报告_2026-11-05.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [DEF-BLE-001_取证结果_H1确认_2026-11-05.md](DEF-BLE-001_取证结果_H1确认_2026-11-05.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [DEF-MQTT-003_实现记录_2026-11-05.md](DEF-MQTT-003_实现记录_2026-11-05.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [DEF-MQTT-003_设备验收报告_2026-11-05.md](DEF-MQTT-003_设备验收报告_2026-11-05.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [DEF-SCAN-001_立案_2026-11-06.md](DEF-SCAN-001_立案_2026-11-06.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [DEF-SCAN-001_节流实现记录_2026-11-06.md](DEF-SCAN-001_节流实现记录_2026-11-06.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [DEF-SCAN-001_设备验收报告_2026-11-06.md](DEF-SCAN-001_设备验收报告_2026-11-06.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [DEF-SCAN-002_立案_2026-11-08.md](DEF-SCAN-002_立案_2026-11-08.md)——已修复：scan_time 0/0
- [E1_E3_验收报告_2026-11-05.md](E1_E3_验收报告_2026-11-05.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [G0验收记录.md](G0验收记录.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [G2验收记录.md](G2验收记录.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [G3操作契约冻结.md](G3操作契约冻结.md)——操作契约以 contract/v1 为准
- [MQTT_v1_真机回归报告_2026-11-05.md](MQTT_v1_真机回归报告_2026-11-05.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [MQTT受理回包竞态_实现记录_2026-11-05.md](MQTT受理回包竞态_实现记录_2026-11-05.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [MQTT受理回包竞态_设备验收报告_r2_2026-11-05.md](MQTT受理回包竞态_设备验收报告_r2_2026-11-05.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [MQTT网络配置控制_阶段0实现记录_2026-11-03.md](MQTT网络配置控制_阶段0实现记录_2026-11-03.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [MQTT网络配置控制_阶段0实现设计_2026-11-03.md](MQTT网络配置控制_阶段0实现设计_2026-11-03.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [MQTT网络配置控制方案评审回复_2026-11-03.md](MQTT网络配置控制方案评审回复_2026-11-03.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [P0_BLE验收记录_2026-10-23.md](P0_BLE验收记录_2026-10-23.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [T3_BLE数据通道验收_2026-10-23.md](T3_BLE数据通道验收_2026-10-23.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [T4_A_Android微信验收_2026-10-26.md](T4_A_Android微信验收_2026-10-26.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [T4_验收失败分析_2026-11-04.md](T4_验收失败分析_2026-11-04.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [T5_V1_RTC保留验收报告_2026-11-03.md](T5_V1_RTC保留验收报告_2026-11-03.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [WIFI-OP-RESOURCE-RACE-六问回答与最小设计_2026-11-12.md](WIFI-OP-RESOURCE-RACE-六问回答与最小设计_2026-11-12.md)——已被修订设计 B1-B5 取代
- [netcheck_P0只读核查_2026-11-07.md](netcheck_P0只读核查_2026-11-07.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [soak_6h_验收报告_2026-11-06.md](soak_6h_验收报告_2026-11-06.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [v0彻底删除_实现记录_2026-11-05.md](v0彻底删除_实现记录_2026-11-05.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [v0彻底删除_设备验收报告_2026-11-05.md](v0彻底删除_设备验收报告_2026-11-05.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [v1_only_文档索引_2026-11-04.md](v1_only_文档索引_2026-11-04.md)——索引被 docs/README.md 取代
- [v1_only_迁移进度_2026-11-04.md](v1_only_迁移进度_2026-11-04.md)——迁移已完成
- [命令与配置依赖清单.md](命令与配置依赖清单.md)——v1-only 迁移期核验，以 contract/v1 为准
- [命令与配置源码核验.md](命令与配置源码核验.md)——同上
- [缺陷立案_DEF-BLE-001_连接句柄悬挂_2026-11-05.md](缺陷立案_DEF-BLE-001_连接句柄悬挂_2026-11-05.md)——已修复闭环
- [网络配置切换方案评审备忘_2026-11-03.md](网络配置切换方案评审备忘_2026-11-03.md)——由 DEF-NET 实现
- [联网验证netcheck_实现记录_2026-11-07.md](联网验证netcheck_实现记录_2026-11-07.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [联网验证netcheck_设计稿v2_2026-11-07.md](联网验证netcheck_设计稿v2_2026-11-07.md)——已实现，证据见 R-NET 报告
- [阶段1网络与重连排查记录.md](阶段1网络与重连排查记录.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [阶段2设计冻结.md](阶段2设计冻结.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [静态IP_M1启动安全设计稿_2026-11-03.md](静态IP_M1启动安全设计稿_2026-11-03.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [静态IP_M1实现PR边界与测试计划_2026-11-03.md](静态IP_M1实现PR边界与测试计划_2026-11-03.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [静态reapply_P0实现记录_2026-11-05.md](静态reapply_P0实现记录_2026-11-05.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [静态reapply_P0设备验收报告_2026-11-05.md](静态reapply_P0设备验收报告_2026-11-05.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [静态reapply_P1状态探测_实现记录_2026-11-05.md](静态reapply_P1状态探测_实现记录_2026-11-05.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)
- [静态reapply_P1设备验收报告_2026-11-05.md](静态reapply_P1设备验收报告_2026-11-05.md)——文档状态标定 2026-11-14，定义与索引见 [docs/README.md](README.md)

## 代码侧遗留命名清点（LEGACY 命名 / ACTIVE 语义，2026-11-14 核验）

| 项 | 位置 | 事实 | 处置建议 |
| --- | --- | --- | --- |
| `WEB_DEFAULT_USERNAME/PASSWORD`（Kconfig） | `main/Kconfig.projbuild:3,11` | **不是死代码**：`auth.c` 用 `s_config.web_username/web_pass_hash` 做 BLE/MQTT 管理员凭据校验；`config.c` 以 Kconfig 值为默认值写入凭据存储 | **保留**。WEB_ 前缀为删除 Web 时代的遗留命名；语义=管理员凭据默认值。重命名 = NVS/config_apply 字段迁移，需单独评审 |
| `web_username/web_pass_hash`（config 结构 + NVS 字段） | `config.h:51,53`、`config_apply.c:528`、`auth.c:23-25`、`cmd.c:2197` | 同上：**活的凭据存储**（auth.login 校验对象） | 不删不改名；建议后续在字段注释加"遗留命名，实为管理员凭据"（代码变更需授权） |
| `APP_CMD_SOURCE_HTTP` / `APP_IO_SOURCE_HTTP` 枚举与 policy 分支 | `cmd_context.h:26`、`cmd.c:44/823/1827/2245`、`cmd_policy.c:8/22/97/145`、`state.c:80` | 无传输通道再产生该来源（HTTP 服务已删），枚举/owner/policy 守卫保留（`cmd_context.h:26` 注明"枚举保留"） | 保留（防御式；删除=纯清理需授权） |
| `docs/soak_test_plan.md` HTTP/Web 章节 | docs | 描述已删除功能（该文档已标 LEGACY） | 如需拆分 ACTIVE 章节另立文档，单独做 |

## 维护规则

1. 新增文档：首条状态行 + 本索引登记；
2. 状态变化：改状态行 + 本索引；
3. 描述已删除功能的章节：所在文档标 LEGACY，或拆出并在原处留指针；
4. 本索引由脚本半自动维护（状态源 = 各文档首条状态行）。
