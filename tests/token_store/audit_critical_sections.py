#!/usr/bin/env python3
"""auth_token.c 静态审计:把代码审查清单变成可执行检查。

为什么需要这层:临界区里的东西(有没有混进日志、比较有没有被挪到锁外、
比较是不是定长、过期用 > 还是 >=)单测测不出来 —— 它们要么不改变返回值,
要么只在极端时序下才表现不同;靠人眼逐行看又容易漏。

局限(必须清楚):这只证明"代码形状符合契约",不证明"运行时行为正确"。
行为由 test_token_store.c 在宿主环境验证。

用法:
    python3 tests/token_store/audit_critical_sections.py
    python3 tests/token_store/audit_critical_sections.py main/auth_token.c
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_SRC = os.path.join(ROOT, 'main', 'auth_token.c')

results = []


def check(name, ok, detail=''):
    results.append((name, ok, detail))


def strip_comments(text):
    """把注释替换成空白但保留换行,这样行号仍然对得上。

    必须做:注释里出现了 'isxdigit(' 和 'portENTER_CRITICAL' 这类字样,
    不剥掉会直接造成误报(第一版就栽在这里)。
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join('\n' if c == '\n' else ' ' for c in text[i:j]))
            i = j
        elif text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        else:
            out.append(text[i])
            i += 1
    return ''.join(out)


def func_body(code, header_re):
    """取出函数体(header 到下一个顶格 '}'),返回 (起始偏移, 文本) 或 (None, '')"""
    m = re.search(header_re, code, re.S)
    if not m:
        return None, ''
    end = code.find('\n}', m.start())
    return m.start(), code[m.start(): (end + 2 if end >= 0 else len(code))]


