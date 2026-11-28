# 待审 harness 快照

本目录存放**等待代码审查**的测试 harness 快照，不参与固件构建，也不随固件发布。

## `cmd0215_stage3_v9.py`（已退役，2026-11-26）

- 状态：**已退役**。已用于 CMD-02/15 阶段 3 r1–r7；不再用于 C15-2 或全量重跑。
  `--selftest` 只用 mock 验证控制流，不需要凭据、不加载 `p0_acceptance`、不连接设备。
- SHA-256 `fb79bd7a2214103dabf5130425da7841eb8aea36b1e69538120ac564f49046a8`，1517 行。
- 退役原因（2026-11-26）：C15-2 判据仍从 attributes 读取 `sta_has_ip`（该字段仅 telemetry）；
  单次只读超时会终止轮询；就绪等待日志与 `stable >= 10` 不一致。C15-2 另立专项脚本，
  修正字段来源与有界超时容错后再单独授权设备验证。单文件 1517 行超过规范，拆分另行处理。
- 修订（2026-11-25）：仅将 `--expect-fw/--expect-sha` 的帮助示例更新为当前生产基线
  `1.3-956afcd` / `c231c9501`；控制流、C2/C15 判据、串口/停止/恢复逻辑未变。
  原示例基线为 `1.3-1ff92db` / `32b10dfa5`（历史对照，不用于本次执行）。
- 修订（2026-11-26，test-only）：`_on_connect` 改用 `rc_int()`，兼容 paho-mqtt 2.x
  `ReasonCode.value` 与 1.x 整数 rc；新增 selftest T31，`--selftest` 31/31。
  不涉及设备协议、生产代码、判据或配置写入范围。
- 修订（2026-11-26，判据）：C15-1 attributes 断言改为实际发布字段
  （net_mode/sta_ssid/sta_ip/uart_baudrate）；`sta_has_ip` 改由同一窗口
  `network.status` 核验。原因：该字段仅在 telemetry，attributes 从 1ff92db 起即未发布。
- 修订（2026-11-26，test-only）：21883 的 AttrTap 支持 `IOTHUB_MQTT_TOKEN`（设备 token，
  仅环境注入，不记录）；1883 仍匿名。21883 只订阅 attributes（ThingsBoard 拒绝设备
  telemetry 订阅，SUBACK 0x80），1883 仍订阅 attributes+telemetry。
- 只修 v8 初审的 **BLOCKER-3（未锁定导入）**：
  - 删除顶部的 `from p0_acceptance import ConnectionLost`。该语句会在 SHA-256 校验
    之前从当前 `sys.path` 尝试导入任意同名模块，破坏依赖锁定边界。
  - `ConnectionLost` 初始为 `None`，唯一赋值点是 `run_device()` 中经哈希校验的
    `load_p0_acceptance()`。
  - 新增 T30：在 `sys.path` 放置会写 sentinel 的同名 `p0_acceptance.py`，
    重新加载 harness 后断言 sentinel 未生成且 `ConnectionLost is None`。
- 自测：`--selftest` 共 31 项，输出 `SELFTEST: PASS (31/31)`。
- **设备操作前的未决项**：`SERIAL_PORT` 默认 `/dev/ttyUSB0`，但 `2026-11-16` 授权
  文档记录的是 `/dev/ttyUSB1`（且该文档状态为 PENDING）。这不代表串口已获授权；
  任何设备运行前必须核对当前有效授权与实际 boot 后的串口映射，不得默认两者一致。
- 其余运行前置、依赖哈希、残余限制与 v8 相同（见下节）；v8/v7/…/v2 保留对照。

## `cmd0215_stage3_v8.py`（已被 v9 取代，保留对照）

- 已知阻塞（v9 修复）：顶部仍有未指定路径、未校验哈希的
  `from p0_acceptance import ConnectionLost`，会在锁定加载前尝试导入同名模块。
