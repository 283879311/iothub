# 待审 harness 快照

本目录存放**等待代码审查**的测试 harness 快照，不参与固件构建，也不随固件发布。

## `cmd0215_stage3_v6.py`（当前待审版本）

- 状态：**待审**。未在真机运行；`--selftest` 只用 mock 验证控制流。
- SHA-256 `cd141430eff000774ec11a2b2d699af25ee108b9ea48f63d1b84542fb3388846`，1121 行。
- 针对 v5 初审的一个阻塞项、一个覆盖缺口与一个证据边界：
  1. **C15-1 接口回归修复**：属性采样改走统一消息模型 `snapshot()` + `merge_attrs()`，
     不再调用 v5 已删除的 `AttrTap.merged()`；窗口从 SUBACK 确认起，最多等 20s
     取首条 attributes；采样失败计入 `case_c151` 返回值，触发停止后续用例。
  2. **C15-1 完整 mock 路径**：新增 T19（订阅建立 → 窗口采样 PASS → finally stop + 恢复）
     与 T20（窗口内只有 telemetry → 属性快照 FAIL，但 finally 仍恢复并 stop）。
     两个 mock 只提供 `snapshot()`，旧接口一旦回归会直接失败。
  3. **C15-2 证据边界**：结果改名为 `C15-2 tryconnect 及恢复区间属性自洽`，明确
     `w0=tryconnect 发送前`、`w1=STA 恢复+身份核验后`，记录
     `semantics=operation_and_recovery_interval`；只声称覆盖“操作及恢复区间”，
     **不声称覆盖瞬时 Wi-Fi 断开窗口**。若需后者，须另行记录 STA 断开/恢复边界
     并按其筛选属性消息。
- 自测：`--selftest` 共 20 项，输出 `SELFTEST: PASS (20/20)`。
- 残余限制：`flock` 为建议性锁、preflight 到 open 有 TOCTOU；C15-1/C15-2 的属性断言
  依赖窗口内确有 attributes 发布；`got_ip` 关联覆盖需已知口令 AP（本版不含）。
- v5/v4/v3/v2 保留对照。

## `cmd0215_stage3_v5.py`（已被 v6 取代，保留对照）

- 已知缺陷（v6 修复）：C15-1 仍调用 v5 已删除的 `tap.merged()`，真机路径会
  `AttributeError`；18 项自测的 C15-1 mock 自带 `merged()`，未覆盖该回归。

- 状态：**待审**。未在真机运行；`--selftest` 只用 mock 验证控制流。
- SHA-256 `8a74f60018ed30ca554ee3098134fea2b9a1f2de40d8ea409c962937fc1bba82`，1028 行。
- 针对 v4 审查的三项：
  1. **SUBACK 真正判定**：`suback_ok()` 检查每个 reason code，任一 ≥0x80 即拒绝；
     `subscribe()` 的本地返回码也必须为 `MQTT_ERR_SUCCESS`；失败即 `AttrTap` 初始化抛错，
     C15-2 在抖动前中止。
  2. **属性窗口隔离**：每条消息保存 `topic/ts/obj`；C15-2 以 `w0`（tryconnect 前）到
     `w1`（恢复+身份核验后）为窗口，`merge_attrs()` 只取窗口内 `attributes` 主题；
     `attributes=0` 直接 FAIL（遥测条数仅记录、不参与判定）；窗口外旧值不参与合并。
  3. **显式 disconnect**：`AttrTap.stop()` 先 `disconnect()` 再 `loop_stop()`。
- **C2-3 结果拆分**：
  - `C2-3 absent 路径（无 trial 残留/终态）`：可 PASS/FAIL；
  - `C2-3 got_ip trial/job 关联`：absent 路径不产生 trial，记 `not_covered=True` +
    detail `NOT_COVERED`，**不计入整体 pass**，并在 `stage3_results.json.not_covered` 中列出。
- 自测：`--selftest` 共 18 项，新增 SUBACK 拒绝、属性窗口隔离、disconnect 顺序、
  C2-3 覆盖拆分。
- 残余限制：`flock` 为建议性锁、preflight 到 open 有 TOCTOU；C15-2 依赖抖动窗口内确有
  attributes 发布；`got_ip` 关联覆盖需已知口令 AP（本版不含）。
- v4/v3/v2 保留对照。

## `cmd0215_stage3_v4.py`（已被 v5 取代，保留对照）

- 状态：**待审**。未在真机运行；`--selftest` 只用 mock 验证控制流。
- SHA-256 `3da89c784da3a33b0aac642896b82a83bac44daeceed261aa16d997670d2b645`，916 行。
- 针对 v3 审查的四项：
  1. **统一写入口**：`w_tryconnect`/`wait_terminal`/`wait_sta` 均改用本文件的 `call()`；
     `race_suite` 只剩 `Session/ADDR/ABSENT`（连接与常量），不再借其写/轮询 helper。
  2. **删除类名兜底**：`classify_exc` 只按真实类型 `isinstance`；真实类型不可用一律 `other`，
     不重连。自测 `classify-no-name-fallback` 验证。
  3. **C2-3 关联**：记录本次 `operation_id`；`trial=got_ip` 时要求 `trial_ssid` 等于本次 job 的
     SSID，并交叉读取 `operation.get` 要求 job 处于 `got_ip/committing/success`；非 `got_ip`
     不得带 trial 字段。**覆盖说明**：absent-SSID 路径不会产生 trial，此时结果标注
     `coverage=no_trial_in_absent_path`，关联校验实际未覆盖（需已知口令的真实 AP 才能覆盖）。
  4. **C15-2 终态与订阅确认**：`tryconnect` 终态必须是 `failed` 且错误码为
     `wrong_password`/`ap_not_found`，否则 FAIL；`AttrTap` 必须等到 CONNACK 与 SUBACK
     才返回并记录时间戳，建立失败直接 FAIL。
