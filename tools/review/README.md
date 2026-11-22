# 待审 harness 快照

本目录存放**等待代码审查**的测试 harness 快照，不参与固件构建，也不随固件发布。

## `cmd0215_stage3_v2.py`

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
