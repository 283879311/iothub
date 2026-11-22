#!/usr/bin/env python3
"""CMD-02 / CMD-15 阶段 3 设备回归 harness（v3）。

v3 针对 2026-11-22 审查的 5 个阻塞项与 3 个用例语义问题：
  B1 写命令默认不重发：仅只读白名单命令允许一次重连重发；写命令传输失败记
     outcome_unknown 并交给核验，不盲发。
  B2 异常按真实类型分类：bleak 的 BleakError 子类 / p0_acceptance.ConnectionLost
     才算 transport；asyncio.TimeoutError 单列 timeout，不触发重发。
  B3 设备身份贯穿：run_device 取 device_id，缺失即失败；恢复核验绑定预期设备。
  B4 verify_restore 结构化：config/mqtt 两阶段各自留证，异常不再吞掉 detail。
  B5 串口：preflight + flock 双重检查；reader 线程异常保存并判 FAIL；启动失败清理 FD。
  C2-2 等待第一次 cancel 终态；C2-3 trial 与 job 一致性；C15-2 属性订阅先于抖动；
  C2-4 状态单调性与字段内容校验。

本版未在真机运行；--selftest 只用 mock，不连接设备、不打开串口。
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
import race_suite as R  # noqa: E402

OUT = Path("/tmp/cmd0215_out")
SERIAL_PORT = "/dev/ttyUSB0"
ABSENT = R.ABSENT

# ---------------------------------------------------------------- 异常分类

try:  # 真实 BLE 异常层级
    from bleak.exc import BleakError, BleakDBusError  # type: ignore
except Exception:  # noqa: BLE001
    BleakError = BleakDBusError = None  # type: ignore

try:
    from p0_acceptance import ConnectionLost  # type: ignore
except Exception:  # noqa: BLE001
    ConnectionLost = None  # type: ignore

READ_ONLY_CMDS = {
    "device.info", "device.status", "config.get", "network.status",
    "operation.get", "network.profile.list",
}


class OutcomeUnknown(Exception):
    """写命令传输失败：设备可能已执行，结果不确定，禁止重发。"""


def classify_exc(exc):
    """返回 transport / timeout / other。真实类型不可用时不按类名兜底。"""
    if isinstance(exc, asyncio.TimeoutError):
        return "timeout"
    if ConnectionLost is not None and isinstance(exc, ConnectionLost):
        return "transport"
    if BleakError is not None and isinstance(exc, BleakError):
        return "transport"
    if BleakDBusError is not None and isinstance(exc, BleakDBusError):
        return "transport"
    return "other"


# ---------------------------------------------------------------- 状态/证据

class AttrTap:
    """订阅属性/遥测；必须等到 CONNACK 与 SUBACK 才返回。"""

    def __init__(self, host, port, timeout=10.0):
        import paho.mqtt.client as mqtt
        self.msgs = []
        self.lock = threading.Lock()
        self.connected = threading.Event()
        self.subacked = threading.Event()
        self.connect_ts = None
        self.suback_ts = None
        self.rc = None
        self.cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, protocol=mqtt.MQTTv311)
        self.cli.on_message = self._on
        self.cli.on_connect = self._on_connect
        self.cli.on_subscribe = self._on_subscribe
        self.cli.connect(host, port, keepalive=30)
        self.cli.loop_start()
        if not self.connected.wait(timeout):
            self.stop()
            raise RuntimeError("mqtt CONNACK 未确认")
        self.cli.subscribe([("v1/devices/me/attributes", 1),
                            ("v1/devices/me/telemetry", 1)])
        if not self.subacked.wait(timeout):
            self.stop()
            raise RuntimeError("mqtt SUBACK 未确认")

    def _on_connect(self, _c, _u, _flags, rc, _props=None):
        self.rc = int(rc) if rc is not None else -1
        self.connect_ts = time.time()
        if self.rc == 0:
            self.connected.set()

    def _on_subscribe(self, _c, _u, _mid, _granted, _props=None):
        self.suback_ts = time.time()
        self.subacked.set()

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
        self.attempts = []
        self.reconnects = 0
        self.fatal = None

    def rec(self, case, ok, detail, **extra):
        item = {"case": case, "ok": bool(ok), "detail": detail,
                "ts": time.time()}
        item.update(extra)
        self.results.append(item)
        print(("PASS " if ok else "FAIL ") + case + " :: " + detail)


# ---------------------------------------------------------------- 调用封装

async def call(sess, st, cmd, args, rid, timeout=15, allow_reconnect=None):
    """发送一次。

    allow_reconnect 默认 = 命令是否在 READ_ONLY_CMDS 内。
    只读命令：transport 异常 → 重连一次后重发。
    写命令：transport 异常 → 记 outcome_unknown 并抛 OutcomeUnknown，不重发。
    其它异常（timeout/protocol/other）→ 原样抛，不重连。
    """
    if allow_reconnect is None:
        allow_reconnect = cmd in READ_ONLY_CMDS

    async def once(n):
        att = {"rid": rid, "cmd": cmd, "ts": time.time(), "attempt": n,
               "allow_reconnect": bool(allow_reconnect), "action": "",
               "error": None, "exc_class": None, "response_ok": None, "code": None}
        try:
            resp = await sess.send_request(
                {"id": rid, "version": "1.0", "cmd": cmd, "args": args,
                 "token": sess.token}, timeout=timeout, retries=0)
            att["response_ok"] = resp.obj.get("ok")
            att["code"] = (resp.obj.get("error") or {}).get("code")
            att["action"] = "ok" if att["response_ok"] is True else "device_rejected"
            st.attempts.append(att)
            return resp.obj
        except Exception as exc:  # noqa: BLE001
            kind = classify_exc(exc)
            att["error"] = str(exc)
            att["exc_class"] = type(exc).__name__
            att["kind"] = kind
            att["action"] = "raise"
            st.attempts.append(att)
            raise

    try:
        return await once(1)
    except Exception as exc:  # noqa: BLE001
        kind = classify_exc(exc)
        if kind == "transport" and allow_reconnect:
            st.reconnects += 1
            await asyncio.sleep(1.0)
            await sess._connect_once()
            return await once(2)
        if kind == "transport" and not allow_reconnect:
            raise OutcomeUnknown("%s transport failure after send; not resent" % cmd) from exc
        raise


async def w_tryconnect(sess, st, rid, ssid=ABSENT, password="not-a-real-password-0"):
    """写命令：经 call() 发送，不内层重试；transport 失败抛 OutcomeUnknown。"""
    r = await call(sess, st, "network.tryconnect",
                   {"ssid": ssid, "password": password}, rid, 20)
    d = r.get("data") or {}
    return r.get("ok") is True, d.get("operation_id"), (r.get("error") or {}).get("code")


async def wait_terminal(sess, st, oid, timeout=40, prefix="term"):
    """只读轮询终态；返回最后 data（超时返回最近一次）。"""
    deadline = time.monotonic() + timeout
    last = {}
    while time.monotonic() < deadline:
        d = (await call(sess, st, "operation.get", {"operation_id": oid},
                        "%s%d" % (prefix, int(time.time() * 1000)))).get("data", {})
        last = d or {}
        if last.get("state") in ("failed", "success", "cancelled", "expired"):
            return last
        await asyncio.sleep(0.3)
    return last


async def wait_sta(sess, st, want, timeout=60, prefix="sta"):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        n = ((await call(sess, st, "device.status", {},
                         "%s%d" % (prefix, int(time.time() * 1000)))).get("data", {}) or {}).get("network", {})
        if bool(n.get("sta_connected")) == want:
            return True
        await asyncio.sleep(1.5)
    return False


# ---------------------------------------------------------------- 恢复核验

async def wait_mqtt_connected(sess, st, timeout=70, expect_device=None, prefix="st"):
    """device.status 轮询；身份缺失/不匹配都判失败。返回 (ok, samples, reason)。"""
    samples = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        d = (await call(sess, st, "device.status", {},
                        "%s%d" % (prefix, int(time.time() * 1000)))).get("data", {})
        dev = d.get("device_id")
        conn = bool((d.get("mqtt") or {}).get("connected"))
        samples.append({"ts": time.time(), "device_id": dev,
                        "mqtt_connected": conn})
        if expect_device:
            if not dev:
                return False, samples, "device_id_missing"
            if dev != expect_device:
                return False, samples, "device_mismatch"
        if conn:
            return True, samples, "connected"
        await asyncio.sleep(2)
    return False, samples, "timeout"


async def verify_restore(sess, st, host, port, expect_device):
    """两阶段结构化核验；任何异常都转为 stage 结果，不丢 detail。"""
    out = {"stages": [], "ok": False}

    def stage(name, ok, **kw):
        item = {"stage": name, "ok": bool(ok), "ts": time.time()}
        item.update(kw)
        out["stages"].append(item)

    try:
        cfg = (await call(sess, st, "config.get", {}, "verify-cfg", 30)).get("data", {})
        m = cfg.get("mqtt") or {}
        port_ok = m.get("mqtt_host") == host and m.get("mqtt_port") == port
        stage("config", port_ok, host=m.get("mqtt_host"), port=m.get("mqtt_port"))
    except Exception as exc:  # noqa: BLE001
        stage("config", False, error=type(exc).__name__, kind=classify_exc(exc))

    try:
        ok, samples, why = await wait_mqtt_connected(sess, st, 70, expect_device, "verify-st")
        dev = samples[-1]["device_id"] if samples else None
        stage("mqtt", ok, reason=why, samples=len(samples), device_id=dev)
    except Exception as exc:  # noqa: BLE001
        stage("mqtt", False, error=type(exc).__name__, kind=classify_exc(exc))

    out["ok"] = len(out["stages"]) == 2 and all(s["ok"] for s in out["stages"])
    return out


# ---------------------------------------------------------------- 串口单读者

class SerialCapture:
    """preflight(/proc) + flock 双重检查；reader 异常保存并判失败。"""

    def __init__(self, port, outdir, proc_root="/proc", require_exists=True):
        self.port = port
        self.dir = Path(outdir)
        self.proc_root = proc_root
        self.require_exists = require_exists
        self.fd = None
        self.thread = None
        self.stop_ev = threading.Event()
        self.file = None
        self.read_error = None
        self.locked = False

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
            raise RuntimeError("串口被其他进程占用（pid=%s）" % ",".join(others))

    def start(self):
        self.preflight()
        try:
            import subprocess
            subprocess.run(["stty", "-F", self.port, "115200", "raw", "-echo"],
                           check=False)
            self.dir.mkdir(parents=True, exist_ok=True)
            self.fd = os.open(self.port, os.O_RDONLY | os.O_NONBLOCK)
            try:
                import fcntl
                fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                self.locked = True
            except OSError as exc:
                # flock 失败：另一持有者存在（或平台不支持），显式失败
                raise RuntimeError("串口 flock 失败（可能已有 reader）: %s" % exc)
            self.file = open(self.dir / "serial.log", "ab")
            self.thread = threading.Thread(target=self._loop, daemon=True)
            self.thread.start()
            return True
        except Exception:
            self._cleanup_fds()
            raise

    def _loop(self):
        import select
        while not self.stop_ev.is_set():
            try:
                r, _, _ = select.select([self.fd], [], [], 0.2)
                if not r:
                    continue
                data = os.read(self.fd, 4096)
                if data and self.file:
                    self.file.write(data)
                    self.file.flush()
            except Exception as exc:  # noqa: BLE001
                self.read_error = "%s: %s" % (type(exc).__name__, exc)
                self.stop_ev.set()
                return

    def _cleanup_fds(self):
        if self.fd is not None:
            try:
                if self.locked:
                    import fcntl
                    fcntl.flock(self.fd, fcntl.LOCK_UN)
            except Exception:  # noqa: BLE001
                pass
            os.close(self.fd)
            self.fd = None
        if self.file is not None:
            self.file.close()
            self.file = None

    def stop(self):
        self.stop_ev.set()
        if self.thread:
            self.thread.join(timeout=3)
        alive = bool(self.thread and self.thread.is_alive())
        self._cleanup_fds()
        if alive:
            return False, "reader 未在 3s 内退出"
        if self.read_error:
            return False, "reader 异常退出: " + self.read_error
        return True, ""


# ---------------------------------------------------------------- 落盘

def finish(st):
    OUT.mkdir(parents=True, exist_ok=True)
    payload = {
        "results": st.results, "attempts": st.attempts,
        "reconnects": st.reconnects, "fatal": st.fatal,
        "pass": st.fatal is None and bool(st.results) and all(r["ok"] for r in st.results),
        "ts": time.time(),
    }
    (OUT / "stage3_results.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    print("SUMMARY:", "PASS" if payload["pass"] else "FAIL",
          "| fatal=%s" % (st.fatal or "-"))


# ---------------------------------------------------------------- 用例

async def case_c21(sess, st):
    okA, oidA, _ = await w_tryconnect(sess, st, "c21a")
    r2 = await call(sess, st, "network.tryconnect",
                    {"ssid": ABSENT, "password": "not-a-real-password-0"}, "c21b")
    busy = (r2.get("error") or {}).get("code")
    if oidA:
        await wait_terminal(sess, st, oidA)
    ok = bool(okA and busy == "busy")
    st.rec("C2-1 活动 job 时第二次 tryconnect", ok, "A=%s 第二次=%s" % (okA, busy))
    return ok


async def case_c22(sess, st):
    okA, oidA, _ = await w_tryconnect(sess, st, "c22a")
    if not oidA:
        st.rec("C2-2 cancel 幂等", False, "未取得 operation_id")
        return False
    c1 = await call(sess, st, "network.cancel", {"operation_id": oidA}, "c22c1")
    s1 = (c1.get("data") or {}).get("state") or (c1.get("error") or {}).get("code")
    # 等待终态确认，再做第二次 cancel
    deadline = time.monotonic() + 15
    term = None
    while time.monotonic() < deadline:
        d = (await call(sess, st, "operation.get", {"operation_id": oidA}, "c22g")).get("data", {})
        term = d.get("state")
        if term in ("cancelled", "failed", "success", "expired"):
            break
        await asyncio.sleep(0.5)
    c2 = await call(sess, st, "network.cancel", {"operation_id": oidA}, "c22c2")
    s2 = (c2.get("data") or {}).get("state") or (c2.get("error") or {}).get("code")
    ok = bool(okA and s1 == "cancelled" and term == "cancelled" and s2 == "cancelled")
    st.rec("C2-2 cancel 幂等", ok, "取消→%s 终态=%s 重复→%s" % (s1, term, s2))
    return ok


async def case_c23(sess, st):
    ssid = ABSENT
    okA, oidA, _ = await w_tryconnect(sess, st, "c23a", ssid)
    if not oidA:
        st.rec("C2-3 在飞 network.status 快照", False, "未取得 operation_id")
        return False
    states, torn, trial_seen = [], [], False
    for i in range(5):
        d = (await call(sess, st, "network.status", {}, "c23s%d" % i)).get("data", {})
        trial = d.get("trial") or {}
        stt = trial.get("state")
        states.append(stt)
        if stt == "got_ip":
            trial_seen = True
            if not trial.get("trial_id") or not trial.get("trial_ssid"):
                torn.append("got_ip 缺 trial_id/trial_ssid")
            elif trial.get("trial_ssid") != ssid:
                torn.append("trial_ssid=%r 与本次 job ssid=%r 不符" %
                            (trial.get("trial_ssid"), ssid))
            op = (await call(sess, st, "operation.get", {"operation_id": oidA},
                             "c23g%d" % i)).get("data", {})
            if op.get("state") not in ("got_ip", "committing", "success"):
                torn.append("trial=got_ip 但 job state=%s" % op.get("state"))
        elif trial.get("trial_id") or trial.get("trial_ssid"):
            torn.append("非 got_ip 却带 trial 字段")
        await asyncio.sleep(0.3)
    term = await wait_terminal(sess, st, oidA)
    tail = ((await call(sess, st, "network.status", {}, "c23end")).get("data", {}) or {}).get("trial") or {}
    ok = bool(okA and not torn and tail.get("state") == "idle"
              and term.get("state") == "failed")
    st.rec("C2-3 在飞 network.status 快照", ok,
           "states=%s torn=%s 终态trial=%s job=%s/%s trial_seen=%s" %
           (states, torn, tail.get("state"), term.get("state"), term.get("error"), trial_seen),
           coverage="trial_seen" if trial_seen else "no_trial_in_absent_path")
    return ok


def _state_rank(s):
    return {"accepted": 0, "connecting": 1, "got_ip": 2, "committing": 3,
            "success": 4, "failed": 4, "cancelled": 4, "expired": 4}.get(s, -1)


async def case_c24(sess, st):
    okA, oidA, _ = await w_tryconnect(sess, st, "c24a")
    seq, term = [], {}
    deadline = time.monotonic() + 40
    while oidA and time.monotonic() < deadline:
        d = (await call(sess, st, "operation.get", {"operation_id": oidA}, "c24g")).get("data", {})
        seq.append(d.get("state"))
        if d.get("state") in ("failed", "success", "cancelled", "expired"):
            term = d
            break
        await asyncio.sleep(0.3)
    ranks = [_state_rank(s) for s in seq if s]
    monotonic = all(ranks[i] <= ranks[i + 1] for i in range(len(ranks) - 1))
    detail = term.get("error_detail") or {}
    stages = term.get("stages") or {}
    content_ok = (bool(detail.get("code")) and bool(detail.get("message"))
                  and set(stages.keys()) == {"wifi_connected", "ip_acquired",
                                             "internet_verified", "mqtt_connected"})
    ok = bool(okA and term.get("state") == "failed" and term.get("error") == "ap_not_found"
              and monotonic and content_ok)
    st.rec("C2-4 在飞 operation.get 快照", ok,
           "序列=%s 单调=%s 终态=%s/%s content=%s" %
           (seq[:6], monotonic, term.get("state"), term.get("error"), content_ok))
    return ok


async def case_c151(sess, st, expect_device, attr_factory=None):
    pre = ((await call(sess, st, "config.get", {}, "c151pre", 30)).get("data", {}) or {}).get("mqtt", {})
    ph, pp = pre.get("mqtt_host"), pre.get("mqtt_port")
    set_ok = conn_ok = restored = False
    verify = {"ok": False, "stages": []}
    tap = None
    try:
        try:
            r = await call(sess, st, "config.set",
                           {"patch": {"mqtt_host": ph, "mqtt_port": 1883}}, "c151set")
            set_ok = r.get("ok") is True
        except OutcomeUnknown as exc:
            st.rec("C15-1 切端口写入结果不确定", False, str(exc))
            set_ok = False
        if set_ok:
            conn_ok, _, why = await wait_mqtt_connected(sess, st, 70, expect_device, "c151st")
            st.rec("C15-1 改端口后连接", bool(conn_ok),
                   "connected=%s/%s" % (conn_ok, why))
            if attr_factory and conn_ok:
                tap = attr_factory(ph, 1883)
                await asyncio.sleep(6)
                merged = tap.merged()
                need = {"net_mode", "sta_ssid", "sta_has_ip", "sta_ip", "uart_baudrate"}
                field_ok = need.issubset(set(merged.keys()))
                consistent = (merged.get("net_mode") == "sta"
                              and merged.get("sta_ssid") == "CMCC-502"
                              and merged.get("sta_has_ip") is True
                              and bool(merged.get("sta_ip")))
                st.rec("C15-1 属性快照", bool(field_ok and consistent),
                       "fields=%s consistent=%s" % (field_ok, consistent),
                       sample={k: merged.get(k) for k in sorted(need)})
    finally:
        if tap is not None:
            tap.stop()
        for attempt in range(3):
            try:
                rr = await call(sess, st, "config.set",
                                {"patch": {"mqtt_host": ph, "mqtt_port": pp}},
                                "c151rst%d" % attempt)
                if rr.get("ok") is True:
                    restored = True
                    break
            except OutcomeUnknown as exc:
                st.attempts.append({"rid": "c151rst%d" % attempt, "cmd": "config.set",
                                    "ts": time.time(), "attempt": attempt + 1,
                                    "action": "restore_outcome_unknown",
                                    "error": str(exc)})
            except Exception as exc:  # noqa: BLE001
                st.attempts.append({"rid": "c151rst%d" % attempt, "cmd": "config.set",
                                    "ts": time.time(), "attempt": attempt + 1,
                                    "action": "restore_exception",
                                    "error": type(exc).__name__})
            await asyncio.sleep(2)
        verify = await verify_restore(sess, st, ph, pp, expect_device)
        st.rec("C15-1 恢复并核验", bool(restored and verify["ok"]),
               "restored=%s verify=%s" % (restored, json.dumps(verify, ensure_ascii=False)))
    return bool(set_ok and conn_ok and restored and verify["ok"])


async def case_c152(sess, st, expect_device, attr_factory=None):
    pre = (await call(sess, st, "network.status", {}, "c152pre")).get("data", {})
    if not pre.get("sta_connected"):
        st.rec("C15-2 前置", False, "STA 未连接")
        return False
    tap = None
    if attr_factory:
        try:
            tap = attr_factory("139.9.93.245", 21883)
        except Exception as exc:  # noqa: BLE001
            st.rec("C15-2 属性订阅建立", False, "%s: %s" % (type(exc).__name__, exc))
            return False
        st.rec("C15-2 属性订阅建立", True,
               "connack=%s suback=%s" % (tap.connect_ts, tap.suback_ts))
    try:
        before = len(tap.msgs) if tap else 0
        okA, oidA, _ = await w_tryconnect(sess, st, "c152a",
                                          "CMCC-502", "race-wrong-pass-0")
        if not oidA:
            st.rec("C15-2 抖动终态", False, "未取得 operation_id")
            return False
        term = await wait_terminal(sess, st, oidA, 40, "c152t")
        term_ok = (term.get("state") == "failed"
                   and term.get("error") in ("wrong_password", "ap_not_found"))
        st.rec("C15-2 抖动终态", term_ok,
               "state=%s error=%s" % (term.get("state"), term.get("error")))
        if not term_ok:
            return False
        restored = await wait_sta(sess, st, True, 60)
        ident = ((await call(sess, st, "device.status", {}, "c152id")).get("data", {}) or {})
        if expect_device and ident.get("device_id") != expect_device:
            st.rec("C15-2 身份核验", False, "device_id=%s（期望 %s）" %
                   (ident.get("device_id"), expect_device))
            return False
        st.rec("C15-2 身份核验", True, "device_id=%s" % ident.get("device_id"))
        post = (await call(sess, st, "network.status", {}, "c152post")).get("data", {})
        if tap is None:
            st.rec("C15-2 Wi-Fi 抖动恢复", bool(okA and restored),
                   "mock/无属性采样；恢复=%s" % restored)
            return bool(okA and restored)
        merged = tap.merged()
        during = len(tap.msgs) - before
        torn = ((merged.get("sta_has_ip") is False and bool(merged.get("sta_ip")))
                or (merged.get("sta_has_ip") is True and not merged.get("sta_ip")))
        match = (post.get("sta_has_ip") == merged.get("sta_has_ip")
                 and post.get("sta_ip") == merged.get("sta_ip"))
        ok = bool(okA and restored and during > 0 and not torn and match)
        st.rec("C15-2 Wi-Fi 抖动期间属性自洽", ok,
               "抖动窗口属性=%d 恢复=%s torn=%s 对读=%s" %
               (during, restored, torn, match),
               attr={k: merged.get(k) for k in ("net_mode", "sta_has_ip", "sta_ip")},
               post={k: post.get(k) for k in ("sta_has_ip", "sta_ip")})
        return ok
    finally:
        if tap is not None:
            tap.stop()


# ---------------------------------------------------------------- 设备路径

async def run_device():
    st = RunState()
    cap = None
    evidence_ok = True
    try:
        cap = SerialCapture(SERIAL_PORT, OUT)
        cap.start()
        sess = R.Session(cli=None, address=R.ADDR, username="admin", password="123456")
        await sess._connect_once()
        d = ((await call(sess, st, "device.status", {}, "pre")).get("data", {}) or {})
        expect = d.get("device_id")
        print("[pre] fw=%s sha=%s device=%s" %
              (d.get("firmware"), d.get("build_sha"), expect))
        if not expect:
            raise RuntimeError("device_id 缺失，拒绝继续（无法绑定恢复核验）")
        for fn in (case_c21, case_c22, case_c23, case_c24):
            if not await fn(sess, st):
                st.fatal = "停止条件：%s 失败" % fn.__name__
                break
        if st.fatal is None and not await case_c151(sess, st, expect, make_attr_tap):
            st.fatal = "停止条件：case_c151 失败"
        if st.fatal is None and not await case_c152(sess, st, expect, make_attr_tap):
            st.fatal = "停止条件：case_c152 失败"
        await sess.cli.disconnect()
    except Exception as exc:  # noqa: BLE001
        st.fatal = "异常退出：%s: %s" % (type(exc).__name__, exc)
    finally:
        if cap is not None:
            ok, why = cap.stop()
            evidence_ok = ok
            if not ok:
                st.results.append({"case": "串口采集完整性", "ok": False,
                                   "detail": why, "ts": time.time()})
            elif not cap.locked:
                evidence_ok = False
                st.results.append({"case": "串口采集完整性", "ok": False,
                                   "detail": "未取得 flock 独占，证据不完整",
                                   "ts": time.time()})
        finish(st)
    return 0 if all(r["ok"] for r in st.results) and not st.fatal and evidence_ok else 1


# ---------------------------------------------------------------- mock 自测

class FakeResp:
    def __init__(self, obj):
        self.obj = obj


class FakeSession:
    def __init__(self, script):
        self.script = list(script)
        self.token = "tok"
        self.connects = 0
        self.sent = []

    async def _connect_once(self):
        self.connects += 1

    async def send_request(self, req, timeout=0, retries=0):
        self.sent.append(req)
        step = self.script.pop(0) if self.script else {"obj": {"ok": True, "data": {}}}
        if "raise" in step:
            raise step["raise"]
        return FakeResp(step["obj"])


def _mk_exc(name):
    cls = type(name, (Exception,), {})
    cls.__name__ = name
    return cls()


def _transport_exc():
    if ConnectionLost is not None:
        return ConnectionLost("simulated transport")
    return _transport_exc()


async def selftest():
    global OUT
    tmp = Path("/tmp/cmd0215_selftest")
    tmp.mkdir(parents=True, exist_ok=True)
    OUT = tmp
    ok_all = True

    def check(name, cond, extra=""):
        nonlocal ok_all
        print(("T " + name + ": " + ("PASS" if cond else "FAIL")) + ((" | " + extra) if extra else ""))
        ok_all &= bool(cond)

    # T1 只读命令 transport → 重连一次并重发
    st = RunState()
    s = FakeSession([{"raise": _transport_exc()},
                     {"obj": {"ok": True, "data": {"x": 1}}}])
    obj = await call(s, st, "device.status", {}, "t1")
    check("readonly-reconnect", s.connects == 1 and obj.get("ok") is True
          and st.reconnects == 1 and len(st.attempts) == 2)

    # T2 写命令 transport → 不重发、OutcomeUnknown
    st = RunState()
    s = FakeSession([{"raise": _transport_exc()}])
    try:
        await call(s, st, "config.set", {}, "t2")
        t2 = False
    except OutcomeUnknown:
        t2 = (s.connects == 0 and len(st.attempts) == 1
              and st.attempts[0]["action"] == "raise")
    check("write-outcome-unknown", t2)

    # T3 TimeoutError → 不属 transport，不重连
    st = RunState()
    s = FakeSession([{"raise": asyncio.TimeoutError()}])
    try:
        await call(s, st, "device.status", {}, "t3")
        t3 = False
    except asyncio.TimeoutError:
        t3 = (s.connects == 0 and st.attempts[0]["kind"] == "timeout")
    check("timeout-not-transport", t3)

    # T4 协议异常透传
    st = RunState()
    s = FakeSession([{"raise": _mk_exc("ProtocolError")}])
    try:
        await call(s, st, "network.cancel", {}, "t4")
        t4 = False
    except Exception as exc:
        t4 = (type(exc).__name__ == "ProtocolError" and s.connects == 0
              and classify_exc(exc) == "other")
    check("protocol-passthrough", t4)

    # T5 身份缺失/不匹配都判失败
    st = RunState()
    s = FakeSession([{"obj": {"ok": True, "data": {"mqtt": {"connected": True}}}}])
    got, _, why = await wait_mqtt_connected(s, st, 5, "A", "t5a")
    check("device-id-missing", got is False and why == "device_id_missing")
    st = RunState()
    s = FakeSession([{"obj": {"ok": True, "data": {"device_id": "B",
                                                   "mqtt": {"connected": True}}}}])
    got, _, why = await wait_mqtt_connected(s, st, 5, "A", "t5b")
    check("device-id-mismatch", got is False and why == "device_mismatch")

    # T6 verify_restore 结构化：config 异常仍保留两阶段
    st = RunState()
    s = FakeSession([{"raise": _transport_exc()}, {"raise": _transport_exc()},
                     {"obj": {"ok": True, "data": {"device_id": "A",
                                                   "mqtt": {"connected": True}}}}])
    v = await verify_restore(s, st, "h", 21883, "A")
    check("verify-structured", v["ok"] is False and len(v["stages"]) == 2
          and v["stages"][0]["stage"] == "config" and v["stages"][0]["ok"] is False
          and "error" in v["stages"][0])

    # T7 恢复循环发送次数 = 3（无内层重发）
    st = RunState()
    s = FakeSession([{"obj": {"ok": True, "data": {"mqtt": {"mqtt_host": "h", "mqtt_port": 21883}}}},
                     {"raise": _transport_exc()},
                     {"raise": _transport_exc()},
                     {"raise": _transport_exc()},
                     {"raise": _transport_exc()},
                     {"raise": _transport_exc()},
                     {"raise": _transport_exc()}])
    try:
        await case_c151(s, st, "A", None)
    except Exception:
        pass
    rst = [r for r in s.sent if str(r["id"]).startswith("c151rst")]
    check("restore-sends==3", len(rst) == 3, "sent=%d" % len(rst))

    # T8 串口 reader 异常 → stop 返回 False
    cap = SerialCapture("/dev/fake", tmp / "cap", proc_root=str(tmp / "none"),
                        require_exists=False)
    cap.read_error = "simulated"
    ok, why = cap.stop()
    check("serial-reader-error", ok is False and "reader" in why)

    # T9 C15-2 属性订阅先于抖动
    order = []

    class Tap:
        def __init__(self, h, p):
            order.append("tap")
            self.msgs = []
            self.connect_ts = 1.0
            self.suback_ts = 1.1

        def merged(self):
            return {}

        def stop(self):
            pass

    class S(FakeSession):
        async def send_request(self, req, timeout=0, retries=0):
            if req["cmd"] == "network.status":
                return FakeResp({"ok": True, "data": {"sta_connected": True,
                                                      "sta_has_ip": True,
                                                      "sta_ip": "1.2.3.4"}})
            if req["cmd"] == "device.status":
                return FakeResp({"ok": True, "data": {"device_id": "A",
                                                      "network": {"sta_connected": True}}})
            if req["cmd"] == "operation.get":
                return FakeResp({"ok": True, "data": {"state": "failed",
                                                      "error": "ap_not_found"}})
            return await super().send_request(req, timeout, retries)

    st = RunState()
    s = S([{"obj": {"ok": True, "data": {"operation_id": "op"}}}])
    await case_c152(s, st, "A", Tap)
    check("c152-tap-before-churn", order[:1] == ["tap"])

    # T10 C2-3 trial 与 job 不一致 → 必须 FAIL
    class S10(FakeSession):
        async def send_request(self, req, timeout=0, retries=0):
            c = req["cmd"]
            if c == "network.tryconnect":
                return FakeResp({"ok": True, "data": {"operation_id": "opX"}})
            if c == "network.status":
                return FakeResp({"ok": True, "data": {"trial": {"state": "got_ip",
                                                                "trial_id": "t",
                                                                "trial_ssid": "OTHER"}}})
            if c == "operation.get":
                return FakeResp({"ok": True, "data": {"state": "failed",
                                                      "error": "ap_not_found"}})
            if c == "device.status":
                return FakeResp({"ok": True, "data": {"device_id": "A",
                                                      "network": {"sta_connected": True}}})
            return await super().send_request(req, timeout, retries)
    st = RunState()
    t10 = await case_c23(S10([]), st)
    check("c23-trial-mismatch-fails", t10 is False)

    # T11 真实异常类型不可用时不得按类名兜底
    saved = (ConnectionLost, BleakError, BleakDBusError)
    globals()["ConnectionLost"] = globals()["BleakError"] = globals()["BleakDBusError"] = None
    try:
        e = type("BleakError", (Exception,), {})()
        kind = classify_exc(e)
    finally:
        globals()["ConnectionLost"], globals()["BleakError"], globals()["BleakDBusError"] = saved
    check("classify-no-name-fallback", kind == "other", kind)

    # T12 写命令 tryconnect 经 call()，不重发
    st = RunState()
    s = FakeSession([{"raise": _transport_exc()}])
    try:
        await w_tryconnect(s, st, "t12")
        t12 = False
    except OutcomeUnknown:
        t12 = (s.connects == 0 and len(s.sent) == 1)
    check("tryconnect-no-retry", t12)

    # T13 C15-2 终态不是失败 → 必须 FAIL
    class S13(FakeSession):
        async def send_request(self, req, timeout=0, retries=0):
            c = req["cmd"]
            if c == "network.status":
                return FakeResp({"ok": True, "data": {"sta_connected": True,
                                                      "sta_has_ip": True,
                                                      "sta_ip": "1.2.3.4"}})
            if c == "device.status":
                return FakeResp({"ok": True, "data": {"device_id": "A",
                                                      "network": {"sta_connected": True}}})
            if c == "network.tryconnect":
                return FakeResp({"ok": True, "data": {"operation_id": "op"}})
            if c == "operation.get":
                return FakeResp({"ok": True, "data": {"state": "success"}})
            return await super().send_request(req, timeout, retries)
    st = RunState()
    t13 = await case_c152(S13([]), st, "A", None)
    check("c152-terminal-wrong-fails", t13 is False)

    print("SELFTEST:", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--out", default="/home/tc/esp/workspace/iothub/artifacts/cmd02_15_stage3_20261122")
    a = ap.parse_args()
    OUT = Path(a.out)
    if a.selftest:
        raise SystemExit(asyncio.run(selftest()))
    raise SystemExit(asyncio.run(run_device()))
