#!/usr/bin/env python3
"""CMD-02 / CMD-15 阶段 3 设备回归 harness（v6）。

v6 针对 v5 初审的一个阻塞项、一个覆盖缺口与一个证据边界：
  V6-1 C15-1 属性采样改走统一消息模型：snapshot() + merge_attrs()，
       不再调用已删除的 AttrTap.merged()；采样失败计入用例返回值（停止后续用例）。
  V6-2 新增 C15-1 完整 mock 路径：订阅建立、窗口采样、finally 恢复与 stop 顺序；
       另有“窗口内只有 telemetry”变体，必须 FAIL 且仍完成恢复。
  V6-3 C15-2 窗口如实命名：w0=tryconnect 发送前、w1=STA 恢复+身份核验后，
       证据只覆盖“tryconnect 操作及恢复区间”，不声称覆盖瞬时 Wi-Fi 中断窗口。

v5 针对 v4 审查：SUBACK reason code 与本地 subscribe 返回码、属性消息
topic/ts 窗口隔离、AttrTap.stop() 显式 disconnect、C2-3 覆盖拆分。
v3/v4/v5 契约（写命令不重发、异常真实类型分类、身份贯穿、结构化核验、
串口单读者、C2-3 关联、C15-2 终态校验）保持不变。

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

def suback_ok(granted_codes):
    """SUBACK 判定：任一 reason code >= 0x80 即拒绝。"""
    for code in granted_codes or []:
        v = getattr(code, "value", code)
        try:
            if int(v) >= 0x80:
                return False, list(granted_codes)
        except (TypeError, ValueError):
            return False, list(granted_codes)
    return True, list(granted_codes or [])


def merge_attrs(msgs, t0, t1):
    """只取 [t0,t1] 窗口内、topic 为 attributes 的消息；返回 (merged, count, tel_count)。"""
    merged = {}
    n_attr = n_tel = 0
    for m in msgs:
        if not (t0 <= m["ts"] <= t1):
            continue
        if m["topic"].endswith("/attributes"):
            n_attr += 1
            merged.update(m["obj"])
        elif m["topic"].endswith("/telemetry"):
            n_tel += 1
    return merged, n_attr, n_tel


class AttrTap:
    """订阅属性/遥测；必须 CONNACK + SUBACK 成功才返回。消息带 topic/ts。"""

    def __init__(self, host, port, timeout=10.0):
        import paho.mqtt.client as mqtt
        self.msgs = []
        self.lock = threading.Lock()
        self.connected = threading.Event()
        self.subacked = threading.Event()
        self.connect_ts = None
        self.suback_ts = None
        self.rc = None
        self.suback_codes = None
        self.cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, protocol=mqtt.MQTTv311)
        self.cli.on_message = self._on
        self.cli.on_connect = self._on_connect
        self.cli.on_subscribe = self._on_subscribe
        self.cli.connect(host, port, keepalive=30)
        self.cli.loop_start()
        if not self.connected.wait(timeout):
            self.stop()
            raise RuntimeError("mqtt CONNACK 未确认")
        rc, _mid = self.cli.subscribe([("v1/devices/me/attributes", 1),
                                       ("v1/devices/me/telemetry", 1)])
        if int(rc) != int(mqtt.MQTT_ERR_SUCCESS):
            self.stop()
            raise RuntimeError("subscribe() 本地返回码=%s" % rc)
        if not self.subacked.wait(timeout):
            self.stop()
            raise RuntimeError("mqtt SUBACK 未确认")
        ok, codes = suback_ok(self.suback_codes)
        if not ok:
            self.stop()
            raise RuntimeError("SUBACK 被拒: %r" % (codes,))

    def _on_connect(self, _c, _u, _flags, rc, _props=None):
        self.rc = int(rc) if rc is not None else -1
        self.connect_ts = time.time()
        if self.rc == 0:
            self.connected.set()

    def _on_subscribe(self, _c, _u, _mid, granted, _props=None):
        self.suback_codes = list(granted or [])
        self.suback_ts = time.time()
        self.subacked.set()

    def _on(self, _c, _u, msg):
        try:
            obj = json.loads(msg.payload.decode("utf-8", "replace"))
        except Exception:  # noqa: BLE001
            return
        with self.lock:
            self.msgs.append({"topic": msg.topic, "ts": time.time(), "obj": obj})

    def snapshot(self):
        with self.lock:
            return list(self.msgs)

    def stop(self):
        """显式断开再停网络循环，避免遗留 broker 连接。"""
        try:
            self.cli.disconnect()
        except Exception:  # noqa: BLE001
            pass
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
        "pass": st.fatal is None and bool(st.results)
                and all(r["ok"] for r in st.results
                        if not r.get("not_covered")),
        "not_covered": [r["case"] for r in st.results if r.get("not_covered")],
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
        st.rec("C2-3 absent 路径（无 trial 残留/终态）", False, "未取得 operation_id")
        st.rec("C2-3 got_ip trial/job 关联", False, "无可关联的 operation",
               not_covered=True)
        return False
    states, absent_issues, assoc_issues = [], [], []
    trial_seen = False
    for i in range(5):
        d = (await call(sess, st, "network.status", {}, "c23s%d" % i)).get("data", {})
        trial = d.get("trial") or {}
        stt = trial.get("state")
        states.append(stt)
        if stt == "got_ip":
            trial_seen = True
            if not trial.get("trial_id") or not trial.get("trial_ssid"):
                assoc_issues.append("got_ip 缺 trial_id/trial_ssid")
            else:
                if trial.get("trial_ssid") != ssid:
                    assoc_issues.append("trial_ssid=%r 与本次 job ssid=%r 不符" %
                                        (trial.get("trial_ssid"), ssid))
                op = (await call(sess, st, "operation.get", {"operation_id": oidA},
                                 "c23g%d" % i)).get("data", {})
                if op.get("state") not in ("got_ip", "committing", "success"):
                    assoc_issues.append("trial=got_ip 但 job state=%s" % op.get("state"))
        elif trial.get("trial_id") or trial.get("trial_ssid"):
            absent_issues.append("非 got_ip 却带 trial 字段")
        await asyncio.sleep(0.3)
    term = await wait_terminal(sess, st, oidA)
    tail = ((await call(sess, st, "network.status", {}, "c23end")).get("data", {}) or {}).get("trial") or {}
    if tail.get("state") != "idle":
        absent_issues.append("终态 trial=%s（期望 idle）" % tail.get("state"))
    if term.get("state") != "failed":
        absent_issues.append("job 终态=%s（期望 failed）" % term.get("state"))
    ok_absent = bool(okA and not absent_issues)
    st.rec("C2-3 absent 路径（无 trial 残留/终态）", ok_absent,
           "states=%s issues=%s job=%s/%s" %
           (states, absent_issues, term.get("state"), term.get("error")),
           coverage="absent_path_checked")
    if trial_seen:
        ok_assoc = not assoc_issues
        st.rec("C2-3 got_ip trial/job 关联", ok_assoc, "issues=%s" % assoc_issues)
        return bool(ok_absent and ok_assoc)
    st.rec("C2-3 got_ip trial/job 关联", False,
           "NOT_COVERED：absent SSID 不产生 trial，需已知口令 AP 才能覆盖",
           not_covered=True)
    return ok_absent


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


async def case_c151(sess, st, expect_device, attr_factory=None, attr_wait=20.0):
    pre = ((await call(sess, st, "config.get", {}, "c151pre", 30)).get("data", {}) or {}).get("mqtt", {})
    ph, pp = pre.get("mqtt_host"), pre.get("mqtt_port")
    set_ok = conn_ok = restored = False
    attr_ok = True  # 未提供 attr_factory 时不评估属性采样
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
                # v6：统一消息模型。窗口从 SUBACK 确认起；最多等 20s 取首条 attributes。
                w0 = getattr(tap, "suback_ts", None) or time.time()
                merged, n_attr, n_tel = {}, 0, 0
                deadline = time.monotonic() + attr_wait
                while time.monotonic() < deadline:
                    await asyncio.sleep(1.0)
                    merged, n_attr, n_tel = merge_attrs(tap.snapshot(), w0, time.time())
                    if n_attr:
                        break
                w1 = time.time()
                need = {"net_mode", "sta_ssid", "sta_has_ip", "sta_ip", "uart_baudrate"}
                field_ok = n_attr > 0 and need.issubset(set(merged.keys()))
                consistent = (merged.get("net_mode") == "sta"
                              and merged.get("sta_ssid") == "CMCC-502"
                              and merged.get("sta_has_ip") is True
                              and bool(merged.get("sta_ip")))
                attr_ok = bool(field_ok and consistent)
                st.rec("C15-1 属性快照", attr_ok,
                       "窗口 attributes=%d telemetry=%d fields=%s consistent=%s" %
                       (n_attr, n_tel, field_ok, consistent),
                       window={"w0": w0, "w1": w1, "start": "SUBACK 确认",
                               "end": "采样结束"},
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
    return bool(set_ok and conn_ok and attr_ok and restored and verify["ok"])


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
        # 证据边界（v6）：w0=tryconnect 发送前，w1=STA 恢复+身份核验后。
        # 本用例只声称覆盖“tryconnect 操作及恢复区间”，不声称覆盖瞬时 Wi-Fi 断开窗口。
        w0 = time.time()
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
        w1 = time.time()
        post = (await call(sess, st, "network.status", {}, "c152post")).get("data", {})
        if tap is None:
            st.rec("C15-2 tryconnect 及恢复（无属性采样）", bool(okA and restored),
                   "mock/无属性采样；恢复=%s" % restored)
            return bool(okA and restored)
        merged, n_attr, n_tel = merge_attrs(tap.snapshot(), w0, w1)
        if n_attr == 0:
            st.rec("C15-2 tryconnect 及恢复区间属性自洽", False,
                   "区间内 attributes=0（telemetry=%d），不能用遥测冒充属性采样" % n_tel,
                   window={"w0": w0, "w1": w1,
                           "semantics": "operation_and_recovery_interval"})
            return False
        torn = ((merged.get("sta_has_ip") is False and bool(merged.get("sta_ip")))
                or (merged.get("sta_has_ip") is True and not merged.get("sta_ip")))
        match = (post.get("sta_has_ip") == merged.get("sta_has_ip")
                 and post.get("sta_ip") == merged.get("sta_ip"))
        ok = bool(okA and restored and not torn and match)
        st.rec("C15-2 tryconnect 及恢复区间属性自洽", ok,
               "窗口 attributes=%d telemetry=%d 恢复=%s torn=%s 对读=%s" %
               (n_attr, n_tel, restored, torn, match),
               window={"w0": w0, "w1": w1,
                       "semantics": "operation_and_recovery_interval"},
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
    n_checks = n_pass = 0

    def check(name, cond, extra=""):
        nonlocal ok_all, n_checks, n_pass
        n_checks += 1
        if cond:
            n_pass += 1
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

        def snapshot(self):
            return []

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

    # T15 SUBACK 拒绝码判定
    ok1, _ = suback_ok([0x87])
    ok2, _ = suback_ok([1, 2])
    check("suback-reject-detected", ok1 is False and ok2 is True)

    # T16 属性窗口隔离：窗口内只有 telemetry → n_attr=0，窗口外 attributes 不计
    msgs = [{"topic": "v1/devices/me/attributes", "ts": 1, "obj": {"old": 1}},
            {"topic": "v1/devices/me/telemetry", "ts": 5, "obj": {"t": 1}},
            {"topic": "v1/devices/me/attributes", "ts": 10, "obj": {"sta_has_ip": True}}]
    merged, n_attr, n_tel = merge_attrs(msgs, 4, 11)
    check("attr-window-isolation", n_attr == 1 and n_tel == 1
          and "old" not in merged and merged.get("sta_has_ip") is True)

    # T17 stop 顺序：disconnect → loop_stop
    class FakeCli:
        def __init__(self):
            self.order = []

        def disconnect(self):
            self.order.append("disconnect")

        def loop_stop(self):
            self.order.append("loop_stop")

    tap = AttrTap.__new__(AttrTap)
    tap.cli = FakeCli()
    tap.stop()
    check("mqtt-disconnect-before-loopstop", tap.cli.order == ["disconnect", "loop_stop"])

    # T18 C2-3 absent 路径：字段/终态 PASS，关联标记 NOT_COVERED
    class S18(FakeSession):
        async def send_request(self, req, timeout=0, retries=0):
            c = req["cmd"]
            if c == "network.tryconnect":
                return FakeResp({"ok": True, "data": {"operation_id": "op"}})
            if c == "network.status":
                return FakeResp({"ok": True, "data": {"trial": {"state": "idle"}}})
            if c == "operation.get":
                return FakeResp({"ok": True, "data": {"state": "failed",
                                                      "error": "ap_not_found"}})
            if c == "device.status":
                return FakeResp({"ok": True, "data": {"device_id": "A",
                                                      "network": {"sta_connected": True}}})
            return await super().send_request(req, timeout, retries)
    st = RunState()
    t18 = await case_c23(S18([]), st)
    nc = [r for r in st.results if r.get("not_covered")]
    check("c23-absent-coverage-split", t18 is True and len(nc) == 1
          and "NOT_COVERED" in nc[0]["detail"])

    # T19 C15-1 完整路径：订阅建立 → 窗口采样 PASS → finally stop + 恢复
    # mock 故意只提供 snapshot()（不提供 merged()），防止旧接口回归再次漏检。
    ev19 = []

    class Tap19:
        def __init__(self, h, p):
            ev19.append(("tap", h, p))
            self.suback_ts = 100.0

        def snapshot(self):
            now = time.time()
            return [
                {"topic": "v1/devices/me/attributes", "ts": now,
                 "obj": {"net_mode": "sta", "sta_ssid": "CMCC-502",
                         "sta_has_ip": True, "sta_ip": "1.2.3.4",
                         "uart_baudrate": 115200}},
                {"topic": "v1/devices/me/telemetry", "ts": now, "obj": {"t": 1}},
            ]

        def stop(self):
            ev19.append(("stop",))

    class S19(FakeSession):
        async def send_request(self, req, timeout=0, retries=0):
            c = req["cmd"]
            if c == "config.get":
                return FakeResp({"ok": True, "data": {"device_id": "A",
                                                      "mqtt": {"mqtt_host": "h",
                                                               "mqtt_port": 21883}}})
            if c == "config.set":
                return FakeResp({"ok": True, "data": {}})
            if c == "device.status":
                return FakeResp({"ok": True, "data": {"device_id": "A",
                                                      "mqtt": {"connected": True}}})
            return await super().send_request(req, timeout, retries)

    st = RunState()
    try:
        t19 = await case_c151(S19([]), st, "A", Tap19, attr_wait=5.0)
    except Exception as exc:  # noqa: BLE001
        t19 = False
        print("  T19 exception:", type(exc).__name__, exc)
    snap19 = [r for r in st.results if r["case"] == "C15-1 属性快照"]
    rest19 = [r for r in st.results if r["case"] == "C15-1 恢复并核验"]
    check("c151-attr-path", t19 is True and len(snap19) == 1 and snap19[0]["ok"]
          and snap19[0].get("window", {}).get("start") == "SUBACK 确认"
          and bool(rest19) and rest19[0]["ok"]
          and bool(ev19) and ev19[0][0] == "tap" and ev19[-1] == ("stop",))

    # T20 C15-1 窗口内只有 telemetry → 属性采样必须 FAIL，但 finally 仍恢复并 stop
    ev20 = []

    class Tap20:
        def __init__(self, h, p):
            self.suback_ts = 100.0

        def snapshot(self):
            return [{"topic": "v1/devices/me/telemetry", "ts": time.time(),
                     "obj": {"t": 1}}]

        def stop(self):
            ev20.append("stop")

    st = RunState()
    t20 = await case_c151(S19([]), st, "A", Tap20, attr_wait=2.0)
    snap20 = [r for r in st.results if r["case"] == "C15-1 属性快照"]
    rest20 = [r for r in st.results if r["case"] == "C15-1 恢复并核验"]
    check("c151-no-attr-fails", t20 is False and bool(snap20)
          and snap20[0]["ok"] is False and "attributes=0" in snap20[0]["detail"]
          and bool(rest20) and rest20[0]["ok"] and ev20 == ["stop"])

    print("SELFTEST:", "PASS" if ok_all else "FAIL", "(%d/%d)" % (n_pass, n_checks))
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
