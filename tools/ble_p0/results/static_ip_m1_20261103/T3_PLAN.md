# T3 方案草案（未执行）：静态字段缺失 → 启动救援

> 状态：仅方案。未改代码、未写 NVS、未执行 T3。

## 1. 测试目标（S2）

冷启动时 NVS 中存在"启用静态但缺 `sta_static_ip`"的配置：
- `apply_begin mode=static source=boot`；
- 失败 stage=`static_parse`，`apply_failed` 记录一次；
- 恢复链 → DHCP 救援成功（`recover action=dhcp_rescue result=ok state=fallback`）；
- `net_config_state=fallback`，GOT_IP（DHCP）、MQTT 在线；
- NVS 未被改写（`config.get` 仍 static=true 且字段缺失/原样）；
- BLE 救援可用；无 abort/重启循环。

## 2. 设备与状态约束

| 设备 | 当前 | T3 约束 |
| --- | --- | --- |
| C8E4 | M1 正式，静态 192.168.1.50 | T3 候选；测试后必须恢复 NVS 备份并回到静态 .50（或经批准转 DHCP） |
| 65D8 | M1 正式，DHCP | 不参与 T3，保持不动 |

## 3. 关键问题：现有钩子无法在冷启动前布防

`diag.m1hook` 的 `apply_fail` 状态是 RAM 变量，重启即丢。M1 的 T3–T8 是**启动期**故障，现有钩子只能在运行期注入（如 `network.restart`），无法覆盖冷启动 S2。

可选路径：

| 方案 | 做法 | 优点 | 代价/风险 |
| --- | --- | --- | --- |
| A. NVS 直写 | 备份 NVS（0x9000/0x4000）→ 改写配置去掉 `sta_static_ip` → 重启 | 最贴近真实持久化故障；可验证 M1-07（NVS 不被救援改写） | 需 NVS 格式/CRC 工具；写坏需恢复备份 |
| B. 启动期注入 | 钩子增加持久注入（RTC_NOINIT 或 NVS 测试键）；`boot_fail stage=static_parse count=1` 后重启 | 确定性、可重复；不改业务 NVS | 需新增钩子代码 + 刷 hooks 构建；只证明代码路径，不证明"真实坏 NVS" |
| C. 运行期模拟 | 运行中 arm `apply_fail static_parse` 再 `network.restart` | 无需改代码/刷写 | 只覆盖运行期 LKG/救援（T10 类），**不能替代 T3 冷启动** |

建议：
- **T3 用 A**（一次性真实持久化故障，验证 NVS 不变式与冷启动救援）；
- **T4–T8 用 B**（确定性矩阵）；
- C 仅作为 T10 运行期用例，不冒充 T3。

## 4. 执行前置（待批准）

1. 是否批准 NVS 分区读取/备份/写回（仅 C8E4）；
2. 是否批准为启动期注入新增测试钩子提交（仍默认 OFF、独立提交）；
3. T3 结束后恢复策略：写回 NVS 备份并确认回到静态 .50；若恢复失败，保留备份与日志，不继续 T4。

## 5. T3 执行草案（批准后）

1. NVS 备份（read_flash 0x9000 0x4000，双份 + sha256）；
2. 构造缺失 `sta_static_ip` 的静态配置并写回；
3. 冷启动采集 ≥120s；
4. 断言：`apply_failed stage=static_parse`、`dhcp_rescue result=ok`、`net_config_state=fallback`、GOT_IP/MQTT、NVS 回读仍 static=true、无 abort/rst 循环；
5. 恢复 NVS 备份、重启确认静态 .50；提交证据。
