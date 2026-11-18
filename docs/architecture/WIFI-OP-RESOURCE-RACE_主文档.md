# Wi-Fi 网络与 owner fencing

> **状态：ACTIVE（当前 Wi-Fi 架构、限制与验收入口）**
> 更新：2026-11-18。产品行为以当前源码和 `contract/v1/` 为准；设备验收结论必须限定到对应设备、镜像和报告。

## 1. 范围与核心原则

本专题维护 STA 连接、网络操作并发、取消/过期恢复、自动连接和静态 IP 启动安全。

- operation 状态描述业务生命周期，不代表 Wi-Fi 驱动资源已释放。
- 任一时刻只能由当前 owner 发起受保护的 STA 状态变更操作。
- cleanup 未完成不得释放 owner；交接必须原子完成，不允许经过可被新操作抢占的 NONE 窗口。
- cleanup 失败进入粘性 `CLEANUP_FAILED`，AUTO/TRYX 不得绕过。
- watchdog 只能请求中止；不能在旧驱动调用仍可能执行时夺取 CLEANUP owner。
- 异步 worker 使用调度/受理时捕获的 operation 身份，不得执行时重新读取可能变化的上下文。

## 2. 当前 owner 模型

owner 状态：

| 状态 | 用途 |
|---|---|
| `NONE` | 当前无 STA 操作所有者 |
| `TRYX` | 显式 tryconnect 生命周期 |
| `AUTO` | 后台自动连接 |
| `APPLY` | 启动或显式配置应用 |
| `CLEANUP` | cancel/expire 后的恢复链；包含配置恢复全过程 |
| `CLEANUP_FAILED` | cleanup 无法可靠完成的粘性保护状态 |

关键规则：

1. owner 由统一仲裁锁保护；TRYX/AUTO/APPLY 释放时若已有 cleanup 请求，直接移交 CLEANUP。
2. cleanup 在 owner 空闲时直接取得 CLEANUP；owner 忙时登记请求，等待当前持有者在释放点移交。
3. CLEANUP 持有期间完成恢复配置，不在中途释放后重新 claim APPLY。
4. CLEANUP 持有期间完成恢复配置，不在中途释放后重新 claim APPLY；最多执行 3 次，失败退避为 0.5s / 1s / 2s。
5. watchdog 在 CLEANUP 超过 12s 时重通知；超过 25s 只置 abort 请求，不夺取 owner。restart 任务仅在 `apply_cfg()` 已返回、没有在飞驱动调用的安全点消费 abort。
6. cleanup 三次失败、watchdog 安全中止或 cleanup 唤醒失败，均不得伪装成 NONE，必须进入 `CLEANUP_FAILED`。
7. `CLEANUP_FAILED` 阻止 AUTO/TRYX 发起 STA 操作；TRYX 返回 `cleanup_failed`。仅显式 `config.set`（effect=reboot）后重启或受控物理重启可清除，不得由后台自动连接清除。
8. 活动 operation 登记按 operation_id 匹配清除，旧 worker 不得清掉新 operation 的登记。异步任务使用调度/受理时捕获的 caller、op_id、generation，不得在执行时重读变化后的上下文。

## 3. 并发路径与保护边界

当前需要按 owner 约束的路径：

- **TRYX**：预扫描、候选连接、取消/完成时断开候选并恢复原配置；完成驱动收尾后才释放或移交。
- **CLEANUP**：cancel/expire 请求登记后，由 restart cleanup 任务持有 CLEANUP 完成恢复；operation 进入终态不等于资源已释放。
- **AUTO**：后台自动连接非阻塞 claim；owner 不可用时让出并等待 monitor 后续调度，不得阻塞 Wi-Fi 事件任务。
- **APPLY**：启动配置应用持有 APPLY；cleanup 内部的 apply_cfg 仍受外层 CLEANUP 保护。
- **事件/检测旁路**：任何直接触发 connect/disconnect/scan 等状态变更的旁路，都必须遵守 owner 仲裁；不得因其运行在事件任务而绕过所有权。

修改上述任一路径、STA 驱动调用点、owner 状态迁移、锁序或异步任务生命周期时，必须重新检查所有调用点的 owner 覆盖、失败出口和释放后的剩余栈路径。旧调用图使用固件 1.3-2ef2a38 行号，已不作为当前代码导航。

