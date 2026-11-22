#!/usr/bin/env python3
"""CMD-02 / CMD-15 阶段 3 设备回归 harness（v2，修订：异常分类/恢复核验/尝试留证/单读者/失败落盘）。

修订依据：2026-11-22 阶段 3 停止后的代码审查意见。
本版**未在真机运行**；`--selftest` 只用 mock 验证控制流，不连接设备、不打开串口。

用法：
    python3 cmd0215_stage3_v2.py --selftest     # 无设备自测
    python3 cmd0215_stage3_v2.py                # 设备路径（需单独授权，本版未获授权）
"""
import argparse
import asyncio
import json
import os
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, "/tmp")
import race_suite as R  # noqa: E402  Session/tryconnect/wait_terminal/wait_sta/ADDR/ABSENT

OUT = Path("/tmp/cmd0215_out")  # 运行时由 --out 覆盖
SERIAL_PORT = "/dev/ttyUSB0"


def set_out(path):
    global OUT
    OUT = Path(path)


# ---------------------------------------------------------------- 异常分类

TRANSPORT_EXC = {"ConnectionLost", "BleakError", "BleakDBusError", "TimeoutError"}


def exc_is_transport(exc):
    """只有明确的传输/链路异常才允许触发一次重连。
    注意：TimeoutError 在 bleak 中表示链路操作超时，归入传输类；
    协议层 ProtocolError（设备拒绝/错误码）不在集合内，绝不当成掉链。
    """
    return type(exc).__name__ in TRANSPORT_EXC


class AttrTap:
    """订阅设备属性/遥测；仅设备路径使用（paho 延迟导入）。"""

    def __init__(self, host, port):
        import paho.mqtt.client as mqtt
        self.msgs = []
        self.lock = threading.Lock()
        self.cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, protocol=mqtt.MQTTv311)
        self.cli.on_message = self._on
        self.cli.connect(host, port, keepalive=30)
        self.cli.subscribe([("v1/devices/me/attributes", 1),
                            ("v1/devices/me/telemetry", 1)])
        self.cli.loop_start()

    def _on(self, _c, _u, msg):
        try:
            obj = json.loads(msg.payload.decode("utf-8", "replace"))
        except Exception:  # noqa: BLE001
            return
        with self.lock:
            self.msgs.append(obj)

    def merged(self):
        out = {}
        with self.lock:
            for m in self.msgs:
                out.update(m)
        return out

    def stop(self):
        try:
            self.cli.loop_stop()
        except Exception:  # noqa: BLE001
            pass


def make_attr_tap(host, port):
    return AttrTap(host, port)


class RunState:
    def __init__(self):
        self.results = []
        self.attempts = []      # 每次调用/恢复尝试，独立留证
        self.reconnects = 0
        self.fatal = None

    def rec(self, case, ok, detail, **extra):
        item = {"case": case, "ok": bool(ok), "detail": detail, "ts": time.time()}
        item.update(extra)
        self.results.append(item)
        print(("PASS " if ok else "FAIL ") + case + " :: " + detail)


# ---------------------------------------------------------------- 调用封装

async def call(sess, st, cmd, args, rid, timeout=15, allow_reconnect=True):
    """发送一次；传输异常→重连一次后重发；协议异常/设备拒绝原样返回，不重连。

    每次尝试写入 st.attempts：{rid, cmd, ts, attempt, action, error, response_ok, code}
    """
    async def once(n):
        att = {"rid": rid, "cmd": cmd, "ts": time.time(), "attempt": n,
               "action": "", "error": None, "response_ok": None, "code": None}
        try:
            resp = await sess.send_request(
                {"id": rid, "version": "1.0", "cmd": cmd, "args": args,
                 "token": sess.token},
                timeout=timeout, retries=0)
            att["response_ok"] = resp.obj.get("ok")
            att["code"] = (resp.obj.get("error") or {}).get("code")
            att["action"] = "ok" if att["response_ok"] is True else "device_rejected"
            st.attempts.append(att)
            return resp.obj
        except Exception as exc:  # noqa: BLE001
            att["error"] = type(exc).__name__
            att["action"] = "reconnect_retry" if (exc_is_transport(exc) and allow_reconnect) else "raise"
            st.attempts.append(att)
            raise

    try:
        return await once(1)
    except Exception as exc:  # noqa: BLE001
        if not (exc_is_transport(exc) and allow_reconnect):
            raise
        st.reconnects += 1
        await asyncio.sleep(1.0)
        await sess._connect_once()
        return await once(2)


