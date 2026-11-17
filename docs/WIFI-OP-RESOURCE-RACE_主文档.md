# WIFI-OP-RESOURCE-RACE（owner fencing）主文档

> **状态：ACTIVE（Wi-Fi owner 竞态专项唯一导航与结论入口）**  
> 更新：2026-11-17。本文汇总 B1–B5/C1–C3 设计、fencing 实现复审及 S6 阶段 A/B 的验证边界。实现事实以对应代码提交和最新证据为准。

## 1. 问题与目标

本专项处理 tryconnect、自动连接、取消/过期 cleanup、显式恢复等路径并发操作 STA 驱动时的资源争用。核心原则：

- operation 业务状态不等于 Wi-Fi 驱动资源所有权；
- cleanup 未完成时不得把 owner 释放成 NONE；
- owner 释放/移交必须在同一临界区内完成，避免新操作越过旧 cleanup；
- cleanup 失败进入粘性 `CLEANUP_FAILED`，禁止 AUTO/TRYX 绕过，须通过明确恢复流程清态；
- watchdog 只能请求中止，不得夺取 CLEANUP owner；检查 abort 与失败状态转换必须原子；
- worker/自动连接任务使用调度时捕获的 caller/op_id/generation，不在执行时重新读取可能已变化的上下文。

## 2. 权威设计与实现复审

| 材料 | 当前用途 |
|---|---|
| [B1–B5 修订设计](WIFI-OP-RESOURCE-RACE-修订设计_B1-B5_2026-11-12.md) | owner 六状态、不窗口移交、CLEANUP_FAILED、9 个 STA 驱动调用点、caller 日志等设计口径 |
| [四路径调用关系图](WIFI-OP-RESOURCE-RACE-四路径调用关系图_2026-11-12.md) | TRYX / CANCEL_CLEANUP / AUTO_CONNECT / BOOT/APPLY 路径与 9 个驱动点映射 |
| [fencing 最终复审](WIFI-OP-RESOURCE-RACE-fencing复审材料_2026-11-14.md) | 代码差量 `0eb30ce47795cff5201388793f926cc4302b5dd8` 的逐 hunk 复审；实际父提交 `ebba25c`；主机 owner 测试申报 59/0 |
| [S6-OWNER-STUCK 修复记录](S6-OWNER-STUCK固件修复_2026-11-15.md) | 后续发现的 owner 记账/唤醒失败路径修正与 FI 注入 |
| [S6 阶段 A 回归结果](S6阶段A回归结果_R1R2R3R5_2026-11-15.md) | 功能镜像上 R1/R2/R3/R5 真机结果 |

旧的六问分析、补丁申请、早期实现记录和 fencing 差量记录已归档；它们保留过程证据，不再作为当前独立维护入口。

## 3. 当前实现结论

- B1–B5/C1–C3 的 owner fencing 设计已落地；`CLEANUP` 期间 owner 不可被 AUTO/TRYX 抢占。
- watchdog >25s 的处理是置 abort 请求，owner 保持 CLEANUP；restart cleanup 在安全点消费 abort，并按状态机转为失败态。
- 活动 operation 登记按 operation_id 匹配清除，防止旧 worker 清掉新 operation 的登记。
- 后续 S6-OWNER-STUCK 修复补齐 owner 状态迁移记账，并将 restart wake 失败收敛到 `CLEANUP_FAILED`，避免“清理未运行但 owner 显示空闲”。
- `CLEANUP_FAILED` 是粘滞保护状态；不是普通 transient busy，也不能靠自动连接自行清除。

## 4. 验证状态（必须分层）

| 验证层 | 当前证据 | 状态 |
|---|---|---|
| 纯状态机 host tests | fencing 复审材料记录 `wifi_owner: 59 passed / 0 failed`；S6 修复未改纯状态机 | 历史/申报证据，不能替代设备并发验证 |
| 构建与栈帧 | S6 修复记录列出构建及 `-fstack-usage` 数据 | 用户/开发执行记录；须以当前冻结镜像证据为准 |
| 阶段 A 真机 | 65D8 功能镜像 `a7e04b6a9`，R1/R2/R3/R5 PASS；R6 未测 | 阶段 A 部分范围通过 |
| 阶段 B FI | R4a/R4b/R6 需 FI 镜像 | 当前独立授权申请待批准；未获批不得烧录/操作 |

