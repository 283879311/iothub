#!/usr/bin/env python3
"""Guard: active tools/docs must not carry v0 command calls or examples."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
ACTIVE_DOCS = [
    ROOT / 'README.md',
    ROOT / 'docs/BLE命令协议规范.md',
    ROOT / 'docs/BLE微信小程序交互协议_T4_2026-10-24.md',
    ROOT / 'docs/soak_test_plan.md',
    ROOT / 'docs/BLE功能现状与目标.md',
    ROOT / 'docs/BLE需求基线.md',
    ROOT / 'tools/ble_p0/README.md',
]
failures = []

for path in sorted((ROOT / 'tools/ble_p0').glob('*.py')):
    if path.name in ('check_v1_only.py', 'a_token_board_test.py'):
        continue  # a_token_board_test intentionally holds a disabled-v0 negative probe
    text = path.read_text(encoding='utf-8', errors='replace')
    if '.v0(' in text:
        failures.append(f'{path.relative_to(ROOT)}: active .v0( call')

for path in ACTIVE_DOCS:
    if not path.exists():
        continue
    text = path.read_text(encoding='utf-8', errors='replace')
    if '"method":' in text or 'method":"' in text:
        failures.append(f'{path.relative_to(ROOT)}: v0 method/params example')
    if 'v0 `config.set`' in text:
        failures.append(f'{path.relative_to(ROOT)}: active v0 config.set wording')

if failures:
    print('V1-ONLY CHECK: FAIL')
    for f in failures:
        print(' -', f)
    sys.exit(1)
print('V1-ONLY CHECK: PASS')