# ---------------------------------------------------------------- 恢复核验

async def wait_mqtt_connected(sess, st, timeout=70, expect_device=None, prefix="st"):
    """真实可达的 device.status 轮询；返回 (ok, samples, reason)。"""
    samples = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        d = (await call(sess, st, "device.status", {}, "%s%d" % (prefix, int(time.time() * 1000)))).get("data", {})
        dev = d.get("device_id")
        conn = bool((d.get("mqtt") or {}).get("connected"))
        samples.append({"ts": time.time(), "device_id": dev, "mqtt_connected": conn})
        if expect_device and dev and dev != expect_device:
            return False, samples, "device_mismatch"
        if conn:
            return True, samples, "connected"
        await asyncio.sleep(2)
    return False, samples, "timeout"


async def verify_restore(sess, st, host, port, expect_device=None):
    """恢复后双重核验：config.get 端口 + device.status 连接；返回 (ok, detail)。"""
    cfg = (await call(sess, st, "config.get", {}, "verify-cfg", 30)).get("data", {})
    m = (cfg.get("mqtt") or {})
    port_ok = m.get("mqtt_host") == host and m.get("mqtt_port") == port
    ok, samples, why = await wait_mqtt_connected(sess, st, 70, expect_device, "verify-st")
    detail = {"config": {"host": m.get("mqtt_host"), "port": m.get("mqtt_port")},
              "port_ok": port_ok, "mqtt_ok": ok, "reason": why,
              "samples": len(samples)}
    return bool(port_ok and ok), detail


# ---------------------------------------------------------------- 串口单读者

class SerialCapture:
    """单 reader 串口采集；启动前检查独占，结束确认退出。"""

    def __init__(self, port, outdir, proc_root="/proc", require_exists=True):
        self.port = port
        self.dir = Path(outdir)
        self.proc_root = proc_root
        self.require_exists = require_exists
        self.fd = None
        self.thread = None
        self.stop_ev = threading.Event()
        self.file = None
        self.error = None

    def _other_readers(self):
        pids = []
        root = Path(self.proc_root)
        if not root.is_dir():
            return pids
        for p in root.iterdir():
            fdd = p / "fd"
            if not fdd.is_dir():
                continue
            try:
                for fd in fdd.iterdir():
                    try:
                        if os.readlink(fd) == self.port:
                            pids.append(p.name)
                            break
                    except OSError:
                        continue
            except OSError:
                continue
        return pids

    def preflight(self):
        if self.require_exists and not os.path.exists(self.port):
            raise RuntimeError("串口不存在: %s" % self.port)
        others = self._other_readers()
        if others:
            raise RuntimeError("串口被其他进程占用（pid=%s），拒绝采集以保持日志干净" %
                               ",".join(others))

    def start(self):
        self.preflight()
        subprocess_stty(self.port)
        self.dir.mkdir(parents=True, exist_ok=True)
        self.file = open(self.dir / "serial.log", "ab")
        self.fd = os.open(self.port, os.O_RDONLY | os.O_NONBLOCK)
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()
        return self

    def _loop(self):
        import select
        while not self.stop_ev.is_set():
            r, _, _ = select.select([self.fd], [], [], 0.2)
            if not r:
                continue
            try:
                data = os.read(self.fd, 4096)
            except BlockingIOError:
                continue
            if data and self.file:
                self.file.write(data)
                self.file.flush()

    def stop(self):
        self.stop_ev.set()
        if self.thread:
            self.thread.join(timeout=3)
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None
        if self.file:
            self.file.close()
            self.file = None
        if self.thread and self.thread.is_alive():
            self.error = "reader 未在 3s 内退出"
            return False
        return True


def subprocess_stty(port):
    import subprocess
    subprocess.run(["stty", "-F", port, "115200", "raw", "-echo"], check=False)


# ---------------------------------------------------------------- 落盘

