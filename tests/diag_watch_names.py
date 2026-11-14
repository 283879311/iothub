#!/usr/bin/env python3
"""校验 main/diag.c 的 s_stack_watch 名字长度（< configMAX_TASK_NAME_LEN=16）。

背景：xTaskGetHandle() 对 >=16 字符的任务名直接 assert 崩溃（1.3-44 实测）。
运行期已有两道护栏（app_diag_start 启动校验 + 快照循环跳过），本用例把问题
挡在提交前，避免等到上机才炸。

用法: python3 tests/diag_watch_names.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'main', 'diag.c')
LIMIT = 16  # configMAX_TASK_NAME_LEN


def main():
    try:
        with open(SRC, 'r', encoding='utf-8') as f:
            text = f.read()
    except OSError as e:
        print('FAIL: cannot read %s: %s' % (SRC, e))
        return 2

    m = re.search(r's_stack_watch\[\]\s*=\s*\{(.*?)\};', text, re.S)
    if not m:
        print('FAIL: s_stack_watch[] not found in diag.c')
        return 2

    names = re.findall(r'"([^"]*)"', m.group(1))
    if not names:
        print('FAIL: s_stack_watch[] is empty')
        return 2

    bad = [(n, len(n)) for n in names if len(n) >= LIMIT]
    for n, ln in bad:
        print('FAIL: "%s" len=%d >= %d (xTaskGetHandle would assert)' % (n, ln, LIMIT))

    print('diag_watch_names: %d entries, %d too long' % (len(names), len(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
