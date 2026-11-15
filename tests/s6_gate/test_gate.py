#!/usr/bin/env python3
"""S6 阶段 B 门禁纯离线测试（可重复执行，不连设备、不动串口）。

覆盖：
  A. 串口协作锁三态（按**串口**唯一，真实 flock）：
     RUNNING / COOPERATING_CAPTURE_STOPPED / UNKNOWN；
     同串口不同日志路径的采集必须互相判为 RUNNING；
     capture_serial.py 真实启动路径（--lock-probe）在锁被占时拒绝启动。
  B. read_mac_flow 授权与恢复语义：端口锁 + 操作者声明两个独立条件、
     stop 失败但已停、read 异常、run 异常、MAC 不符、证据写入失败。
  C. anchor_validate / load_mac_record 严格校验矩阵。
  D. validate_final_evidence 消费端校验（run_id/端口/MAC/旧终态/恢复字段）。

用法：.venv-bleak/bin/python tests/s6_gate/test_gate.py
"""

import fcntl
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "ble_p0"))

import port_identity as PI  # noqa: E402

PASS = 0
FAIL = 0
FAKE_PORTS = []          # 用完清理 /run/lock 下的锁文件


def ck(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  PASS  {name}")
    else:
        FAIL += 1
        print(f"  FAIL  {name}  {detail}")


def make_log(tmp, name):
    log = Path(tmp) / f"{name}.log"
    log.write_text("x")
    return str(log)


def test_port_lock_state(tmp):
    print("[A] 串口协作锁三态（真实 flock，按串口唯一）")

    # A1 端口锁被持有 → RUNNING（显式 lock_path，测试隔离）
    lp = str(tmp / "a1.lock")
    h = PI.PortLock(lock_path=lp)
    ok, why = h.acquire(0)
    st, inf = PI.capture_state("/dev/ttyUSB1", make_log(tmp, "a1"), lock_path=lp)
    ck("A1 端口锁被持有 → RUNNING",
       ok and st == PI.RUNNING and inf["port_lock"]["reason"] == "lock_held",
       f"{st} {inf['port_lock']}")
    h.release()

    # A2 端口锁空闲、无 pidfile → COOPERATING_CAPTURE_STOPPED
    lp = str(tmp / "a2.lock")
    PI.open_lock_file(lp).close()
    st, inf = PI.capture_state("/dev/ttyUSB1", make_log(tmp, "a2"), lock_path=lp)
    ck("A2 锁空闲 → COOPERATING_CAPTURE_STOPPED",
       st == PI.COOPERATING_STOPPED, st)
    ck("A2 不声称端口已空闲（port_free_proven=False）",
       inf["port_free_proven"] is False, str(inf["port_free_proven"]))

    # A3 锁文件不存在（从未启动）→ COOPERATING_CAPTURE_STOPPED
    log = make_log(tmp, "a3")
    st, _ = PI.capture_state("/dev/ttyUSB1", log, lock_path=str(tmp / "a3.lock"))
    ck("A3 锁文件缺失 → COOPERATING_CAPTURE_STOPPED",
       st == PI.COOPERATING_STOPPED, st)

    # A4 pidfile 存在但不可解析 → UNKNOWN
    lp = str(tmp / "a4.lock")
    PI.open_lock_file(lp).close()
    log = make_log(tmp, "a4")
    Path(log + ".pid").write_text("not-a-pid")
    st, inf = PI.capture_state("/dev/ttyUSB1", log, lock_path=lp)
    ck("A4 pidfile 不可解析 → UNKNOWN",
       st == PI.UNKNOWN and inf["unknown_reason"] == "pidfile_unparsable", st)

    # A5 锁空闲但 pid 进程存活 → RUNNING（保守）
    lp = str(tmp / "a5.lock")
    PI.open_lock_file(lp).close()
    log = make_log(tmp, "a5")
    Path(log + ".pid").write_text(str(os.getpid()))
    st, _ = PI.capture_state("/dev/ttyUSB1", log, lock_path=lp)
    ck("A5 锁空闲但 pid 存活 → RUNNING", st == PI.RUNNING, st)

    # A6 无 log_path → UNKNOWN（无法排除旧版采集的日志锁）
    lp = str(tmp / "a6.lock")
    PI.open_lock_file(lp).close()
    st, inf = PI.capture_state("/dev/ttyUSB1", None, lock_path=lp)
    ck("A6 无 log_path → UNKNOWN",
       st == PI.UNKNOWN and inf["unknown_reason"] == "no_log_path", st)

    # A7 注入 scanner 额外未知原因 → UNKNOWN
    lp = str(tmp / "a7.lock")
    PI.open_lock_file(lp).close()
    st, _ = PI.capture_state("/dev/ttyUSB1", make_log(tmp, "a7"), lock_path=lp,
                             scanner=lambda r, pt: ([], "injected_unknown"))
    ck("A7 注入未知原因 → UNKNOWN", st == PI.UNKNOWN, st)

    # A8 同串口、**不同日志路径**：另一路径的采集必须被判 RUNNING
    fake = "/dev/ttyUSB_T8"
    FAKE_PORTS.append(fake)
    real_lp = PI.port_lock_path(fake)
    h = PI.PortLock(port=fake)
    ok, _ = h.acquire(0)
    other_log = str(tmp / "other_dir_a" / "serial.log")
    Path(other_log).parent.mkdir(parents=True, exist_ok=True)
    st, inf = PI.capture_state(fake, other_log)          # 查询方用另一条日志路径
    ck("A8 同串口不同日志路径 → RUNNING（锁按串口而非日志）",
       ok and st == PI.RUNNING and inf["port_lock"]["path"] == real_lp,
       f"{st} {inf['port_lock']}")
    ck("A8 该路径的日志锁并不存在（判定不依赖日志锁）",
       not Path(other_log + ".lock").exists())
    h.release()
    st, _ = PI.capture_state(fake, other_log)
    ck("A8 释放后 → COOPERATING_CAPTURE_STOPPED", st == PI.COOPERATING_STOPPED, st)

    # A9 capture_serial.py 真实启动路径：锁被占 → rc=3；释放后 → rc=0
    probe = [sys.executable, str(ROOT / "tools" / "capture_serial.py"),
             "--lock-probe", fake, str(tmp / "probe_dir" / "serial.log")]
    Path(tmp / "probe_dir").mkdir(parents=True, exist_ok=True)
    h = PI.PortLock(port=fake)
    h.acquire(0)
    r = subprocess.run(probe, capture_output=True, text=True, timeout=60)
    ck("A9 端口锁被占 → capture 拒绝启动（rc=3，未开串口）",
       r.returncode == 3 and "拒绝启动" in (r.stdout + r.stderr),
       f"rc={r.returncode} out={(r.stdout + r.stderr)[:120]}")
    h.release()
    r = subprocess.run(probe, capture_output=True, text=True, timeout=60)
    ck("A9 锁空闲 → capture 可取锁（rc=0）",
       r.returncode == 0 and "lock-probe ok" in r.stdout,
       f"rc={r.returncode} out={(r.stdout + r.stderr)[:120]}")

    # 清理测试在 /run/lock 留下的锁文件
    for p in [real_lp, str(tmp / "probe_dir" / "serial.log.lock")]:
        try:
            os.unlink(p)
        except OSError:
            pass


def test_read_mac_flow(tmp):
    print("[B] read_mac_flow 授权与恢复语义（注入 fake，不碰真实串口）")
    CHAIN_OK = {k: True for k in ("running", "script_ok", "port_arg_ok",
                                  "log_arg_ok", "fd_holds_port", "fd_holds_log")}
    chain = lambda log_dir, name: dict(CHAIN_OK)   # noqa: E731

    class States:
        """按调用次序返回给定状态（默认 RUNNING）。"""
        def __init__(self, seq):
            self.seq = list(seq)
        def __call__(self):
            st = self.seq.pop(0) if self.seq else PI.RUNNING
            return (st, {"port": "/dev/ttyUSB1", "port_lock": {"reason": "test"}})

    L = str(tmp / "logs"); Path(L).mkdir(parents=True, exist_ok=True)
    log = Path(L) / "serial_ttyUSB1.log"; log.write_text("rst:0x1 boot\n")

    mac_ok = lambda port: (True, "10:97:bd:d3:65:d8")          # noqa: E731
    mac_bad = lambda port: (True, "de:ad:be:ef:00:11")          # noqa: E731
    mac_raise = lambda port: (_ for _ in ()).throw(RuntimeError("boom"))  # noqa: E731
    run_ok = lambda port: (True, "ran")                          # noqa: E731
    run_raise = lambda port: (_ for _ in ()).throw(RuntimeError("boom"))  # noqa: E731

    def ctl_writer(path, stop_rc=0, start_rc=0):
        p = Path(path)
        p.write_text(
            "#!/bin/sh\n"
            f'[ "$1" = stop ] && exit {stop_rc}\n'
            f'[ "$1" = start ] && exit {start_rc}\n'
            "exit 1\n")
        os.chmod(p, 0o755)
        return str(p)

    def run_case(tag, states, **kw):
        lp = str(tmp / f"lk_{tag}")
        args = dict(evidence_dir=str(tmp / f"ev_{tag}"), lock_path=lp,
                    strict_restore=False, chain_check=chain, state_fn=States(states),
                    capture_ctl=ctl_writer(tmp / f"{tag}.sh"))
        args.update(kw)
        return PI.read_mac_flow("/dev/ttyUSB1", L, str(tmp / f"{tag}.json"), **args)

    # B0 缺操作者声明 → 不读 MAC、不取端口锁，但仍重启录制
    calls = {"n": 0}
    def mac_count(port):
        calls["n"] += 1
        return (True, "10:97:bd:d3:65:d8")
    ok, ev = run_case("b0", [PI.COOPERATING_STOPPED, PI.RUNNING], read_mac=mac_count,
                      run_app=run_ok)
    ck("B0 无操作者声明 → read_mac 0 次且 ok=False",
       (not ok) and calls["n"] == 0
       and "external_holder_not_attested" in ev["errors"], str(ev.get("errors")))
    ck("B0 仍重启录制（采集已停必须恢复）",
       ev["capture_restarted"] is True and ev["state_after_restore"] == PI.RUNNING,
       str(ev.get("state_after_restore")))

    # B1 正常：声明 + 端口锁 + stop ok → read ok → run ok → start ok
    ok, ev = run_case("b1", [PI.COOPERATING_STOPPED, PI.RUNNING], read_mac=mac_ok,
                      run_app=run_ok, external_holder_attested=True,
                      attestation_note="test")
    ck("B1 正常 → ok/记录/证据/端口锁",
       ok and ev["record_written"] and ev["evidence_written"]
       and ev["port_lock"]["acquired"] is True
       and ev["port_free_proven"] is False,
       f"{ok} {ev.get('errors')} {ev.get('port_lock')}")
    fin_ok, fin_why, _ = PI.validate_final_evidence(
        str(tmp / "ev_b1"), ev["run_id"], "/dev/ttyUSB1", require_restore=True,
        require_new_boot=False, expected_lock_path=str(tmp / "lk_b1"))
    ck("B1 证据可被消费端校验通过", fin_ok, str(fin_why))

    # B11 记录与证据同落在**尚不存在**的证据目录（门禁默认形态）→ 仍成功
    evdir = str(tmp / "ev_new_b11" / "nested")
    ok, ev = PI.read_mac_flow("/dev/ttyUSB1", L, str(Path(evdir) / "mac_record.json"),
                              capture_ctl=ctl_writer(tmp / "b11.sh"),
                              evidence_dir=evdir, lock_path=str(tmp / "lk_b11"),
                              read_mac=mac_ok, run_app=run_ok, strict_restore=False,
                              chain_check=chain,
                              state_fn=States([PI.COOPERATING_STOPPED, PI.RUNNING]),
                              external_holder_attested=True)
    ck("B11 证据目录尚不存在 → 记录与证据均写出且 ok",
       ok and ev["record_written"] and ev["evidence_written"],
       str(ev.get("errors")))

    # B12 同一证据目录二次调用 → 拒绝，且不读 MAC（目录防复用对 read 入口同样生效）
    calls = {"n": 0}
    def mac_count12(port):
        calls["n"] += 1
        return (True, "10:97:bd:d3:65:d8")
    evdir12 = str(tmp / "ev_b12")
    ok1, ev1 = PI.read_mac_flow("/dev/ttyUSB1", L, str(Path(evdir12) / "r.json"),
                                capture_ctl=ctl_writer(tmp / "b12.sh"),
                                evidence_dir=evdir12, lock_path=str(tmp / "lk_b12a"),
                                read_mac=mac_ok, run_app=run_ok, strict_restore=False,
                                chain_check=chain,
                                state_fn=States([PI.COOPERATING_STOPPED, PI.RUNNING]),
                                external_holder_attested=True,
                                attestation_note="test")
    ok2, ev2 = PI.read_mac_flow("/dev/ttyUSB1", L, str(Path(evdir12) / "r2.json"),
                                capture_ctl=ctl_writer(tmp / "b12b.sh"),
                                evidence_dir=evdir12, lock_path=str(tmp / "lk_b12b"),
                                read_mac=mac_count12, run_app=run_ok, strict_restore=False,
                                chain_check=chain,
                                state_fn=States([PI.COOPERATING_STOPPED, PI.RUNNING]),
                                external_holder_attested=True,
                                attestation_note="test")
    ck("B12 同一证据目录二次调用 → 拒绝且 read_mac 0 次",
       ok1 and (not ok2) and calls["n"] == 0
       and any("evidence_dir" in e for e in ev2["errors"]), str(ev2.get("errors")))

    # B13 prepared=True（rollback_gate 路径）+ 已建空目录 → 正常
    evdir13 = str(tmp / "ev_b13"); Path(evdir13).mkdir()
    ok, ev = PI.read_mac_flow("/dev/ttyUSB1", L, str(Path(evdir13) / "r.json"),
                              capture_ctl=ctl_writer(tmp / "b13.sh"),
                              evidence_dir=evdir13, lock_path=str(tmp / "lk_b13"),
                              read_mac=mac_ok, run_app=run_ok, strict_restore=False,
                              chain_check=chain,
                              state_fn=States([PI.COOPERATING_STOPPED, PI.RUNNING]),
                              external_holder_attested=True, attestation_note="test",
                              evidence_dir_prepared=True)
    ck("B13 prepared=True + 空目录 → 正常", ok and ev["evidence_written"],
       str(ev.get("errors")))

    # B14 prepared=True 但目录已有旧产物 → 拒绝，read_mac 0 次
    calls = {"n": 0}
    def mac_count14(port):
        calls["n"] += 1
        return (True, "10:97:bd:d3:65:d8")
    evdir14 = str(tmp / "ev_b14"); Path(evdir14).mkdir()
    (Path(evdir14) / PI.FINAL_EVIDENCE_NAME).write_text("{}")
    ok, ev = PI.read_mac_flow("/dev/ttyUSB1", L, str(Path(evdir14) / "r.json"),
                              capture_ctl=ctl_writer(tmp / "b14.sh"),
                              evidence_dir=evdir14, lock_path=str(tmp / "lk_b14"),
                              read_mac=mac_count14, run_app=run_ok, strict_restore=False,
                              chain_check=chain, state_fn=States([PI.COOPERATING_STOPPED]),
                              external_holder_attested=True,
                              evidence_dir_prepared=True)
    ck("B14 prepared=True 但已有旧产物 → 拒绝且 read_mac 0 次",
       (not ok) and calls["n"] == 0
       and "evidence_dir_prepared_invalid" in ev["errors"], str(ev.get("errors")))

    # B2 read_mac 抛异常 → 仍恢复，ok=False，证据落盘，端口锁已释放
    ok, ev = run_case("b2", [PI.COOPERATING_STOPPED, PI.RUNNING], read_mac=mac_raise,
                      run_app=run_ok, external_holder_attested=True)
    ck("B2 read 异常 → 仍恢复且证据落盘",
       (not ok) and ev["run_app_ok"] and ev["evidence_written"]
       and any("read_mac" in e for e in ev["errors"]), str(ev.get("errors")))
    lp2 = str(tmp / "lk_b2")
    h = PI.PortLock(lock_path=lp2)
    rel_ok, _ = h.acquire(0)
    h.release()
    ck("B2 异常后端口锁已释放", rel_ok)

    # B3 run_app 抛异常 → 仍重启录制
    ok, ev = run_case("b3", [PI.COOPERATING_STOPPED, PI.RUNNING], read_mac=mac_ok,
                      run_app=run_raise, external_holder_attested=True)
    ck("B3 run 异常 → 仍重启录制",
       (not ok) and ev["capture_restarted"] is True
       and any("run_app" in e for e in ev["errors"]), str(ev.get("errors")))

    # B4 MAC 不符 → 恢复但 ok=False
    ok, ev = run_case("b4", [PI.COOPERATING_STOPPED, PI.RUNNING], read_mac=mac_bad,
                      run_app=run_ok, external_holder_attested=True)
    ck("B4 MAC 不符 → 恢复但 ok=False", (not ok) and ev["mac_ok"] is False, str(ok))

    # B7 state=UNKNOWN → read_mac 0 次、不取锁、不重启录制（终止态）
    calls = {"n": 0}
    def mac_count7(port):
        calls["n"] += 1
        return (True, "10:97:bd:d3:65:d8")
    ok, ev = run_case("b7", [PI.UNKNOWN], read_mac=mac_count7, run_app=run_ok,
                      external_holder_attested=True)
    ck("B7 UNKNOWN → read_mac 0 次、不取端口锁、不重启录制",
       (not ok) and calls["n"] == 0 and ev["port_lock"]["acquired"] is False
       and ev["capture_restarted"] is False,
       f"calls={calls['n']} ok={ok} {ev.get('port_lock')}")

    # B8 state=RUNNING（stop 未生效）→ read_mac 0 次
    calls2 = {"n": 0}
    def mac_count8(port):
        calls2["n"] += 1
        return (True, "10:97:bd:d3:65:d8")
    ok, ev = run_case("b8", [PI.RUNNING], read_mac=mac_count8, run_app=run_ok,
                      external_holder_attested=True)
    ck("B8 RUNNING → read_mac 0 次且 ok=False",
       (not ok) and calls2["n"] == 0, f"calls={calls2['n']} ok={ok}")

    # B10 端口锁被他人持有 → 不读 MAC、ok=False，但仍重启录制
    calls = {"n": 0}
    def mac_count10(port):
        calls["n"] += 1
        return (True, "10:97:bd:d3:65:d8")
    lp10 = str(tmp / "lk_b10")
    h = PI.PortLock(lock_path=lp10)
    h.acquire(0)
    ok, ev = run_case("b10", [PI.COOPERATING_STOPPED, PI.RUNNING], read_mac=mac_count10,
                      run_app=run_ok, external_holder_attested=True, lock_timeout_s=0.5)
    ck("B10 端口锁被占 → read_mac 0 次且 ok=False",
       (not ok) and calls["n"] == 0
       and ev["port_lock"]["reason"] == "held_by_other", f"{ev.get('port_lock')}")
    ck("B10 仍重启录制（不让采集停在已停状态）",
       ev["capture_restarted"] is True, str(ev.get("state_after_restore")))
    h.release()

    # B5 evidence_dir 缺失 → 拒绝
    ok, ev = PI.read_mac_flow("/dev/ttyUSB1", L, str(tmp / "b5.json"),
                              capture_ctl=ctl_writer(tmp / "b5.sh"),
                              evidence_dir=None, lock_path=str(tmp / "lk_b5"),
                              read_mac=mac_ok, run_app=run_ok,
                              strict_restore=False, external_holder_attested=True)
    ck("B5 evidence_dir 缺失 → 拒绝",
       (not ok) and "evidence_dir_missing" in ev["errors"], str(ev.get("errors")))

    # B6 证据目录不可写 → ok=False
    ro = Path(tmp / "ro"); ro.mkdir()
    os.chmod(ro, 0o555)
    ok, ev = run_case("b6", [PI.COOPERATING_STOPPED, PI.RUNNING], read_mac=mac_ok,
                      run_app=run_ok, external_holder_attested=True,
                      evidence_dir=str(ro / "sub"))
    os.chmod(ro, 0o755)
    ck("B6 证据写入失败 → ok=False",
       (not ok) and str(ev.get("evidence_write_result")).startswith("fail"),
       str(ev.get("evidence_write_result")))


def test_anchor_and_record(tmp):
    print("[C] 锚点 / MAC 记录严格校验")
    L = "/tmp/logs"
    base = {"version": 1, "port": "/dev/ttyUSB1", "log_dir": L,
            "mac": "10:97:bd:d3:65:d8", "mac_ok": True,
            "checked_at_unix": int(time.time())}
    ck("C1 合法锚点 → 通过", PI.anchor_validate(base, "/dev/ttyUSB1", L)[0])
    for name, mut in [
        ("空对象", {}),
        ("version=True", {**base, "version": True}),
        ("version=2", {**base, "version": 2}),
        ("mac_ok=False", {**base, "mac_ok": False}),
        ("mac 伪造", {**base, "mac": "de:ad:be:ef:00:11"}),
        ("时间非数值", {**base, "checked_at_unix": "abc"}),
        ("时间 NaN", {**base, "checked_at_unix": float("nan")}),
        ("时间 +Inf", {**base, "checked_at_unix": float("inf")}),
        ("时间 -Inf", {**base, "checked_at_unix": float("-inf")}),
        ("时间 bool", {**base, "checked_at_unix": True}),
        ("过期", {**base, "checked_at_unix": 1}),
        ("端口不符", {**base, "port": "/dev/ttyUSB0"}),
        ("目录不符", {**base, "log_dir": "/tmp"}),
    ]:
        ck(f"C 锚点 {name} → 拒绝",
           not PI.anchor_validate(mut, "/dev/ttyUSB1", L)[0])

    def rec(path, body):
        Path(path).write_text(json.dumps(body))
        return str(path)

    good = {"version": 1, "port": "/dev/ttyUSB1", "log_dir": L,
            "mac": "10:97:bd:d3:65:d8",
            "read_at_unix": int(time.time())}
    ck("C10 合法记录 → 通过",
       PI.load_mac_record(rec(tmp / "r1.json", good), "/dev/ttyUSB1", L)[0])
    for name, mut in [
        ("version=True", {**good, "version": True}),
        ("时间非数值", {**good, "read_at_unix": "x"}),
        ("时间 NaN", {**good, "read_at_unix": float("nan")}),
        ("时间 +Inf", {**good, "read_at_unix": float("inf")}),
        ("时间 -Inf", {**good, "read_at_unix": float("-inf")}),
        ("mac 不符", {**good, "mac": "de:ad:be:ef:00:11"}),
        ("端口不符", {**good, "port": "/dev/ttyUSB0"}),
        ("缺字段", {"version": 1, "port": "/dev/ttyUSB1"}),
    ]:
        ck(f"C 记录 {name} → 拒绝",
           not PI.load_mac_record(rec(tmp / f"r_{name}.json", mut),
                                  "/dev/ttyUSB1", L)[0])


def test_final_evidence(tmp):
    print("[D] 最终证据消费端校验（唯一关联本轮）")
    good = {
        "run_id": "20261115_120000_abcd1234",
        "port": "/dev/ttyUSB1",
        "mac": "10:97:bd:d3:65:d8",
        "mac_ok": True,
        "evidence_write_result": "ok",
        "state_after_stop": PI.COOPERATING_STOPPED,
        "state_after_restore": PI.RUNNING,
        "run_app_ok": True,
        "capture_restarted": True,
        "new_boot": True,
        "record_written": True,
        "port_lock": {"path": "/run/lock/iothub_serial_ttyUSB1.lock",
                      "acquired": True, "reason": "acquired"},
        "external_holder_attested": True,
        "attestation_note": "测试声明：无外部工具占用",
        "port_free_proven": False,
        "errors": [],
        "at": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "at_unix": int(time.time()),
    }

    def write_case(name, body):
        d = Path(tmp) / f"d_{name}"
        d.mkdir(parents=True, exist_ok=True)
        (d / PI.FINAL_EVIDENCE_NAME).write_text(json.dumps(body, ensure_ascii=False))
        return str(d)

    def reason_has(rs, needle):
        return any(needle in r for r in rs)

    d = write_case("ok", good)
    ok, rs, _ = PI.validate_final_evidence(d, good["run_id"], "/dev/ttyUSB1")
    ck("D1 全字段匹配 → 通过", ok, str(rs))

    d = write_case("runid", {**good, "run_id": "20261115_115900_deadbeef"})
    ok, rs, _ = PI.validate_final_evidence(d, good["run_id"], "/dev/ttyUSB1")
    ck("D2 run_id 不符 → 拒绝", (not ok) and reason_has(rs, "run_id"), str(rs))

    d = write_case("port", {**good, "port": "/dev/ttyUSB0"})
    ok, rs, _ = PI.validate_final_evidence(d, good["run_id"], "/dev/ttyUSB1")
    ck("D3 端口不符 → 拒绝", (not ok) and reason_has(rs, "port"), str(rs))

    d = write_case("mac", {**good, "mac": "de:ad:be:ef:00:11"})
    ok, rs, _ = PI.validate_final_evidence(d, good["run_id"], "/dev/ttyUSB1")
    ck("D4 MAC 不符 → 拒绝", (not ok) and reason_has(rs, "mac"), str(rs))

    # D5 旧 final 文件（上一轮遗留）不得冒充本轮
    d = write_case("stale", {**good, "run_id": "20261114_090000_0000aaaa",
                             "at_unix": int(time.time()) - 3 * 86400,
                             "at": time.strftime(
                                 "%Y-%m-%dT%H:%M:%S",
                                 time.localtime(time.time() - 3 * 86400))})
    ok, rs, _ = PI.validate_final_evidence(d, good["run_id"], "/dev/ttyUSB1")
    ck("D5 旧终态证据（旧 run_id/旧时间）→ 拒绝",
       (not ok) and reason_has(rs, "run_id") and reason_has(rs, "过期"), str(rs))

    for tag, mut in [
        ("restore_unknown", {**good, "state_after_restore": PI.UNKNOWN}),
        ("run_fail", {**good, "run_app_ok": False}),
        ("restart_fail", {**good, "capture_restarted": False}),
        ("no_boot", {**good, "new_boot": False}),
        ("no_record", {**good, "record_written": False}),
    ]:
        d = write_case(tag, mut)
        ok, rs, _ = PI.validate_final_evidence(d, good["run_id"], "/dev/ttyUSB1")
        ck(f"D6 恢复字段异常 {tag} → 拒绝", not ok, str(rs))

    for tag, mut in [
        ("no_lock", {**good, "port_lock": {"acquired": False, "reason": "held_by_other"}}),
        ("no_attest", {**good, "external_holder_attested": False}),
        ("fake_free", {**good, "port_free_proven": True}),
        ("errors", {**good, "errors": ["read_mac:RuntimeError:boom"]}),
        ("write_fail", {**good, "evidence_write_result": "fail:OSError"}),
        ("stop_running", {**good, "state_after_stop": PI.RUNNING}),
    ]:
        d = write_case(tag, mut)
        ok, rs, _ = PI.validate_final_evidence(d, good["run_id"], "/dev/ttyUSB1")
        ck(f"D7 授权/写入字段异常 {tag} → 拒绝", not ok, str(rs))

    # D8 文件缺失 / 非法 JSON
    d = Path(tmp) / "d_missing"; d.mkdir()
    ok, rs, _ = PI.validate_final_evidence(str(d), good["run_id"], "/dev/ttyUSB1")
    ck("D8 最终证据缺失 → 拒绝", (not ok) and reason_has(rs, "不存在"), str(rs))
    (d / PI.FINAL_EVIDENCE_NAME).write_text("{not json")
    ok, rs, _ = PI.validate_final_evidence(str(d), good["run_id"], "/dev/ttyUSB1")
    ck("D8 最终证据不可解析 → 拒绝", (not ok) and reason_has(rs, "不可解析"), str(rs))

    # D9 声明说明 / 锁路径 / Unix 时间戳
    for tag, mut in [
        ("note_empty", {**good, "attestation_note": "   "}),
        ("note_missing", {k: v for k, v in good.items() if k != "attestation_note"}),
        ("lock_path", {**good, "port_lock": {**good["port_lock"],
                                            "path": "/tmp/evil.lock"}}),
        ("no_at_unix", {k: v for k, v in good.items() if k != "at_unix"}),
        ("bad_at_unix", {**good, "at_unix": "abc"}),
        ("stale_at_unix", {**good, "at_unix": int(time.time()) - 3 * 86400}),
        ("nan_at_unix", {**good, "at_unix": float("nan")}),
        ("inf_at_unix", {**good, "at_unix": float("inf")}),
        ("neg_inf_at_unix", {**good, "at_unix": float("-inf")}),
        ("bool_at_unix", {**good, "at_unix": True}),
        ("str_at_unix", {**good, "at_unix": "1794741630"}),
        ("future_at_unix", {**good, "at_unix": int(time.time()) + 3600}),
        ("just_over_240", {**good, "at_unix": int(time.time()) - 241 * 60}),
    ]:
        d = write_case(tag, mut)
        ok, rs, _ = PI.validate_final_evidence(d, good["run_id"], "/dev/ttyUSB1")
        ck(f"D9 声明/锁路径/时间异常 {tag} → 拒绝", not ok, str(rs))

    # D10 显式给出本轮实际锁路径时不匹配 → 拒绝
    d = write_case("lock_expect", good)
    ok, rs, _ = PI.validate_final_evidence(
        d, good["run_id"], "/dev/ttyUSB1",
        expected_lock_path="/run/lock/iothub_serial_ttyUSB9.lock")
    ck("D10 expected_lock_path 不符 → 拒绝",
       (not ok) and reason_has(rs, "端口锁路径不符"), str(rs))


def test_recovery_path(tmp):
    print("[F] 恢复路径：FI 不可启动时的门禁放宽与烧录互斥")
    import rollback_gate as RG

    CHAIN_OK = {k: True for k in RG.CHAIN_KEYS}
    LOG_OK = {"fresh": True, "boot_marker": True, "age_sec": 1}
    cap_bad = {k: False for k in RG.CHAIN_KEYS}
    log_bad = {"fresh": False, "boot_marker": False, "age_sec": 9999}

    hard, soft, passes = RG.evaluate_preflight(CHAIN_OK, LOG_OK, False)
    ck("F1 正常模式全满足 → 无否决",
       not hard and not soft and len(passes) == len(RG.CHAIN_KEYS) + 2,
       f"hard={hard} passes={len(passes)}")

    hard, soft, passes = RG.evaluate_preflight(cap_bad, log_bad, False)
    ck("F2 正常模式：链/日志不满足 → 硬否决",
       len(hard) == len(RG.CHAIN_KEYS) + 2 and not soft, f"hard={len(hard)}")

    hard, soft, passes = RG.evaluate_preflight(cap_bad, log_bad, True)
    ck("F3 恢复模式：不满足项全部转软记录，不否决",
       not hard and len(soft) == len(RG.CHAIN_KEYS) + 2 and not passes,
       f"hard={len(hard)} soft={len(soft)}")

    # F4/F5 require_chain 语义（用 fake，不碰串口）
    class States:
        def __init__(self, seq):
            self.seq = list(seq)
        def __call__(self):
            st = self.seq.pop(0) if self.seq else PI.RUNNING
            return (st, {"port": "/dev/ttyUSB1", "port_lock": {"reason": "test"}})

    def chain_bad(log_dir, name):
        return {k: False for k in RG.CHAIN_KEYS}

    def ctl_writer(path):
        p = Path(path)
        p.write_text("#!/bin/sh\nexit 0\n")
        os.chmod(p, 0o755)
        return str(p)

    L2 = str(tmp / "logs2"); Path(L2).mkdir(parents=True, exist_ok=True)
    (Path(L2) / "serial_ttyUSB1.log").write_text("x")
    mac_ok = lambda port: (True, "10:97:bd:d3:65:d8")   # noqa: E731
    run_ok = lambda port: (True, "ran")                  # noqa: E731

    calls = {"n": 0}
    def mac_count(port):
        calls["n"] += 1
        return (True, "10:97:bd:d3:65:d8")

    ok, ev = PI.read_mac_flow(
        "/dev/ttyUSB1", L2, str(tmp / "f4.json"),
        capture_ctl=ctl_writer(tmp / "f4.sh"), evidence_dir=str(tmp / "ev_f4"),
        lock_path=str(tmp / "lk_f4"), read_mac=mac_ok, run_app=run_ok,
        strict_restore=False, chain_check=chain_bad, state_fn=States(
            [PI.COOPERATING_STOPPED, PI.RUNNING]),
        external_holder_attested=True, attestation_note="test",
        require_chain=False)
    ck("F4 恢复模式：链不完整仍读 MAC（互斥由端口锁保证）",
       ok and ev["mac_ok"] is True and ev["port_lock"]["acquired"] is True,
       f"{ok} {ev.get('errors')}")

    ok, ev = PI.read_mac_flow(
        "/dev/ttyUSB1", L2, str(tmp / "f5.json"),
        capture_ctl=ctl_writer(tmp / "f5.sh"), evidence_dir=str(tmp / "ev_f5"),
        lock_path=str(tmp / "lk_f5"), read_mac=mac_count, run_app=run_ok,
        strict_restore=False, chain_check=chain_bad, state_fn=States([PI.RUNNING]),
        external_holder_attested=True, attestation_note="test",
        require_chain=True)
    ck("F5 正常模式：链不完整 → 不读 MAC、不取锁",
       (not ok) and calls["n"] == 0 and ev["port_lock"]["acquired"] is False,
       f"calls={calls['n']} {ev.get('port_lock')}")

    # F6 flash_frozen.sh 自带端口锁守卫（静态守卫；行为验证见运行手册）
    txt = (ROOT / "tools" / "flash_frozen.sh").read_text(encoding="utf-8")
    ck("F6 flash_frozen.sh 在 esptool 之前取端口锁并拒绝占用",
       "flock -n 9" in txt and "exit 6" in txt
       and txt.index("flock -n 9") < txt.index("-m esptool --chip"))


def test_gate_recovery_wiring(tmp):
    print("[G] rollback_gate 恢复模式接线（fake flow 写真实最终证据，不碰串口）")
    import rollback_gate as RG

    def write_bin():
        p = Path(tmp) / "rb.bin"; p.write_bytes(b"\x00" * 64)
        return str(p), hashlib.sha256(p.read_bytes()).hexdigest()

    LOGS = str(tmp / "logs3")
    Path(LOGS).mkdir(parents=True, exist_ok=True)

    def write_anchor():
        p = Path(tmp) / "anchor.json"
        p.write_text(json.dumps({
            "version": 1, "port": "/dev/ttyUSB1",
            "log_dir": LOGS, "mac": "10:97:bd:d3:65:d8", "mac_ok": True,
            "checked_at_unix": int(time.time())}))
        return str(p)

    seen = []
    state = {"new_boot": None, "run_app_ok": True}

    def fake_flow(*a, **kw):
        seen.append(kw)
        d = Path(kw["evidence_dir"])
        ev = {"run_id": kw["run_id"], "port": "/dev/ttyUSB1",
              "mac": "10:97:bd:d3:65:d8", "mac_ok": True,
              "evidence_write_result": "ok",
              "state_after_stop": PI.COOPERATING_STOPPED,
              "state_after_restore": PI.RUNNING,
              "run_app_ok": state["run_app_ok"], "capture_restarted": True,
              "new_boot": state["new_boot"], "record_written": True,
              "port_lock": {"path": PI.port_lock_path("/dev/ttyUSB1"),
                            "acquired": True, "reason": "acquired"},
              "external_holder_attested": True, "attestation_note": "test",
              "port_free_proven": False, "errors": [], "diagnostics": [],
              "at": time.strftime("%Y-%m-%dT%H:%M:%S"),
              "at_unix": int(time.time())}
        (d / PI.FINAL_EVIDENCE_NAME).write_text(json.dumps(ev), encoding="utf-8")
        return (True, ev)

    def argv(evdir, extra=()):
        return (["rollback_gate.py", "--port", "/dev/ttyUSB1", "--log-dir", LOGS,
                 "--anchor-file", anchor, "--bin", binp,
                 "--expect-bin-sha256", bsha, "--evidence-dir", evdir,
                 "--attest-no-external-holder", "test"] + list(extra))

    orig = (RG.inspect_capture, RG.inspect_log, RG.PI.read_mac_flow,
            RG.usb_events_since, sys.argv)
    try:
        RG.inspect_capture = lambda *a, **k: {x: False for x in RG.CHAIN_KEYS}
        RG.inspect_log = lambda *a, **k: ({}, {"fresh": False, "boot_marker": False,
                                               "age_sec": 9999})
        RG.PI.read_mac_flow = fake_flow
        RG.usb_events_since = lambda *a, **k: (True, "none")
        binp, bsha = write_bin()
        anchor = write_anchor()

        # G1 正常模式：链/日志不满足 → rc=1，根本不调用 read_mac_flow
        sys.argv = argv(str(tmp / "g1_ev"))
        rc = RG.main()
        ck("G1 正常模式链不合格 → rc=1 且不读 MAC",
           rc == 1 and not seen, f"rc={rc} seen={len(seen)}")

        # G2 恢复模式：同样不合格 → rc=0；真实消费端校验只放开 new_boot
        state["new_boot"], state["run_app_ok"] = None, True
        sys.argv = argv(str(tmp / "g2_ev"), ["--recovery-mode"])
        rc = RG.main()
        kw = seen[-1] if seen else {}
        ck("G2 恢复模式 → rc=0 且 require_chain/strict_restore 放宽、仅 new_boot 免检",
           rc == 0 and kw.get("require_chain") is False
           and kw.get("strict_restore") is False, f"rc={rc} kw={kw}")

        # G3 恢复模式下 run_app 失败仍判失败（其余恢复字段不放宽）
        state["new_boot"], state["run_app_ok"] = None, False
        sys.argv = argv(str(tmp / "g3_ev"), ["--recovery-mode"])
        rc = RG.main()
        ck("G3 恢复模式但 run_app_ok=False → rc=1（恢复字段照查）", rc == 1, f"rc={rc}")
        state["run_app_ok"] = True

        # G4 恢复模式：检查函数**抛异常**也降级为诊断，仍进入读 MAC
        n_before = len(seen)
        RG.inspect_capture = lambda *a, **k: (_ for _ in ()).throw(OSError("boom"))
        RG.inspect_log = lambda *a, **k: (_ for _ in ()).throw(OSError("boom"))
        sys.argv = argv(str(tmp / "g4_ev"), ["--recovery-mode"])
        rc = RG.main()
        ck("G4 恢复模式检查异常 → rc=0 且仍调用 read_mac_flow",
           rc == 0 and len(seen) == n_before + 1, f"rc={rc} seen={len(seen)}")

        # G5 正常模式：检查函数抛异常仍是硬失败
        n_before = len(seen)
        sys.argv = argv(str(tmp / "g5_ev"))
        rc = RG.main()
        ck("G5 正常模式检查异常 → rc=1 且不读 MAC",
           rc == 1 and len(seen) == n_before, f"rc={rc} seen={len(seen)}")
    finally:
        (RG.inspect_capture, RG.inspect_log, RG.PI.read_mac_flow,
         RG.usb_events_since, sys.argv) = orig


def test_chain_exception(tmp):
    print("[H] 录制链检查异常：正常模式 vs 恢复模式")
    def chain_raise(log_dir, name):
        raise OSError("boom")

    class States:
        def __init__(self, seq):
            self.seq = list(seq)
        def __call__(self):
            st = self.seq.pop(0) if self.seq else PI.RUNNING
            return (st, {"port": "/dev/ttyUSB1", "port_lock": {"reason": "test"}})

    def ctl_writer(path):
        p = Path(path); p.write_text("#!/bin/sh\nexit 0\n"); os.chmod(p, 0o755)
        return str(p)

    L = str(tmp / "logs_h"); Path(L).mkdir(parents=True, exist_ok=True)
    (Path(L) / "serial_ttyUSB1.log").write_text("x")
    mac_ok = lambda port: (True, "10:97:bd:d3:65:d8")   # noqa: E731
    run_ok = lambda port: (True, "ran")                  # noqa: E731

    ok, ev = PI.read_mac_flow(
        "/dev/ttyUSB1", L, str(tmp / "h1.json"),
        capture_ctl=ctl_writer(tmp / "h1.sh"), evidence_dir=str(tmp / "ev_h1"),
        lock_path=str(tmp / "lk_h1"), read_mac=mac_ok, run_app=run_ok,
        strict_restore=False, chain_check=chain_raise,
        state_fn=States([PI.COOPERATING_STOPPED, PI.RUNNING]),
        external_holder_attested=True, attestation_note="test",
        require_chain=False)
    ck("H1 恢复模式：链检查异常 → ok=True，errors 空、diagnostics 有记录",
       ok and ev["errors"] == [] and any("chain_check" in d for d in ev["diagnostics"]),
       f"{ok} errors={ev['errors']} diag={ev.get('diagnostics')}")

    ok, ev = PI.read_mac_flow(
        "/dev/ttyUSB1", L, str(tmp / "h2.json"),
        capture_ctl=ctl_writer(tmp / "h2.sh"), evidence_dir=str(tmp / "ev_h2"),
        lock_path=str(tmp / "lk_h2"), read_mac=mac_ok, run_app=run_ok,
        strict_restore=False, chain_check=chain_raise,
        state_fn=States([PI.RUNNING]),
        external_holder_attested=True, attestation_note="test",
        require_chain=True)
    ck("H2 正常模式：链检查异常 → ok=False 且写入 errors",
       (not ok) and any("chain_check" in e for e in ev["errors"]),
       str(ev.get("errors")))


def test_evidence_dir(tmp):
    print("[E] 证据目录全新性（read / rollback_gate 共用同一函数）")
    d = str(tmp / "e_new" / "nested")
    ok, why = PI.prepare_fresh_evidence_dir(d)
    ck("E1 全新目录 → 原子创建成功", ok and Path(d).is_dir(), str(why))
    ok2, why2 = PI.prepare_fresh_evidence_dir(d)
    ck("E2 同一目录再次准备 → 拒绝（防复用与并发竞争）",
       (not ok2) and bool(why2), str(why2))

    d2 = tmp / "e_stale"; Path(d2).mkdir()
    (Path(d2) / "mac_read_flow.final.json").write_text("{}")
    ok3, why3 = PI.prepare_fresh_evidence_dir(str(d2))
    ck("E3 含旧产物目录 → 拒绝并列出产物",
       (not ok3) and "mac_read_flow.final.json" in " ".join(why3), str(why3))

    d3 = tmp / "e_empty"; Path(d3).mkdir()
    ok4, why4 = PI.prepare_fresh_evidence_dir(str(d3))
    ck("E4 预先存在的空目录 → 仍拒绝（目录必须由工具创建）", not ok4, str(why4))

    ok5, why5 = PI.prepare_fresh_evidence_dir("")
    ck("E5 空路径 → 拒绝", (not ok5) and "为空" in " ".join(why5), str(why5))


def main():
    tmp = Path(tempfile.mkdtemp(prefix="s6_gate_test_"))
    try:
        test_port_lock_state(tmp)
        test_read_mac_flow(tmp)
        test_anchor_and_record(tmp)
        test_final_evidence(tmp)
        test_recovery_path(tmp)
        test_gate_recovery_wiring(tmp)
        test_chain_exception(tmp)
        test_evidence_dir(tmp)
    finally:
        for p in FAKE_PORTS:
            for suffix in ("", ".lock"):
                try:
                    os.unlink(PI.port_lock_path(p) + suffix)
                except OSError:
                    pass
    print(f"\ns6_gate: {PASS} passed, {FAIL} failed")
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
