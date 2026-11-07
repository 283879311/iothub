# tests

主机侧检查，不需要设备或烧录：

```bash
make -C tests/token_store run
python3 tests/token_store/audit_critical_sections.py
python3 tests/syntax_check.py --strict main/auth_token.c main/ble_cmd.c main/cmd.c

# Stage 0 接口治理（纯策略/门控/错误码）
make -C tests/cmd_policy run
make -C tests/cmd_cfg_txn run
make -C tests/cmd_error run

# v1-only 工具/当前文档防回退
.venv-bleak/bin/python tools/ble_p0/check_v1_only.py
```

`syntax_check.py` 复用 `build/compile_commands.json`，需先在完整 IDF 环境构建过一次。
