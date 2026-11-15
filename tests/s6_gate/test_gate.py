#!/usr/bin/env python3
"""S6 阶段 B 门禁纯离线测试（可重复执行，不连设备、不动串口）。

覆盖：
  A. capture_state 三态判定（STOPPED / RUNNING / UNKNOWN，含 /proc 不可用）
  B. read_mac_flow 异常与恢复语义（stop 失败但已停、read 异常、run 异常、
     MAC 不符、证据写入失败、evidence_dir 缺失）
  C. anchor_validate / load_mac_record 严格校验矩阵

用法：.venv-bleak/bin/python tests/s6_gate/test_gate.py
"""

import json
import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "ble_p0"))

import port_identity as PI  # noqa: E402

PASS = 0
FAIL = 0


def ck(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  PASS  {name}")
    else:
        FAIL += 1
        print(f"  FAIL  {name}  {detail}")


def fake_proc(root, holders=(), pidfile_pid=None, unreadable=False):
    """构造假 /proc：holders 里的 pid 持有目标端口。"""
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)

    def mkproc(pid, hold=False):
        base = root / str(pid)
        d = base / "fd"
        d.mkdir(parents=True, exist_ok=True)
        # 扫描按 UID 过滤：桩进程必须是当前用户
        (base / "status").write_text(f"Name:\ttest\nUid:\t{os.getuid()}\t0\t0\t0\n")
        (base / "cmdline").write_bytes(b"python3\x00tools/capture_serial.py\x00")
        if hold:
            os.symlink("/dev/ttyUSB1", d / "3")

    for pid in holders:
        mkproc(pid, hold=True)
    if pidfile_pid is not None:
        mkproc(pidfile_pid, hold=False)
    return str(root)


def test_capture_state(tmp):
    print("[A] capture_state 三态")
    # A1 有进程持有端口 → RUNNING
    p = fake_proc(tmp / "a1", holders=(111,))
    st, inf = PI.capture_state("/dev/ttyUSB1", None, proc_root=p)
    ck("A1 有 holder → RUNNING", st == PI.RUNNING and 111 in inf["holders"], f"{st}")

    # A2 无 holder、pidfile 进程存活 → RUNNING
    p = fake_proc(tmp / "a2", pidfile_pid=222)
    log = Path(tmp / "a2.log"); log.write_text("x")
    Path(str(log) + ".pid").write_text("222")
    st, _ = PI.capture_state("/dev/ttyUSB1", str(log), proc_root=p)
    ck("A2 pid 存活但未持端口 → RUNNING", st == PI.RUNNING, st)

    # A3 无 holder、pid 不存在 → STOPPED
    p = fake_proc(tmp / "a3")
    log = Path(tmp / "a3.log"); log.write_text("x")
    Path(str(log) + ".pid").write_text("999999")
    st, _ = PI.capture_state("/dev/ttyUSB1", str(log), proc_root=p)
    ck("A3 无 holder 且 pid 不存在 → STOPPED", st == PI.STOPPED, st)

    # A4 proc_root 不可用 → UNKNOWN
    st, inf = PI.capture_state("/dev/ttyUSB1", None, proc_root=str(tmp / "no_such"))
    ck("A4 /proc 不可用 → UNKNOWN", st == PI.UNKNOWN, st)

    # A5 fd 目录 EACCES → UNKNOWN（不能 fail-open 成 STOPPED）
    st, inf = PI.capture_state("/dev/ttyUSB1", None, proc_root=p,
                               scanner=lambda r, pt: ([], "eacces_fd_dir:pid=5"))
    ck("A5 fd 目录不可读 → UNKNOWN", st == PI.UNKNOWN, st)

    # A6 readlink EPERM → UNKNOWN
    st, inf = PI.capture_state("/dev/ttyUSB1", None, proc_root=p,
                               scanner=lambda r, pt: ([], "eperm_readlink:pid=6"))
    ck("A6 readlink 不可读 → UNKNOWN", st == PI.UNKNOWN, st)

    # A7 扫描完整、无 holder → STOPPED（进程消失属可确认）
    st, inf = PI.capture_state("/dev/ttyUSB1", None, proc_root=p,
                               scanner=lambda r, pt: ([], None))
    ck("A7 扫描完整无 holder → STOPPED", st == PI.STOPPED, st)

    # A8 scanner 抛异常 → UNKNOWN
    st, inf = PI.capture_state("/dev/ttyUSB1", None, proc_root=p,
                               scanner=lambda r, pt: (_ for _ in ()).throw(RuntimeError("x")))
    ck("A8 scanner 异常 → UNKNOWN", st == PI.UNKNOWN, st)

    # A9 PID 文件存在但内容不可解析 → UNKNOWN（不能当作没有 PID）
    p9 = fake_proc(tmp / "a9")
    log9 = Path(tmp / "a9.log"); log9.write_text("x")
    Path(str(log9) + ".pid").write_text("not-a-pid")
    st, inf = PI.capture_state("/dev/ttyUSB1", str(log9), proc_root=p9,
                               scanner=lambda r, pt: ([], None))
    ck("A9 pidfile 不可解析 → UNKNOWN", st == PI.UNKNOWN, st)

    # A10 holder 存在 → RUNNING
    st, inf = PI.capture_state("/dev/ttyUSB1", None, proc_root=p,
                               scanner=lambda r, pt: ([123], None))
    ck("A10 有 holder → RUNNING", st == PI.RUNNING, st)