## 4. 当前实现与风险结论

- B1–B5/C1–C3 的核心 fencing 规则已进入实现：释放即交接、CLEANUP_FAILED 粘性保护、AUTO 不得抢占 cleanup、worker 使用捕获的 caller/op_id/generation。
- watchdog 超时只置 abort 请求，owner 保持 CLEANUP；restart cleanup 在无在飞驱动调用的安全点消费 abort。
- S6-OWNER-STUCK 修复统一了 owner 状态迁移后的 cleanup 记账，补齐 restart kind/started 时间；cleanup 唤醒失败转入 `CLEANUP_FAILED`，不再回退 NONE；restart 任务保留状态不变量兜底。
- 纯状态机 host tests 曾由执行方报告 59 passed / 0 failed；该结果只证明相应状态机断言，不代替 ESP-IDF 构建或设备并发验收。
- 不得把代码复审、host tests、构建成功写成真机并发 PASS。

## 5. S6 现场验收状态

- **阶段 A**：设备 65D8，功能镜像 build_sha `a7e04b6a9`，BIN SHA-256 `047e4726455db505a5433bd3cb90a94022144add78361d6d8a52548e79f0ea41`；R1/R2/R3/R5 PASS，R6 未测。结论只适用于该设备、镜像和报告。
- **阶段 B**：FI 镜像 R4a/R4b/R6 尚未执行；当前为待批准申请，不是授权。须按申请文件重新核验设备、镜像、端口、boot 会话和采集链。
- 曾发生端口映射误烧和镜像身份漂移；烧录前后必须以设备身份、完整 BIN SHA-256、build_sha 反证和连续采集证据构成门禁。
- 任一身份 UNKNOWN、端口/镜像不符或证据链不完整，立即 STOP。申请不等于批准，旧授权不延续。

必要证据入口：
- [S6 当前状态与现场操作边界](../validation/S6_现场验收总览.md)
- [阶段 A R1/R2/R3/R5 真机结果](../reports/S6阶段A回归结果_R1R2R3R5_2026-11-15.md)
- [阶段 B FI 操作授权申请](../validation/S6阶段B操作授权申请_2026-11-16.md)
- [阶段 A 烧录事故与身份门禁](../reports/S6烧录事故记录_2026-11-15.md)
- [阶段 A 分区表证据](../evidence/S6_stageA/README.md)
- [阶段 B 镜像 manifest](../evidence/S6_stageB/images_manifest.md)
- [阶段 B 六维身份采集边界](../evidence/S6_stageB/identity_2026-11-15/README.md)

## 6. 1.3 当前 Wi-Fi 相关验收边界

### 6.1 候选预扫描失败必须 fail-closed

旧问题 1.3-93 已在当前实现中落地：候选 AP 预扫描失败不再按“扫描为空/继续连接”处理，而是以 `wifi_scan_failed` 终止该路径。该错误已进入当前 Contract。后续若修改扫描、试连或错误映射，必须同时核对 `main/wifi.c`、命令错误映射和 `contract/v1/errors.json`；代码存在不等于当前设备回归已通过。

### 6.2 已关闭与不再维护的旧问题

旧 v1.2.1-37 的 STA 连接静默问题已由 1.3-08 记录修复，不再作为当前 OPEN 项。旧 Wi-Fi 网页连接向导（1.3-51）依赖已移除的 Web UI，不属于当前 1.3 产品功能或验收范围。

### 6.3 静态 IP 启动安全：M1 实现、M2 缺口与 M4 边界