def main(argv):
    src = os.path.abspath(argv[1]) if len(argv) > 1 else DEFAULT_SRC
    if not os.path.exists(src):
        print('找不到 %s' % src)
        return 2

    raw = open(src, 'r', encoding='utf-8').read()
    code = strip_comments(raw)
    lines = code.split('\n')

    # ---------- A1: 临界区配对 ----------
    enters, exits, bad = [], [], []
    open_at = None
    for idx, ln in enumerate(lines, 1):
        if 'portENTER_CRITICAL' in ln:
            if open_at is not None:
                bad.append('第%d行嵌套进入' % idx)
            open_at = idx
            enters.append(idx)
        if 'portEXIT_CRITICAL' in ln:
            if open_at is None:
                bad.append('第%d行无对应enter' % idx)
            exits.append(idx)
            open_at = None
    check('A1 临界区 enter/exit 逐个配对、无嵌套',
          len(enters) == len(exits) and open_at is None and not bad,
          'enter=%s exit=%s %s' % (enters, exits, '; '.join(bad)))

    # ---------- A2: 临界区内只允许固定长度内存操作 ----------
    forbidden = [
        ('printf', r'\bprintf\s*\('), ('snprintf', r'\bsnprintf\s*\('),
        ('sprintf', r'\bsprintf\s*\('), ('ESP_LOG', r'\bESP_LOG[EWIDV]\s*\('),
        ('esp_fill_random', r'\besp_fill_random\s*\('),
        ('esp_timer_get_time', r'\besp_timer_get_time\s*\('),
        ('cJSON', r'\bcJSON_\w+\s*\('),
        ('malloc/free', r'\b(malloc|free|calloc|realloc|heap_caps_\w+)\s*\('),
        ('阻塞调用', r'\b(vTaskDelay\w*|taskYIELD|vTaskSuspend\w*|xQueue\w+)\s*\('),
        ('strlen/str*', r'\b(strlen|strcpy|strcat|strncpy)\s*\('),
    ]
    hits = []
    for start in enters:
        end = next((e for e in exits if e > start), None)
        if end is None:
            continue
        body = '\n'.join(lines[start:end - 1])
        for fname, pat in forbidden:
            if re.search(pat, body):
                hits.append('%s@临界区(第%d行起)' % (fname, start))
    check('A2 临界区内无日志/随机数/时间/分配/阻塞调用', not hits, '; '.join(hits))

    # ---------- A3: 全文件不得出现短路比较 ----------
    short = re.findall(r'\b(memcmp|strcmp|strncmp|stricmp)\s*\(', code)
    check('A3 无 memcmp/strcmp 等短路比较', not short, str(short))

    # ---------- A4: 比较循环定长且无提前退出 ----------
    ok4, detail4 = False, '未找到 diff |= 异或累加'
    pos = code.find('diff |=')
    if pos >= 0:
        loop = None
        for m in re.finditer(
                r'for\s*\(\s*\w+\s*=\s*0\s*;\s*\w+\s*<\s*APP_TOKEN_HEX_LEN\s*;\s*\w+\+\+\s*\)', code):
            if m.start() < pos:
                loop = m
        if loop is None:
            detail4 = '异或累加前找不到定长 APP_TOKEN_HEX_LEN 循环'
        else:
            seg = code[loop.end():pos]
            ok4 = ('break' not in seg) and ('return' not in seg)
            detail4 = '循环起点第%d字符, 无提前退出=%s' % (loop.start(), ok4)
    check('A4 比较为定长 APP_TOKEN_HEX_LEN 次异或、无提前退出', ok4, detail4)

    # ---------- A5: 过期判断语义 ----------
    ok5 = re.search(r'now_ms\s*>=\s*s_slots\[\w+\]\.expire_ms', code) is not None
    bad5 = re.search(r'now_ms\s*>\s*s_slots\[\w+\]\.expire_ms', code) is not None
    check('A5 过期判断用 >=(到点即失效,不是 >)', ok5 and not bad5)

    # ---------- A6: isxdigit 参数转换 ----------
    # 不能简单抓括号内容:isxdigit((unsigned char)x) 的内层括号会让 [^)]* 提前截断。
    # 改为在调用点之后 40 字符内找强制转换。
    sites = [m.end() for m in re.finditer(r'\bisxdigit\s*\(', code)]
    ok6 = bool(sites) and all('(unsigned char)' in code[p:p + 40] for p in sites)
    check('A6 isxdigit 参数已转 unsigned char(避免负值 UB)',
          ok6, '%d 处调用, 全部带转换=%s' % (len(sites), ok6))

    # ---------- A7: issue 回包不得二次读槽位 ----------
    off, body = func_body(code, r'bool\s+app_token_store_issue\s*\([^)]*\)\s*\{')
    ok7, detail7 = False, '未找到 issue 函数体'
    if off is not None:
        m = re.search(r'memcpy\s*\(\s*out\s*,\s*([A-Za-z_]\w*)', body)
        if m:
            ok7 = (m.group(1) == 'local') and ('s_slots' not in body[m.end():m.end() + 80])
            detail7 = 'out <- %s' % m.group(1)
        else:
            detail7 = '未找到 memcpy(out, ...)'
    check('A7 issue 回包取自局部缓冲,未二次读槽位', ok7, detail7)

    # ---------- A8: clear_all 单次临界区 ----------
    off, body = func_body(code, r'void\s+app_token_store_clear_all\s*\(\s*void\s*\)\s*\{')
    ok8, detail8 = False, '未找到 clear_all'
    if off is not None:
        n_in = body.count('portENTER_CRITICAL')
        n_out = body.count('portEXIT_CRITICAL')
        e = body.find('portENTER_CRITICAL')
        x = body.find('portEXIT_CRITICAL')
        loop = body.find('for')
        ok8 = (n_in == 1 and n_out == 1 and e < loop < x)
        detail8 = 'enter=%d exit=%d 循环在锁内=%s' % (n_in, n_out, e < loop < x)
    check('A8 clear_all 在单次临界区内一次清完(不会清一半被打断)', ok8, detail8)

    # ---------- A9: 比较必须在锁内 ----------
    off, body = func_body(code, r'bool\s+app_token_store_validate\s*\([^)]*\)\s*\{')
    ok9, detail9 = False, '未找到 validate 函数体'
    if off is not None:
        e = body.find('portENTER_CRITICAL')
        x = body.find('portEXIT_CRITICAL')
        c = body.find('diff |=')
        ok9 = e != -1 and x != -1 and e < c < x
        detail9 = 'enter@%d 比较@%d exit@%d' % (e, c, x)
    check('A9 常量时间比较发生在锁内(未复制出锁再比较)', ok9, detail9)

    # ---------- A10: 辅助锁内函数只允许在临界区内被调用 ----------
    helper = 'token_slot_clear_locked'
    ok10, detail10 = True, ''
    if helper in code:
        for m in re.finditer(r'\n(?:static\s+)?[\w\*]+\s+(\w+)\s*\([^;]*?\)\s*\{', code):
            fstart = m.start()
            fend = code.find('\n}', fstart)
            # 从函数体左大括号之后开始找调用点:否则会把函数定义头自身
            # 的 "token_slot_clear_locked(" 当成一次调用(第一版就误报了)。
            brace = code.rfind('{', m.start(), m.end())
            body_from_brace = code[brace: fend + 2]
            if helper + '(' not in body_from_brace:
                continue
            e = body_from_brace.find('portENTER_CRITICAL')
            c = body_from_brace.find(helper + '(')
            x = body_from_brace.find('portEXIT_CRITICAL')
            if not (e != -1 and e < c < x):
                ok10 = False
                detail10 += '%s() 里的调用不在临界区内 ' % m.group(1)
        detail10 = detail10 or '%s 的调用点均在临界区内' % helper
    check('A10 清槽辅助函数只在持锁状态下被调用', ok10, detail10)

    # ---------- 输出 ----------
    print('%s 静态审计\n' % os.path.basename(src))
    n_fail = 0
    for name, ok, detail in results:
        print('  %s  %s' % ('PASS' if ok else 'FAIL', name))
        if not ok and detail:
            print('        -> %s' % detail)
        if not ok:
            n_fail += 1
    print('\n%d 项:%d 通过 / %d 失败'
          % (len(results), len(results) - n_fail, n_fail))
    return 1 if n_fail else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