def test_read_mac_flow(tmp):
    print("[B] read_mac_flow 恢复语义（注入 fake，不碰真实串口）")
    CHAIN_OK = {k: True for k in ("running", "script_ok", "port_arg_ok",
                                  "log_arg_ok", "fd_holds_port", "fd_holds_log")}
    chain = lambda log_dir, name: dict(CHAIN_OK)   # noqa: E731

    class States:
        """按调用次序返回 STOPPED → RUNNING（stop 后 / start 后）。"""
        def __init__(self, seq):
            self.seq = list(seq)
        def __call__(self):
            st = self.seq.pop(0) if self.seq else PI.RUNNING
            return (st, {"port": "/dev/ttyUSB1", "holders": [], "reason": "test"})
    L = str(tmp / "logs"); Path(L).mkdir(parents=True, exist_ok=True)
    log = Path(L) / "serial_ttyUSB1.log"; log.write_text("rst:0x1 boot\n")

    mac_ok = lambda port: (True, "10:97:bd:d3:65:d8")          # noqa: E731
    mac_bad = lambda port: (True, "de:ad:be:ef:00:11")          # noqa: E731
    mac_raise = lambda port: (_ for _ in ()).throw(RuntimeError("boom"))  # noqa: E731
    run_ok = lambda port: (True, "ran")                          # noqa: E731
    run_raise = lambda port: (_ for _ in ()).throw(RuntimeError("boom"))  # noqa: E731

    def ctl_writer(path, stop_rc=0, start_rc=0, remove_pid=None):
        p = Path(path)
        p.write_text(
            "#!/bin/sh\n"
            f'[ "$1" = stop ] && {{ {("rm -f " + remove_pid) if remove_pid else "true"}; exit {stop_rc}; }}\n'
            f'[ "$1" = start ] && exit {start_rc}\n'
            "exit 1\n")
        os.chmod(p, 0o755)
        return str(p)

    # B1 正常：stop ok → read ok → run ok → start ok → 记录+证据
    ctl = ctl_writer(tmp / "b1.sh")
    ok, ev = PI.read_mac_flow("/dev/ttyUSB1", L, str(tmp / "b1.json"),
                              capture_ctl=ctl, evidence_dir=str(tmp / "ev1"),
                              read_mac=mac_ok, run_app=run_ok,
                              strict_restore=False, chain_check=chain,
                              state_fn=States([PI.STOPPED, PI.RUNNING]))
    ck("B1 正常 → ok/记录/证据",
       ok and ev["record_written"] and ev["evidence_written"],
       f"{ok} {ev.get('errors')}")

    # B2 read_mac 抛异常 → 仍恢复，ok=False，证据落盘
    ctl = ctl_writer(tmp / "b2.sh")
    ok, ev = PI.read_mac_flow("/dev/ttyUSB1", L, str(tmp / "b2.json"),
                              capture_ctl=ctl, evidence_dir=str(tmp / "ev2"),
                              read_mac=mac_raise, run_app=run_ok,
                              strict_restore=False, chain_check=chain,
                              state_fn=States([PI.STOPPED, PI.RUNNING]))
    ck("B2 read 异常 → 仍恢复且证据落盘",
       (not ok) and ev["run_app_ok"] and ev["evidence_written"]
       and any("read_mac" in e for e in ev["errors"]), str(ev.get("errors")))

    # B3 run_app 抛异常 → 仍重启录制
    ctl = ctl_writer(tmp / "b3.sh")
    ok, ev = PI.read_mac_flow("/dev/ttyUSB1", L, str(tmp / "b3.json"),
                              capture_ctl=ctl, evidence_dir=str(tmp / "ev3"),
                              read_mac=mac_ok, run_app=run_raise,
                              strict_restore=False, chain_check=chain,
                              state_fn=States([PI.STOPPED, PI.RUNNING]))
    ck("B3 run 异常 → 仍重启录制",
       (not ok) and ev["capture_restarted"] is True
       and any("run_app" in e for e in ev["errors"]), str(ev.get("errors")))

    # B4 MAC 不符 → 恢复后 ok=False
    ctl = ctl_writer(tmp / "b4.sh")
    ok, ev = PI.read_mac_flow("/dev/ttyUSB1", L, str(tmp / "b4.json"),
                              capture_ctl=ctl, evidence_dir=str(tmp / "ev4"),
                              read_mac=mac_bad, run_app=run_ok,
                              strict_restore=False, chain_check=chain,
                              state_fn=States([PI.STOPPED, PI.RUNNING]))
    ck("B4 MAC 不符 → 恢复但 ok=False", (not ok) and ev["mac_ok"] is False, str(ok))

    # B7 state=UNKNOWN → read_mac 调用次数必须为 0
    calls = {"n": 0}
    def mac_count(port):
        calls["n"] += 1
        return (True, "10:97:bd:d3:65:d8")
    ctl = ctl_writer(tmp / "b7.sh")
    ok, ev = PI.read_mac_flow("/dev/ttyUSB1", L, str(tmp / "b7.json"),
                              capture_ctl=ctl, evidence_dir=str(tmp / "ev7"),
                              read_mac=mac_count, run_app=run_ok,
                              strict_restore=False, chain_check=chain,
                              state_fn=States([PI.UNKNOWN]))
    ck("B7 UNKNOWN → read_mac 0 次且 ok=False",
       (not ok) and calls["n"] == 0, f"calls={calls['n']} ok={ok}")

    # B8 state=RUNNING（stop 未生效）→ read_mac 0 次
    calls2 = {"n": 0}
    def mac_count2(port):
        calls2["n"] += 1
        return (True, "10:97:bd:d3:65:d8")
    ok, ev = PI.read_mac_flow("/dev/ttyUSB1", L, str(tmp / "b8.json"),
                              capture_ctl=ctl, evidence_dir=str(tmp / "ev8"),
                              read_mac=mac_count2, run_app=run_ok,
                              strict_restore=False, chain_check=chain,
                              state_fn=States([PI.RUNNING]))
    ck("B8 RUNNING → read_mac 0 次且 ok=False",
       (not ok) and calls2["n"] == 0, f"calls={calls2['n']} ok={ok}")

    # B5 evidence_dir 缺失 → 拒绝
    ok, ev = PI.read_mac_flow("/dev/ttyUSB1", L, str(tmp / "b5.json"),
                              capture_ctl=ctl, evidence_dir=None,
                              read_mac=mac_ok, run_app=run_ok,
                              strict_restore=False)
    ck("B5 evidence_dir 缺失 → 拒绝",
       (not ok) and "evidence_dir_missing" in ev["errors"], str(ev.get("errors")))

    # B6 证据目录不可写 → ok=False
    ro = Path(tmp / "ro"); ro.mkdir()
    os.chmod(ro, 0o555)
    ok, ev = PI.read_mac_flow("/dev/ttyUSB1", L, str(tmp / "b6.json"),
                              capture_ctl=ctl, evidence_dir=str(ro / "sub"),
                              read_mac=mac_ok, run_app=run_ok,
                              strict_restore=False, chain_check=chain,
                              state_fn=States([PI.STOPPED, PI.RUNNING]))
    os.chmod(ro, 0o755)
    ck("B6 证据写入失败 → ok=False",
       (not ok) and str(ev.get("evidence_write_result")).startswith("fail"),
       str(ev.get("evidence_write_result")))