阶段 A 回归不证明 watchdog 与注入失败路径已通过；阶段 B 申请不等于授权或执行结果。当前最新操作许可见 [S6 阶段 B 申请](S6阶段B操作授权申请_2026-11-16.md)。

## 5. 风险与停止条件

1. 进入 `CLEANUP_FAILED` 后 STA 业务可能保持离线，直到受控复位/显式恢复；该行为是安全保护，不得通过自动抢占绕过。
2. 设备/串口映射、目标镜像字节身份或采集连续性无法确认时，必须 STOP。
3. S6 曾发生端口映射错误和镜像身份漂移，详见 [S6 烧录事故记录](S6烧录事故记录_2026-11-15.md)；后续烧录必须使用新鲜身份映射与冻结 BIN SHA。
4. 不允许把主机测试 PASS、代码复审通过或构建成功写成真机并发 PASS。

## 6. 维护规则

- owner 状态机、锁序、调用点或 fencing 语义变更时，先更新 B1–B5 设计和本主文档，再更新调用图。
- 最新复审与现场证据分别保存；不得把设计签收、代码完成、host PASS、设备 PASS 混为一项。
- S6 的每一轮授权、镜像和设备范围独立管理，不由本专项文档扩大。


## 7. 1.3 当前 Wi-Fi 相关验收边界

### 7.1 候选预扫描失败必须 fail-closed

旧问题 1.3-93 已在当前实现中落地：候选 AP 预扫描失败不再按“扫描为空/继续连接”处理，而是以 `wifi_scan_failed` 终止该路径。该错误已进入当前 Contract。后续若修改扫描、试连或错误映射，必须同时核对 `main/wifi.c`、命令错误映射和 `contract/v1/errors.json`；本条代码存在不等于当前设备回归已通过。

### 7.2 Owner fencing 与 S6 验证状态

- B1–B5/C1–C3 和 S6-OWNER-STUCK 修复属于当前 1.3 Wi-Fi 资源所有权保护，不是历史 Web 功能。
- 阶段 A 的 R1/R2/R3/R5 按执行报告通过；R6 未测。阶段 B 的 R4a/R4b/R6 仍受 FI 镜像、独立授权和设备资源约束，不能标为通过。
- 现有串口抓包进程和端口占用不属于可自动处理范围；任何端口、设备、烧录或复位操作必须另行授权。

### 7.3 已关闭与不再维护的旧问题

旧 v1.2.1-37 的 STA 连接静默问题已由 1.3-08 记录修复，不再作为当前 OPEN 项。旧 Wi-Fi 网页连接向导（1.3-51）依赖已移除的 Web UI，不属于当前 1.3 产品功能或验收范围。

### 7.4 静态 IP 启动安全：M1 实现、M2 缺口与 M4 边界

- **M1 核心已进入当前源码**：`main/wifi.c` 已包含 `app_wifi_boot_apply_or_recover()`、`app_wifi_recover_from_apply_failure()`、RAM LKG、DHCP rescue、offline gate/quiesce 等路径。早期 M1 PR 草案中“未实施/待合并”的状态已过时；对应历史测试矩阵仍可用于追踪覆盖面，但不能据此宣称 M1-01～M1-10 全部设备验收 PASS。
- **M2 静态参数语义校验仍有缺口**：当前 `main/config_apply.c` 的 `cfg_validate_static_ip_cross()` 检查 IP/网关/掩码字段存在、IPv4 格式合法且非 0.0.0.0、DNS 非空时格式合法；尚未检查掩码连续性、IP 与网关同子网、网络/广播地址、IP 与网关相同等约束。非法组合可能通过当前交叉校验，需单独评审并补充测试。
- **M4 持久化两阶段提交/回滚尚未实现**：当前 RAM LKG 与 DHCP rescue 是运行期启动恢复，不等于 NVS VALID/PENDING 两段提交；救援不会自动修正持久化静态配置，后续启动仍可能再次进入救援路径。M4 是否纳入 1.3 交付范围须单独决策，不在本次文档整理中扩展实现。
- M1 板级故障注入和恢复覆盖以 [静态 reapply P0 设备验收报告](archive/2026-11/静态reapply_P0设备验收报告_2026-11-05.md) 与 [P1 设备验收报告](archive/2026-11/静态reapply_P1设备验收报告_2026-11-05.md) 的原始镜像、设备和场景为边界；源码存在、历史局部 PASS 或计划矩阵均不等于完整 M1 验收通过。旧 M0/M1 取证与设计材料保留为启动安全决策和历史证据。