- 状态：**待审**。未在真机运行；`--selftest` 只用 mock 验证控制流，不需要凭据、
  不加载 `p0_acceptance`、不连接设备、不打开串口。
- SHA-256 `32af5e79a1254bd40a06f49d12d6479827dfb879238fe195ddc5698627670945`，1429 行。
- 针对 v7 初审的两个阻塞项：
  1. **设备身份绑定**（BLOCKER-1）：新增必填 `--expect-device`（本轮为
     `IotHub-65D8`）。连接后先读 `device.status.device_id`，与授权身份不一致即记录
     FAIL 并在**任何测试写命令之前**停止。
     **地址说明**：`ADDR=10:97:BD:D3:65:DA` 是 65D8 的 BLE 地址；其 WiFi MAC 是
     `10:97:BD:D3:65:D8`。按授权文档（`docs/authorization/S6阶段B操作授权申请_2026-11-16.md`
     §身份表），65D8 = BLE `…65:DA` + WiFi MAC `…65:D8` + device_id `IotHub-65D8`。
     BLE 地址比 WiFi MAC 大 2，不能把 WiFi MAC 当作 BLE 地址连接，因此默认地址保持
     `…65:DA` 并增加 device_id 强校验，而不是改成 `…65:D8`。
  2. **依赖凭据边界闭合**（BLOCKER-2）：`tools/ble_p0/p0_acceptance.py` 的
     `Session.__init__` 删除内置默认口令，改为显式参数或
     `IOTHUB_BLE_USER` / `IOTHUB_BLE_PASSWORD`，两者都缺失时 `ValueError`。
     依赖锁定 SHA-256 更新为
     `3882a2e050c02ca8ec854915dd7aba528a689e23758f1d709d7bbd51bcaa4673`。
     harness 加载依赖后额外校验构造器 `username/password` 默认值必须为 `None`
     或缺失；带任何非 None 默认值即拒绝运行。
- 附带清理：
  - **BLE 断开进 finally**：身份不符、基线不符、中途异常都保证 `disconnect()`。
  - **stty 失败即前置失败**：改为 `check=True`，失败抛 `RuntimeError`，不再继续
    生成可能不完整的串口证据。
- 负向自测：T26 身份门（缺失/不符/匹配）、T27 构造器凭据默认值守卫、
  T28 stty 失败拒绝、T29 断开清理。`--selftest` 共 29 项，
  输出 `SELFTEST: PASS (29/29)`。
- 运行前置（设备路径）：
  ```bash
  export IOTHUB_BLE_USER=... IOTHUB_BLE_PASSWORD=...   # 只注入环境，不入库
  .venv-bleak/bin/python cmd0215_stage3_v8.py \
    --expect-device IotHub-65D8 \
    --expect-fw 1.3-1ff92db --expect-sha 32b10dfa5 --out /path/to/artifacts
  ```
  五项缺一即 `REFUSE`（exit 2），且在打开串口/连接 BLE 之前。
- **依赖行为变化**：`p0_acceptance.Session` 不再有默认凭据，仓库内其它直接构造
  `Session` 的历史工具（未显式传参者）现在需要注入环境变量，否则 fail-closed。
  这是有意为之；本轮未逐个修改那些工具。
- 残余限制：`flock` 为建议性锁、preflight→open 仍有 TOCTOU；29 项自测只验证
  mock 控制流，不能验证真实 Paho 回调时序、真实 MQTT 窗口或设备恢复行为；
  C15-1/C15-2 属性断言依赖窗口内确有 attributes 发布；`got_ip` 关联覆盖需
  已知口令 AP。
- v7/v6/v5/v4/v3/v2 保留对照。

## `cmd0215_stage3_v7.py`（已被 v8 取代，保留对照）

- 已知阻塞（v8 修复）：未将 `device_id` 与授权设备身份比对；锁定的依赖构造器
  仍保留默认口令，README“仓库文件内不出现口令”的表述不成立。

