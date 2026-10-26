#!/usr/bin/env python3
"""单文件语法/类型检查(复用工程真实编译参数)。

用途:在整机编译之外快速检查单个源文件;build/compile_commands.json 由真实
工具链生成,带 -Wall -Werror -Wextra 和全套 -I。把任一 main/*.c 的参数抽出来、
换掉 -o/-c/源文件,就能用同一套标准检查任意源文件,包括新增/未进 build 的文件。
整机编译仍以 `idf.py -B build build` 为准。

用法:
    python3 tests/syntax_check.py                    # 检查 main/ 下全部 .c
    python3 tests/syntax_check.py main/auth_token.c main/ble_cmd.c
    python3 tests/syntax_check.py --strict main/auth_token.c   # 忽略 -Wno-error=*
"""
import json
import os
import shlex
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CC_DB = os.path.join(ROOT, 'build', 'compile_commands.json')


def load_template():
    with open(CC_DB, 'r', encoding='utf-8') as f:
        db = json.load(f)
    # 优先用 auth.c:它只依赖少量头文件,参数集最"干净",且不涉及 sdkconfig 差异
    for cand in ('/main/auth.c', '/main/main.c'):
        for e in db:
            if e['file'].endswith(cand):
                return e
    return db[0]


def build_flags(entry, strict=False):
    args = shlex.split(entry['command'])
    out = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == '-o' or a == '-c':
            i += 2
            continue
        if a.endswith(('.c', '.cpp')) and not a.startswith('-'):
            i += 1
            continue
        if strict and a.startswith('-Wno-error='):
            i += 1
            continue
        out.append(a)
        i += 1
    return out


def main(argv):
    strict = '--strict' in argv
    argv = [a for a in argv if a != '--strict']

    if not os.path.exists(CC_DB):
        print('缺少 %s —— 需要先在完整环境里跑过一次 idf.py build' % CC_DB)
        return 2

    entry = load_template()
    base = build_flags(entry, strict)

    if len(argv) > 1:
        targets = [os.path.abspath(a) for a in argv[1:]]
    else:
        main_dir = os.path.join(ROOT, 'main')
        targets = sorted(
            os.path.join(main_dir, f) for f in os.listdir(main_dir) if f.endswith('.c')
        )

    failed = []
    for t in targets:
        cmd = base + ['-fsyntax-only', t]
        r = subprocess.run(cmd, cwd=entry['directory'], capture_output=True, text=True)
        name = os.path.relpath(t, ROOT)
        if r.returncode == 0:
            print('  OK    %s' % name)
        else:
            print('  FAIL  %s' % name)
            sys.stderr.write(r.stderr)
            failed.append(name)

    print('\n%d 个文件:%d 通过 / %d 失败' % (len(targets), len(targets) - len(failed), len(failed)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
