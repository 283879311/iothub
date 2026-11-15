> 归档说明（2026-11-16）：旧申请目标 C8E4/旧镜像，后续因事故和目标变更失效。 当前 S6 状态见 [S6 现场验收总览](../../S6_现场验收总览.md)。历史授权与执行记录只适用于原文指定的设备/镜像/窗口，不构成当前许可。

---

# S6-OWNER-STUCK · 一次性烧录授权申请（2026-11-15）

> **状态**：**待批准**（申请烧录与设备重启；获批前不进行任何设备操作）
> 依据：核心修复与 FI 注入逻辑已**代码签收**（`856c669`）；R4a/R4b 执行设计
> 通过，尚未设备实测。**第五轮测试授权不覆盖烧录。**

## 1. 目标与镜像

| 阶段 | 镜像 | build_sha | 用途 |
| --- | --- | --- | --- |
| 阶段 A | 功能镜像（`APP_WIFI_FAULT_INJECT=OFF`） | **`95315d5ae46b`** | 回归 R1 / R2 / R3 / R5 / R6 |
| 阶段 B | FI 镜像（`-DAPP_WIFI_FAULT_INJECT=ON`） | **`a805202f2b07`** | 回归 **R4a**；复位后 **R4b** |

- 目标设备：**C8E4**（`B0:A7:32:28:C8:E6`）；65D8 **不动**；
- 烧录方式：`idf.py -B <build> -p <port> flash`（整片 app 分区），串口双 console 全程在录；
- 代码基线：`856c669`（含 S6-OWNER-STUCK 修复与 FI 注入闭环）。

## 2. 分阶段流程（每阶段独立验收、独立停止）

### 阶段 A · 功能镜像

1. 烧录 `95315d5ae46b` → 重启（`rst` 后重建 uptime 基线）；
2. 冒烟：`device.info.build_sha == 95315d5ae46b`；`network.status` 正常；`mqtt_connected=true`；
3. 重建基线：uptime 起点、连接 SSID/IP、owner 初始状态（console `op_owner` 首次转移前应为无 CLEANUP）；
4. 执行回归：
   - **R1** TRYX 持有期间 cleanup 请求 → 正常释放：期望 `TRYX -> CLEANUP` + `cleanup complete` + owner→NONE；
   - **R2** 同场景但 **BLE 断连触发取消**（本次故障复现路径）：期望同上，**不得**粘滞；
   - **R3** restart 正常执行：cleanup 有明确终态，owner 不停 CLEANUP；
   - **R5** 无 cleanup 请求时 TRYX 释放：owner→NONE，且**无** cleanup 日志；
   - **R6** watchdog：强制 cleanup 超时（可用 FI 镜像补测；功能镜像仅观察 12s 重通知是否存在）。
5. 阶段 A 全部通过才进入阶段 B；任一项失败立即 STOP、保留镜像与日志、回退到阶段 A 之前状态（见 §4）。

### 阶段 B · FI 镜像（R4）

1. 烧录 `a805202f2b07` → 重启；
2. 前置：`diag.m1hook {action:"status"}` 显示 `restart_wake_fail=0`；owner=NONE；无活动 op；
3. **R4a**：布防 `restart_wake_fail=1` → 发起 tryconnect → 期间 `network.cancel`（释放时 `cleanup_requested=true`）
   → 期望 `hook: restart_wake forced fail (R4 injection, cleanup path)` +
   `cleanup wake failed -> CLEANUP_FAILED op=… by tryconnect_release`；owner=`CLEANUP_FAILED`；
   `status` 计数归 0；
4. **复位设备**（R4a 后 CLEANUP_FAILED 粘性，必须清态）；复位后核对 owner=NONE、无活动 op、`restart_wake_fail=0`；
5. **R4b**：布防 `restart_wake_fail=1` → 产生 GOT_IP trial（tryconnect 成功至 got_ip）→ `network.cancel` 触发
   `cleanup_request` NOTIFY → 期望 `cleanup wake failed -> CLEANUP_FAILED op=… by cleanup_request`；
   `app_wifi_cleanup_request` 返回 false；owner=`CLEANUP_FAILED`，**不得**回 NONE；
6. 阶段 B 结束后：`diag.m1hook {action:"clear"}` 确认 `restart_wake_fail=0`；**建议烧回功能镜像**（FI 镜像仅用于回归）。

## 3. 验收判据（逐项留证）

| 项 | 判据 |
| --- | --- |
| 镜像身份 | `device.info.build_sha` 与目标 sha 一致 |
| R1/R2/R3/R5/R6 | 见 §2 各条；console 日志 + `op_owner` 转移链完整留证 |
| R4a/R4b | 各含注入、失败分类、`CLEANUP_FAILED` 终态三要素；两次结果**分别归档** |
| 无假恢复 | 任何失败路径**不得**出现"owner=NONE 但清理未执行" |
| 录制 | 双 console 连续录制；执行前后记录字节增量 |

## 4. 风险与回退

| 风险 | 处置 |
| --- | --- |
| 烧录后无法启动 | 保留当前镜像备份；`idf.py flash` 重烧；必要时 `erase-flash` 后烧回旧镜像（**需另行授权**） |
| 回归触发 `CLEANUP_FAILED` 粘性 | 单次复位清态；若复位后仍失败 → STOP 上报 |
| FI 镜像行为差异 | FI 未布防时行为等价功能镜像；回归结束烧回功能镜像 |

## 5. 边界

**请求**：烧录、重启、串口录制、BLE/MQTT 只读与测试命令（tryconnect/cancel/status/diag.m1hook）、回归期临时复位。
**不含**：配置文件/生产参数变更、erasing 全片、65D8 任何操作、expired/op-V 复测（需另立窗口）。