### 7.5 N03/N07 时序注入缺口

旧 MQTT v1 真机回归的 38/40、0 FAIL、2 SKIP 中，N03/N07 因静态 IP 锁条件按预期跳过；底层路径有当时真机证据，但 harness 尚不能完成对应时序注入闭环。此项属于低优先级测试工具能力缺口，不得改写为产品功能 FAIL，也不得把 SKIP 计为 PASS。重新执行须按当前镜像、设备和授权建立新记录。

### 7.6 静态 IP 配置剩余运行时语义与可观察性

本节从当前 `main/config_apply.c`、`main/wifi.c`、`main/cmd.c` 复核，替代 2026-11-02 旧专项清单中已过时的 v0 接口描述。以下是代码事实与待验证边界，不代表设备验收结果。

| 项目 | 当前源码事实 | 结论 |
|---|---|---|
| 静态字段空串 | `sta_static_ip/gateway/netmask/dns1/dns2` 字段仍设置 `empty_keeps=true`；空串在 apply 时保留旧值 | 不能用空串清除单个静态字段；尚无对应的字段级清除语义 |
| DHCP client 停止 | `app_configure_sta_static_ip()` 对 `esp_netif_dhcpc_stop()` 的失败（除 ALREADY_STOPPED）会中止静态应用并进入 M1 恢复决策 | 旧清单“只告警继续”已过时 |
| DNS 设置 | 非空 DNS 才调用 `esp_netif_set_dns_info()`，调用返回值未检查；空 DNS 不显式清除 MAIN/BACKUP | 设置失败未形成明确错误路径；DNS 空值/静态↔DHCP 切换残留仍需专项验证 |
| 静态 IP 冲突检测 | GOT_IP 后启动 ARP 探测；内部结果只有 bool。若读取自身 MAC 失败但 ARP 有应答，当前代码仍置 conflict=true | 无法区分真实冲突与检测不完整，需引入 unknown/clear/conflict 语义 |
| 冲突状态输出 | 当前 `config.get` 输出持久化静态配置，但未输出 `sta_ip_conflict`；没有可供客户端判断检测结果的三态字段 | 旧注释所称“状态接口透出”与实际输出不符 |
| 静态模式 tryconnect | `app_cmd_static_switch_blocked()` 已拒绝静态模式下切换到不同 SSID 或请求 DHCP trial（返回 `static_ip_pinned`） | 旧清单中“缺少 tryconnect 互锁”已由当前实现覆盖 |

**仍需独立验证/评审：**

1. DNS MAIN/BACKUP 的空值清理策略，以及静态↔DHCP 切换后实际 DNS 是否残留；不能仅由当前“非空才设置”的代码推断具体 netif 行为。
2. `esp_netif_set_dns_info()` 失败时是否应中止配置、进入 M1 恢复，还是允许降级；需要明确产品语义并测试。
3. ARP 冲突检测应区分检测未完成、无冲突和确认冲突；自身 MAC 获取失败不得直接等价于确认冲突。
4. 冲突结果是否进入只读状态接口，以及客户端提示/恢复动作如何定义，需单独评审。
5. 静态字段清除应通过明确的专用清除字段/操作定义，不应复用空串的“保持原值”语义。

以上属于 M2/运行时可观察性和配置语义的剩余审查范围；本轮只迁移证据，不修改源码、Contract 或设备状态。