- 自测：`--selftest` 共 14 项，含只读重连、写命令不重发、超时不重连、协议异常透传、
  身份缺失/不匹配、结构化核验、恢复发送次数、reader 异常、C15-2 订阅先于抖动、
  C2-3 trial 不一致必 FAIL、类名兜底禁用、tryconnect 不重发、C15-2 终态不符必 FAIL。
- 残余限制：`flock` 为建议性锁、preflight 到 open 仍有 TOCTOU；C15-2 依赖抖动窗口内确有
  属性发布；C2-3 的 trial 关联在 absent-SSID 刺激下无法覆盖。
- v3/v2 保留对照。

## `cmd0215_stage3_v3.py`（已被 v4 取代，保留对照）

- 状态：**待审**。未在真机运行；`--selftest` 只用 mock 验证控制流。
- SHA-256 `51f35925d7215a35803d53f0b4dff06e65cfb609f4d6bf99adff7aae8f0e2eb2`，727 行。
- 针对 v2 审查的 5 个阻塞项：
  1. 写命令默认不重发：`READ_ONLY_CMDS` 白名单外的命令遇 transport 异常抛
     `OutcomeUnknown`，不重发；恢复循环 3 次 = 3 次实际发送。
  2. 异常按真实类型分类（`bleak.exc.BleakError` 子类 / `paho` 无关；
     `p0_acceptance.ConnectionLost`）；`asyncio.TimeoutError` 单列 `timeout`，不重连。
  3. 设备身份贯穿：`device.status` 缺 `device_id` 即失败；C15-1 恢复核验绑定预期设备；
     C15-2 增加身份核验步骤。
  4. `verify_restore` 两阶段结构化留证（config/mqtt 各自 stage + ts + error/kind），
     不再因异常丢 detail。
  5. 串口：`/proc` preflight + `flock(LOCK_EX|LOCK_NB)` 双重检查；reader 线程异常
     保存并判 FAIL；启动失败清理 FD/文件句柄。
- 用例语义修正：C2-2 等待 cancel 终态；C2-3 校验 trial 与 job 字段一致性；
  C15-2 属性订阅先于 Wi-Fi 抖动；C2-4 校验状态单调性与 `error_detail/stages` 内容。
- 自测：`--selftest` 共 10 项（只读重连/写命令不重发/超时不重连/协议透传/
  身份缺失/身份不匹配/结构化核验/恢复发送次数/reader 异常/属性订阅顺序）。
- 残余限制：
  - `flock` 是建议性锁，只能阻止同样加锁的读者；未加锁的外部 `cat` 仍可能并发，
    `/proc` preflight 到 `open` 之间仍有 TOCTOU 窗口。采集完整性依赖运行纪律。
  - C15-2 依赖设备在抖动窗口内确实发布属性；若窗口内 0 条，会判 FAIL（环境问题，
    需真机确认）。
  - 所有设备路径与属性采样均未经真机验证。

## `cmd0215_stage3_v2.py`（已被 v3 取代，保留对照）

- 用途：CMD-02 / CMD-15 阶段 3 设备回归（C2-1～C2-4、C15-1、C15-2）。
- 状态：**待审**。尚未在真机运行；`--selftest` 只用 mock 验证控制流。
- 快照来源：`/tmp/cmd0215_stage3_v2.py`，SHA-256
  `99f1e40b9a6f953a9561db6b24edcf04062c8240cee8112bfea78cd08133cd82`。
- 运行前置（仅设备路径）：Python 环境需含 `bleak` / `paho-mqtt`（本机 `.venv-bleak`），
  并可用 `tools/ble_p0/p0_acceptance.py` 的 `Session`；脚本顶部 `sys.path.insert(0, "/tmp")`
  依赖 `race_suite`，在仓库内直接运行需先调整该路径或提供等价的会话封装。
- 设备参数：`SERIAL_PORT = /dev/ttyUSB0`、`ADDR = 10:97:BD:D3:65:DA`（65D8），
  `OUT` 默认 `/tmp/cmd0215_out`，可用 `--out` 覆盖。

### 已知待改（作者自审，尚未修）

1. `TRANSPORT_EXC` 按类名匹配，`asyncio.TimeoutError` 会被误判为传输故障，
   可能对写命令触发重发；应改为按 bleak 异常类型判断。
2. `call()` 对写命令未加“可重发白名单”，`config.set` 在重连后会被盲发。
3. `verify_restore()` 在 `config.get` 抛异常时直接退出，不产出 detail。
4. `SerialCapture.preflight()` 的 `/proc` 扫描与 `os.open` 之间存在检查-使用窗口（TOCTOU），
   不是严格独占。

### 审查边界

本文件通过审查只代表 harness 具备申请下一次设备测试窗口的条件；
不代表 C15-1 重跑或 C15-2 已获授权。