def finish(st, quiet=False):
    OUT.mkdir(parents=True, exist_ok=True)
    payload = {
        "results": st.results,
        "attempts": st.attempts,
        "reconnects": st.reconnects,
        "fatal": st.fatal,
        "pass": (st.fatal is None) and all(r["ok"] for r in st.results),
        "ts": time.time(),
    }
    (OUT / "stage3_results.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    if not quiet:
        print("SUMMARY:", "PASS" if payload["pass"] else "FAIL",
              "| fatal=%s" % (st.fatal or "-"))


# ---------------------------------------------------------------- 设备用例（本版未执行）

async def case_c21(sess, st):
    okA, oidA, _ = await R.tryconnect(sess, sess.token, "c21a")
    r2 = await call(sess, st, "network.tryconnect",
                    {"ssid": R.ABSENT, "password": "not-a-real-password-0"}, "c21b")
    busy = (r2.get("error") or {}).get("code")
    if oidA:
        await R.wait_terminal(sess, sess.token, oidA)
    ok = bool(okA and busy == "busy")
    st.rec("C2-1 活动 job 时第二次 tryconnect", ok, "A=%s 第二次=%s" % (okA, busy))
    return ok


async def case_c22(sess, st):
    okA, oidA, _ = await R.tryconnect(sess, sess.token, "c22a")
    if not oidA:
        st.rec("C2-2 cancel 幂等", False, "未取得 operation_id")
        return False
    await asyncio.sleep(0.8)
    c1 = await call(sess, st, "network.cancel", {"operation_id": oidA}, "c22c1")
    s1 = (c1.get("data") or {}).get("state") or (c1.get("error") or {}).get("code")
    c2 = await call(sess, st, "network.cancel", {"operation_id": oidA}, "c22c2")
    s2 = (c2.get("data") or {}).get("state") or (c2.get("error") or {}).get("code")
    ok = bool(okA and s1 == "cancelled" and s2 == "cancelled")
    st.rec("C2-2 cancel 幂等", ok, "取消→%s；重复取消→%s" % (s1, s2))
    return ok


async def case_c23(sess, st):
    okA, oidA, _ = await R.tryconnect(sess, sess.token, "c23a")
    trial_states, torn = [], []
    for i in range(5):
        d = (await call(sess, st, "network.status", {}, "c23s%d" % i)).get("data", {})
        trial = d.get("trial") or {}
        trial_states.append(trial.get("state"))
        if trial.get("state") == "got_ip" and not trial.get("trial_ssid"):
            torn.append("got_ip 缺 trial_ssid")
        await asyncio.sleep(0.3)
    if oidA:
        await R.wait_terminal(sess, sess.token, oidA)
    end_trial = ((await call(sess, st, "network.status", {}, "c23end")).get("data", {}) or {}).get("trial") or {}
    ok = bool(okA and not torn and end_trial.get("state") == "idle")
    st.rec("C2-3 在飞 network.status 快照", ok,
           "states=%s torn=%s 终态trial=%s" % (trial_states, torn, end_trial.get("state")))
    return ok


async def case_c24(sess, st):
    okA, oidA, _ = await R.tryconnect(sess, sess.token, "c24a")
    seq, term = [], {}
    deadline = time.monotonic() + 40
    while oidA and time.monotonic() < deadline:
        d = (await call(sess, st, "operation.get", {"operation_id": oidA}, "c24g")).get("data", {})
        seq.append(d.get("state"))
        if d.get("state") in ("failed", "success", "cancelled", "expired"):
            term = d
            break
        await asyncio.sleep(0.3)
    has_detail = isinstance(term.get("error_detail"), dict) and isinstance(term.get("stages"), dict)
    ok = bool(okA and term.get("state") == "failed" and term.get("error") == "ap_not_found" and has_detail)
    st.rec("C2-4 在飞 operation.get 快照", ok,
           "序列=%s 终态=%s/%s detail=%s" % (seq[:6], term.get("state"), term.get("error"), has_detail))
    return ok


async def case_c151(sess, st, attr_factory=None):
    pre = ((await call(sess, st, "config.get", {}, "c151pre", 30)).get("data", {}) or {}).get("mqtt", {})
    ph, pp = pre.get("mqtt_host"), pre.get("mqtt_port")
    set_ok = conn_ok = restored = vok = False
    vdetail = {"ok": False, "detail": "not_run"}
    tap = None
    try:
        try:
            r = await call(sess, st, "config.set",
                           {"patch": {"mqtt_host": ph, "mqtt_port": 1883}}, "c151set")
            set_ok = r.get("ok") is True
            conn_ok, _, why = await wait_mqtt_connected(sess, st, 70, None, "c151st")
            st.rec("C15-1 改端口后连接", bool(set_ok and conn_ok),
                   "set=%s connected=%s/%s" % (set_ok, conn_ok, why))
            if attr_factory is not None and conn_ok:
                tap = attr_factory(ph, 1883)
                await asyncio.sleep(6)
                merged = tap.merged()
                need = {"net_mode", "sta_ssid", "sta_has_ip", "sta_ip", "uart_baudrate"}
                field_ok = need.issubset(set(merged.keys()))
                consistent = (merged.get("net_mode") == "sta" and merged.get("sta_ssid") == "CMCC-502"
                              and merged.get("sta_has_ip") is True and bool(merged.get("sta_ip")))
                st.rec("C15-1 属性快照", bool(field_ok and consistent),
                       "fields=%s consistent=%s" % (field_ok, consistent),
                       sample={k: merged.get(k) for k in sorted(need)})
            elif attr_factory is None:
                st.rec("C15-1 属性快照", True, "mock/无属性采样（未执行）")
        except Exception as exc:  # noqa: BLE001
            st.rec("C15-1 改端口后连接", False, "异常 %s" % type(exc).__name__)
    finally:
        if tap is not None:
            tap.stop()
        for attempt in range(3):
            try:
                rr = await call(sess, st, "config.set",
                                {"patch": {"mqtt_host": ph, "mqtt_port": pp}},
                                "c151rst%d" % attempt, allow_reconnect=True)
                if rr.get("ok") is True:
                    restored = True
                    break
            except Exception as exc:  # noqa: BLE001
                st.attempts.append({"rid": "c151rst%d" % attempt, "cmd": "config.set",
                                    "ts": time.time(), "attempt": attempt + 1,
                                    "action": "restore_exception",
                                    "error": type(exc).__name__})
                await asyncio.sleep(2)
        try:
            vok, vdetail = await verify_restore(sess, st, ph, pp)
        except Exception as exc:  # noqa: BLE001
            vok, vdetail = False, {"error": type(exc).__name__}
        st.rec("C15-1 恢复并核验", bool(restored and vok),
               "restored=%s verify=%s" % (restored, json.dumps(vdetail, ensure_ascii=False)))
    return bool(set_ok and conn_ok and restored and vok)



async def case_c152(sess, st, attr_factory=None, mqtt_host="139.9.93.245", mqtt_port=21883):
    """Wi-Fi 抖动期间 MQTT 属性自洽；恢复后与 network.status 对读。"""
    pre = (await call(sess, st, "network.status", {}, "c152pre")).get("data", {})
    if not pre.get("sta_connected"):
        st.rec("C15-2 前置", False, "STA 未连接，拒绝制造抖动")
        return False
    tap = None
    try:
        okA, oidA, _ = await R.tryconnect(sess, sess.token, "c152a", "CMCC-502", "race-wrong-pass-0")
        if oidA:
            try:
                await R.wait_terminal(sess, sess.token, oidA)
            except Exception as exc:  # noqa: BLE001
                st.attempts.append({"rid": "c152wait", "cmd": "operation.get",
                                    "ts": time.time(), "action": "terminal_exception",
                                    "error": type(exc).__name__})
        if attr_factory is not None:
            tap = attr_factory(mqtt_host, mqtt_port)
        await asyncio.sleep(8)
        restored = await R.wait_sta(sess, sess.token, True, 60)
        post = (await call(sess, st, "network.status", {}, "c152post")).get("data", {})
        if attr_factory is None:
            st.rec("C15-2 Wi-Fi 抖动恢复", bool(okA and restored),
                   "mock/无属性采样；恢复=%s" % restored)
            return bool(okA and restored)
        merged = tap.merged()
        torn = ((merged.get("sta_has_ip") is False and bool(merged.get("sta_ip"))) or
                (merged.get("sta_has_ip") is True and not merged.get("sta_ip")))
        match = (post.get("sta_has_ip") == merged.get("sta_has_ip")
                 and post.get("sta_ip") == merged.get("sta_ip"))
        ok = bool(okA and restored and not torn and match)
        st.rec("C15-2 Wi-Fi 抖动期间属性自洽", ok,
               "terminal=%s 恢复=%s torn=%s 对读一致=%s" %
               (oidA and "seen", restored, torn, match),
               attr={k: merged.get(k) for k in ("net_mode", "sta_has_ip", "sta_ip")},
               post={k: post.get(k) for k in ("sta_has_ip", "sta_ip")})
        return ok
    finally:
        if tap is not None:
            tap.stop()

async def run_device():
    st = RunState()
    cap = None
    try:
        cap = SerialCapture(SERIAL_PORT, OUT).start()
        sess = R.Session(cli=None, address=R.ADDR, username="admin", password="123456")
        await sess._connect_once()
        d = ((await call(sess, st, "device.status", {}, "pre")).get("data", {}) or {})
        expect = d.get("device_id")
        print("[pre] fw=%s sha=%s device=%s" %
              (d.get("firmware"), d.get("build_sha"), expect))
        for fn in (case_c21, case_c22, case_c23, case_c24):
            if not await fn(sess, st):
                st.fatal = "停止条件：%s 失败" % fn.__name__
                break
        if st.fatal is None and not await case_c151(sess, st, make_attr_tap):
            st.fatal = "停止条件：case_c151 失败"
        if st.fatal is None and not await case_c152(sess, st, make_attr_tap):
            st.fatal = "停止条件：case_c152 失败"
        await sess.cli.disconnect()
    except Exception as exc:  # noqa: BLE001
        st.fatal = "异常退出：%s: %s" % (type(exc).__name__, exc)
    finally:
        if cap:
            if not cap.stop():
                st.results.append({"case": "串口采集收尾", "ok": False,
                                   "detail": cap.error or "reader 未退出", "ts": time.time()})
            elif cap.error:
                st.results.append({"case": "串口采集", "ok": False, "detail": cap.error,
                                   "ts": time.time()})
        finish(st)
    return 0 if all(r["ok"] for r in st.results) and not st.fatal else 1


# ---------------------------------------------------------------- mock 自测

class FakeResp:
    def __init__(self, obj):
        self.obj = obj


class FakeSession:
    """按脚本返回响应/抛异常；统计连接次数。"""

    def __init__(self, script):
        self.script = list(script)
        self.token = "tok"
        self.connects = 0
        self.sent = []

    async def _connect_once(self):
        self.connects += 1

    async def send_request(self, req, timeout=0, retries=0):
        self.sent.append(req)
        action = self.script.pop(0) if self.script else {"obj": {"ok": True, "data": {}}}
        if action.get("raise"):
            raise action["raise"]
        return FakeResp(action["obj"])


class FakeTransient(Exception):
    pass


FakeTransient.__name__ = "ConnectionLost"


class FakeProtocolError(Exception):
    pass


FakeProtocolError.__name__ = "ProtocolError"


async def selftest():
    ok_all = True
    tmp = Path("/tmp/cmd0215_selftest")
    tmp.mkdir(parents=True, exist_ok=True)
    set_out(tmp)

    # T1: 传输异常 → 重连一次并重发成功
    st = RunState()
    s = FakeSession([{"raise": FakeTransient()}, {"obj": {"ok": True, "data": {"x": 1}}}])
    obj = await call(s, st, "device.status", {}, "t1")
    t1 = (s.connects == 1 and obj.get("ok") is True and st.reconnects == 1
          and len(st.attempts) == 2 and st.attempts[1]["attempt"] == 2)
    print("T1 transport-reconnect:", "PASS" if t1 else "FAIL")
    ok_all &= t1

    # T2: 设备拒绝（ok=false）→ 不重连、错误码原样
    st = RunState()
    s = FakeSession([{"obj": {"ok": False, "error": {"code": "busy"}}}])
    obj = await call(s, st, "network.tryconnect", {}, "t2")
    t2 = (s.connects == 0 and (obj.get("error") or {}).get("code") == "busy"
          and st.attempts[-1]["action"] == "device_rejected")
    print("T2 device-reject-no-reconnect:", "PASS" if t2 else "FAIL")
    ok_all &= t2

    # T3: 协议异常 → 不重连、异常透传
    st = RunState()
    s = FakeSession([{"raise": FakeProtocolError()}])
    try:
        await call(s, st, "config.set", {}, "t3")
        t3 = False
    except FakeProtocolError:
        t3 = (s.connects == 0 and st.attempts[-1]["action"] == "raise")
    print("T3 protocol-error-passthrough:", "PASS" if t3 else "FAIL")
    ok_all &= t3

    # T4: wait_mqtt_connected 可达 + device_id 校验
    st = RunState()
    s = FakeSession([
        {"obj": {"ok": True, "data": {"device_id": "A", "mqtt": {"connected": False}}}},
        {"obj": {"ok": True, "data": {"device_id": "A", "mqtt": {"connected": True}}}},
    ])
    got, samples, why = await wait_mqtt_connected(s, st, 10, "A", "t4")
    t4 = got and why == "connected" and len(samples) == 2
    print("T4 wait_mqtt_connected-reachable:", "PASS" if t4 else "FAIL")
    ok_all &= t4

    # T5: device_id 不匹配 → 立即停止
    st = RunState()
    s = FakeSession([{"obj": {"ok": True, "data": {"device_id": "B", "mqtt": {"connected": True}}}}])
    got, _, why = await wait_mqtt_connected(s, st, 10, "A", "t5")
    t5 = (got is False and why == "device_mismatch")
    print("T5 device-id-mismatch:", "PASS" if t5 else "FAIL")
    ok_all &= t5

    # T6: 恢复重试有上限 + 尝试留证 + 失败落盘
    st = RunState()
    st.results.append({"case": "pre", "ok": True, "detail": "", "ts": time.time()})
    script = [
        {"obj": {"ok": True, "data": {"mqtt": {"mqtt_host": "h", "mqtt_port": 21883}}}},
        {"obj": {"ok": True, "data": {}}},
        {"obj": {"ok": True, "data": {"device_id": "A", "mqtt": {"connected": True}}}},
    ] + [{"raise": FakeTransient()} for _ in range(10)]
    s = FakeSession(script)
    try:
        await case_c151(s, st)
    except Exception:
        pass
    restore_exc = [a for a in st.attempts if a.get("action") == "restore_exception"]
    t6a = len(restore_exc) == 3
    finish(st, quiet=True)
    on_disk = json.load(open(tmp / "stage3_results.json", encoding="utf-8"))
    t6 = ("attempts" in on_disk and on_disk["pass"] is False
          and any(r["case"] == "C15-1 恢复并核验" and r["ok"] is False
                  for r in on_disk["results"]))
    print("T6 bounded-retry+persist:", "PASS" if (t6a and t6) else "FAIL",
          "| restore_exc=%d" % len(restore_exc))
    ok_all &= bool(t6a and t6)

    # T7: 串口独占检查（伪造 proc 根，不触碰真实设备）
    fake = tmp / "fake_proc" / "4242" / "fd"
    fake.mkdir(parents=True, exist_ok=True)
    link = fake / "3"
    if link.exists() or link.is_symlink():
        link.unlink()
    os.symlink("/dev/fake_port", link)
    cap = SerialCapture("/dev/fake_port", tmp / "cap", proc_root=str(tmp / "fake_proc"),
                        require_exists=False)
    busy = cap._other_readers()
    t7 = busy == ["4242"]
    print("T7 serial-exclusive-check:", "PASS" if t7 else "FAIL", busy)
    ok_all &= t7

    print("SELFTEST:", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--out", default="/home/tc/esp/workspace/iothub/artifacts/cmd02_15_stage3_20261122")
    a = ap.parse_args()
    set_out(a.out)
    if a.selftest:
        raise SystemExit(asyncio.run(selftest()))
    raise SystemExit(asyncio.run(run_device()))