- 状态：**待审**。未在真机运行；`--selftest` 只用 mock 验证控制流，不需要凭据、
  不加载 `p0_acceptance`、不连接设备、不打开串口。
- SHA-256 `b7096df64bb74d0eb1dc9322c5089a3f7d4f7f3f6ed063cbaf02c959f34c0de9`，1317 行。
- 针对 v6 初审的三项阻塞：
  1. **恢复写不再自动重发**（BLOCKER-1）：C15-1 的恢复 `config.set` 只发一次。
     `OutcomeUnknown` / `asyncio.TimeoutError` / 其它异常分别记录为
     `restore_outcome_unknown_no_resend` / `restore_timeout_no_resend` /
     `restore_exception_no_resend`，一律不进入重试；是否恢复成功只由随后的
     只读核验（`config.get` + `device.status`）判定，核验失败即 FAIL 并停止后续用例。
  2. **凭据不再硬编码**（BLOCKER-2）：设备路径要求环境变量
     `IOTHUB_BLE_USER` / `IOTHUB_BLE_PASSWORD`，缺失直接 `REFUSE`（exit 2），
     且发生在打开串口/连接 BLE 之前；仓库文件内不出现口令。
  3. **依赖固化**（BLOCKER-3）：删除对 `/tmp/race_suite.py` 的隐式 import（该文件
     不在仓库内，且其 `main()` 含硬编码口令）。改为显式加载仓库内
     `tools/ble_p0/p0_acceptance.py`（可用 `CMD0215_P0_ACCEPTANCE` 覆盖路径），
     锁定 SHA-256
     `45977737165aa176998c5d92a021e22db83036e62c2a80985dca799f935c1a63`；
     文件缺失、哈希不符或缺少 `Session`/`ConnectionLost` 接口即拒绝运行。
- 非阻塞事项：
  - **REVIEW-05 基线镜像强制匹配**：设备路径必须给 `--expect-fw` 与
    `--expect-sha`，与 `device.status.firmware/build_sha` 不一致即记录 FAIL 并停止。
  - **REVIEW-06 串口顺序**：先 `open` + `flock`，再 `stty`；preflight→open 的
    TOCTOU 仍存在（建议性锁），但改串口参数已不在无锁窗口内。
  - **REVIEW-07 窗口边界 mock**：T19 增加 SUBACK 之前的属性消息（`ts=50 < w0`，
    带冲突值），断言采样只取窗口内值。
  - **REVIEW-04 规模偏离**：见下节。
- 负向自测：T7（恢复只发 1 次）、T21（恢复结果不确定但只读核验通过）、
  T22（恢复超时且核验失败）、T23（凭据/基线缺失拒绝）、T24（依赖哈希不符）、
  T25（依赖缺失）。`--selftest` 共 25 项，输出 `SELFTEST: PASS (25/25)`。
- 运行前置（设备路径）：
  ```bash
  export IOTHUB_BLE_USER=... IOTHUB_BLE_PASSWORD=...   # 只注入环境，不入库
  .venv-bleak/bin/python cmd0215_stage3_v7.py \
    --expect-fw 1.3-1ff92db --expect-sha 32b10dfa5 --out /path/to/artifacts
  ```
  默认依赖路径 `/home/tc/esp/workspace/iothub/tools/ble_p0/p0_acceptance.py`；
  换环境用 `CMD0215_P0_ACCEPTANCE=/abs/path/p0_acceptance.py`，哈希必须与常量一致。
- 残余限制：`flock` 为建议性锁、preflight→open 仍有 TOCTOU；25 项自测只验证
  mock 控制流，**不能验证真实 Paho 回调时序、真实 MQTT 窗口或设备恢复行为**
  （REVIEW-08），这些只能在获授权的设备窗口内验证；C15-1/C15-2 属性断言依赖
  窗口内确有 attributes 发布；`got_ip` 关联覆盖需已知口令 AP。
- v6/v5/v4/v3/v2 保留对照。

### REVIEW-04：单文件规模偏离说明

