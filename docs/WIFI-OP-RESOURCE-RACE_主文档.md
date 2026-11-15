# WIFI-OP-RESOURCE-RACE（owner fencing）主文档

> **状态：ACTIVE（Wi-Fi owner 竞态专项唯一导航与结论入口）**  
> 更新：2026-11-16。本文汇总 B1–B5/C1–C3 设计、fencing 实现复审及 S6 阶段 A/B 的验证边界。实现事实以对应代码提交和最新证据为准。

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
