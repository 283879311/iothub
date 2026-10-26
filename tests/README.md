# tests

主机侧检查，不需要设备或烧录：

```bash
make -C tests/token_store run
python3 tests/token_store/audit_critical_sections.py
python3 tests/syntax_check.py --strict main/auth_token.c main/ble_cmd.c main/cmd.c
```

`syntax_check.py` 复用 `build/compile_commands.json`，需先在完整 IDF 环境构建过一次。