- 依据：仓库 `AGENTS.md` §7“Python/测试 harness 约 400–500 行评估，超过 600 行原则上
  拆分”。本文件 1317 行，已超过该阈值。
- 暂不拆分的理由：该文件是**待审快照**，评审以 v2→v7 的逐版 diff 为主线；
  现在拆成多文件会让 1300 行整体移动，历史版本对照失效，新增的导入路径与模块
  边界本身也需要一轮完整审查。v7 的授权范围只允许修 harness，不适合同时做结构重构。
- 处理决定：**记录偏离并延后**。设备窗口关闭后拆分为 `harness_common.py`
  （异常分类/call/RunState）、`harness_attr.py`（AttrTap/merge_attrs）、
  `harness_serial.py`（SerialCapture）、`harness_cases.py`（C2-1…C15-2）、
  `harness_selftest.py`；拆分需单独提交与审查，并用同一套 selftest 验证等价性。

## `cmd0215_stage3_v6.py`（已被 v7 取代，保留对照）

- 已知阻塞（v7 修复）：C15-1 恢复写在 `OutcomeUnknown` 后仍循环重发；设备凭据
  硬编码；隐式依赖 `/tmp/race_suite.py` 且未校验哈希。

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

## C15-2 BUSY 诊断构建（`APP_C15_BUSY_DIAG`）

- test-only，默认 OFF；启用时必须提供 `APP_C15_BUSY_DIAG_TAG`
  （格式 `C15-BUSY-DIAG-YYYYMMDD-NN`），否则 CMake `FATAL_ERROR`。
- 构建示例：
  ```bash
  idf.py -B /tmp/c15diag-diag \
    -DAPP_C15_BUSY_DIAG=ON \
    -DAPP_C15_BUSY_DIAG_TAG=C15-BUSY-DIAG-20261126-01 build
  ```
- 生产构建不定义该宏；用 `nm` 检查 `app_c15_busy_diag_*`、
  `app_operation_diag_snapshot` 符号数量为 0。
- 诊断构建在 tryconnect BUSY/NO_TXN 时输出一行 `C15_BUSY_DIAG`，
  含 `busy_gate`、`mapped_busy_source`、job 与 active operation 快照；
  不新增 BLE 命令、不改变配置写入范围。
- 设备使用需单独授权；tag、镜像 SHA-256 与证据一并归档。
- host `tests/cmd_net_job` 增加 D01–D05：4 个最大长度 operation、文本截断、
  计数截断、终态 job_op、无效 job_op；`truncated` 与 `text_truncated` 分开记录。

## `c152_mqtt_attrs.py` + `c152_policy.py`（C15-2 专用，Host 级，待评审）

- 取代 v9 的 C15-2 字段来源：attributes 自洽仅使用实际发布字段
  （`net_mode`/`sta_ssid`/`sta_ip`/`uart_baudrate`，`net_mode` 运行态预期 `sta`）；
  `sta_has_ip` 由 w1 之后 `network.status` 独立对读，要求 status 采样不早于窗口内
  最后一条 attributes 且两侧 `sta_ip` 均非空。
- 身份门：`device_id`/`firmware`/`build_sha` 任一缺失或不匹配，在任何写命令前 `STOP`。
- 容错语义：只读命令最多 3 次、间隔 2 s；所有请求由全局单调序号分配唯一 ID（跨轮询不重复，迟到响应不得满足下一次查询）；持续超时 → `STOP`；
  `n_attr=0` → `NOT_COVERED`；写命令 transport 异常不重发；就绪门禁用
  `time.monotonic()` 实际连续 10 s、间隔 >3 s 清零。
- 纯判定在 `c152_policy.py`；Host 测试：`make -C tests/c152_policy run`（policy + runner
  fake-session/fake-clock，覆盖身份门、唯一 ID、超时、busy、空 IP、SUBACK、退出码）。
- 设备执行需单独授权；本目录脚本不参与固件构建。