- **M1 核心已进入当前源码**：`main/wifi.c` 已包含 `app_wifi_boot_apply_or_recover()`、`app_wifi_recover_from_apply_failure()`、RAM LKG、DHCP rescue、offline gate/quiesce 等路径。早期 M1 PR 草案中“未实施/待合并”的状态已过时；历史测试矩阵可用于追踪覆盖面，但不能据此宣称 M1-01～M1-10 全部设备验收 PASS。
- **M2 静态参数语义校验仍有缺口**：当前 `main/config_apply.c` 的 `cfg_validate_static_ip_cross()` 检查 IP/网关/掩码字段存在、IPv4 格式合法且非 0.0.0.0、DNS 非空时格式合法；尚未检查掩码连续性、IP 与网关同子网、网络/广播地址、IP 与网关相同等约束。非法组合可能通过当前交叉校验，需单独评审并补充测试。
- **M4 持久化两阶段提交/回滚尚未实现**：当前 RAM LKG 与 DHCP rescue 是运行期启动恢复，不等于 NVS VALID/PENDING 两段提交；救援不会自动修正持久化静态配置，后续启动仍可能再次进入救援路径。M4 是否纳入 1.3 交付范围须单独决策，不在本次文档整理中扩展实现。
- M1 板级故障注入和恢复覆盖以 [静态 reapply P0 设备验收报告](../archive/2026-11/静态reapply_P0设备验收报告_2026-11-05.md) 与 [P1 设备验收报告](../archive/2026-11/静态reapply_P1设备验收报告_2026-11-05.md) 的原始镜像、设备和场景为边界；源码存在、历史局部 PASS 或计划矩阵均不等于完整 M1 验收通过。

### 6.4 N03/N07 时序注入缺口

旧 MQTT v1 真机回归的 38/40、0 FAIL、2 SKIP 中，N03/N07 因静态 IP 锁条件按预期跳过；底层路径有当时真机证据，但 harness 尚不能完成对应时序注入闭环。此项属于低优先级测试工具能力缺口，不得改写为产品功能 FAIL，也不得把 SKIP 计为 PASS。重新执行须按当前镜像、设备和授权建立新记录。

### 6.5 静态 IP 配置剩余运行时语义与可观察性

本节是当前源码事实与待验证边界，不代表设备验收结果。

| 项目 | 当前源码事实 | 结论 |
|---|---|---|
| 静态字段空串 | `sta_static_ip/gateway/netmask/dns1/dns2` 字段仍设置 `empty_keeps=true`；空串在 apply 时保留旧值 | 不能用空串清除单个静态字段；尚无字段级清除语义 |
| DHCP client 停止 | `app_configure_sta_static_ip()` 对 `esp_netif_dhcpc_stop()` 的失败（除 ALREADY_STOPPED）会中止静态应用并进入 M1 恢复决策 | 旧清单“只告警继续”已过时 |
| DNS 设置 | 非空 DNS 才调用 `esp_netif_set_dns_info()`，调用返回值未检查；空 DNS 不显式清除 MAIN/BACKUP | 设置失败未形成明确错误路径；DNS 空值/静态↔DHCP 切换残留仍需专项验证 |
| 静态 IP 冲突检测 | GOT_IP 后启动 ARP 探测；内部结果只有 bool。若读取自身 MAC 失败但 ARP 有应答，当前代码仍置 conflict=true | 无法区分真实冲突与检测不完整，需引入 unknown/clear/conflict 语义 |
| 冲突状态输出 | 当前 `config.get` 输出持久化静态配置，但未输出 `sta_ip_conflict`；没有可供客户端判断检测结果的三态字段 | 旧注释所称“状态接口透出”与实际输出不符 |
| 静态模式 tryconnect | `app_cmd_static_switch_blocked()` 已拒绝静态模式下切换到不同 SSID 或请求 DHCP trial（返回 `static_ip_pinned`） | 旧清单中“缺少 tryconnect 互锁”已由当前实现覆盖 |

仍需独立验证/评审：

1. DNS MAIN/BACKUP 的空值清理策略，以及静态↔DHCP 切换后实际 DNS 是否残留；不能仅由当前“非空才设置”的代码推断具体 netif 行为。
2. `esp_netif_set_dns_info()` 失败时是否应中止配置、进入 M1 恢复，还是允许降级；需要明确产品语义并测试。
3. ARP 冲突检测应区分检测未完成、无冲突和确认冲突；自身 MAC 获取失败不得直接等价于确认冲突。
4. 冲突结果是否进入只读状态接口，以及客户端提示/恢复动作如何定义，需单独评审。
5. 静态字段清除应通过明确的专用清除字段/操作定义，不应复用空串的“保持原值”语义。

以上属于 M2/运行时可观察性和配置语义的剩余审查范围；本轮只迁移证据，不修改源码、Contract 或设备状态。