def test_anchor_and_record(tmp):
    print("[C] 锚点 / MAC 记录严格校验")
    L = "/tmp/logs"
    base = {"version": 1, "port": "/dev/ttyUSB1", "log_dir": L,
            "mac": "10:97:bd:d3:65:d8", "mac_ok": True,
            "checked_at_unix": int(__import__("time").time())}
    ck("C1 合法锚点 → 通过", PI.anchor_validate(base, "/dev/ttyUSB1", L)[0])
    for name, mut in [
        ("空对象", {}),
        ("version=True", {**base, "version": True}),
        ("version=2", {**base, "version": 2}),
        ("mac_ok=False", {**base, "mac_ok": False}),
        ("mac 伪造", {**base, "mac": "de:ad:be:ef:00:11"}),
        ("时间非数值", {**base, "checked_at_unix": "abc"}),
        ("过期", {**base, "checked_at_unix": 1}),
        ("端口不符", {**base, "port": "/dev/ttyUSB0"}),
        ("目录不符", {**base, "log_dir": "/tmp"}),
    ]:
        ck(f"C 锚点 {name} → 拒绝",
           not PI.anchor_validate(mut, "/dev/ttyUSB1", L)[0])

    # MAC 记录
    def rec(path, body):
        Path(path).write_text(json.dumps(body))
        return str(path)

    good = {"version": 1, "port": "/dev/ttyUSB1", "log_dir": L,
            "mac": "10:97:bd:d3:65:d8",
            "read_at_unix": int(__import__("time").time())}
    ck("C10 合法记录 → 通过",
       PI.load_mac_record(rec(tmp / "r1.json", good), "/dev/ttyUSB1", L)[0])
    for name, mut in [
        ("version=True", {**good, "version": True}),
        ("时间非数值", {**good, "read_at_unix": "x"}),
        ("mac 不符", {**good, "mac": "de:ad:be:ef:00:11"}),
        ("端口不符", {**good, "port": "/dev/ttyUSB0"}),
        ("缺字段", {"version": 1, "port": "/dev/ttyUSB1"}),
    ]:
        ck(f"C 记录 {name} → 拒绝",
           not PI.load_mac_record(rec(tmp / f"r_{name}.json", mut),
                                  "/dev/ttyUSB1", L)[0])


def main():
    tmp = Path(tempfile.mkdtemp(prefix="s6_gate_test_"))
    test_capture_state(tmp)
    test_read_mac_flow(tmp)
    test_anchor_and_record(tmp)
    print(f"\ns6_gate: {PASS} passed, {FAIL} failed")
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
