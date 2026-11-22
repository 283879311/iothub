# 待审 harness 快照

本目录存放**等待代码审查**的测试 harness 快照，不参与固件构建，也不随固件发布。

## `cmd0215_stage3_v3.py`（当前待审版本）

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
