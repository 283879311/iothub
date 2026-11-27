# 主机侧测试入口

> 文档状态：CURRENT  
> 本页只列仓库中实际存在的测试目标；不包含设备烧录或现场操作授权。

主机侧检查不需要设备或烧录：

```bash
# Token 生命周期与临界区审计
make -C tests/token_store run
make -C tests/token_store audit

# 统一命令上下文
make -C tests/cmd_context run

# CMD-03 network job/trial ownership
make -C tests/cmd_net_job run

# Operation expiring 窄窗口与可证伪性
make -C tests/operation_host run
make -C tests/operation_host falsify

# C15-2 test-only hook/defer 诊断矩阵
make -C tests/c15_hook run

# Wi-Fi owner 状态机
make -C tests/wifi_owner run

# netcheck 策略
make -C tests/netcheck run

# S6 操作门禁脚本
python3 tests/s6_gate/test_gate.py

# C 语法检查（复用 build/compile_commands.json，需先在完整 IDF 环境构建过一次）
python3 tests/syntax_check.py --strict main/auth_token.c main/ble_cmd.c main/cmd.c

# v1-only 工具/当前文档防回退
.venv-bleak/bin/python tools/ble_p0/check_v1_only.py
```

以上目录和 Makefile 均以当前仓库树为准。此页不再引用不存在的 `cmd_policy`、`cmd_cfg_txn`、`cmd_error` 测试目录。
