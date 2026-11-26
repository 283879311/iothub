#!/usr/bin/env python3
"""C15-2 单项探针（test-only）：错误口令 tryconnect + attributes/STA 对读。

边界：
- 只执行 C15-2：不做 C2、不切 MQTT 端口、不写配置。
- 21883 attributes 使用 IOTHUB_MQTT_TOKEN（仅环境）；telemetry 不订阅。
- 写命令 network.tryconnect 只发一次；结果不确定即停止，不重发。
- 只读轮询在总预算内允许单次超时继续，不因一次抖动终止。

用法：
  IOTHUB_BLE_USER=... IOTHUB_BLE_PASSWORD=... IOTHUB_MQTT_TOKEN=... \
  .venv-bleak/bin/python tools/review/cmd15_c152_probe_v1.py \
    --expect-device IotHub-65D8 --expect-fw 1.3-956afcd \
    --expect-sha c231c9501 --out artifacts/c152_probe_YYYYmmdd
  ... --selftest   # 仅 mock，不连设备
"""
import argparse
import asyncio
import fcntl
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

import paho.mqtt.client as mqtt

ADDR = "10:97:BD:D3:65:DA"
MQTT_HOST = "139.9.93.245"
MQTT_PORT = 21883
ATTR_TOPIC = "v1/devices/me/attributes"
P0_PATH = Path(__file__).resolve().parent.parent / "ble_p0" / "p0_acceptance.py"
P0_SHA256 = "3882a2e050c02ca8ec854915dd7aba528a689e23758f1d709d7bbd51bcaa4673"
TERMINAL = {"failed", "cancelled", "expired", "success"}
READ_CMDS = {"device.status", "network.status", "config.get", "operation.get"}


def rc_int(rc):
    return -1 if rc is None else int(getattr(rc, "value", rc))


def suback_ok(codes):
    codes = list(codes or [])
    return bool(codes) and all(int(getattr(c, "value", c)) < 0x80 for c in codes)


def merge_attrs(msgs, t0, t1):
    merged, n_attr = {}, 0
    for m in msgs:
        if not (t0 <= m["ts"] <= t1):
            continue
        if m["topic"].endswith("/attributes"):
            n_attr += 1
            merged.update(m["obj"])
    return merged, n_attr


def evaluate(terminal, attrs, post, expect_ssid):
    """纯判定：C15-2 的终态、attributes 配置字段与恢复后网络状态。"""
    issues = []
    term_ok = (terminal.get("state") == "failed"
               and terminal.get("error") in ("wrong_password", "ap_not_found"))
    if not term_ok:
        issues.append("terminal=%s/%s" % (terminal.get("state"), terminal.get("error")))
    need = {"net_mode", "sta_ssid", "sta_ip", "uart_baudrate"}
    if not need.issubset(set(attrs.keys())):
        issues.append("attrs_missing=%s" % sorted(need - set(attrs.keys())))
    if attrs.get("net_mode") != "sta":
        issues.append("net_mode=%s" % attrs.get("net_mode"))
    if attrs.get("sta_ssid") != expect_ssid:
        issues.append("attr_ssid=%s" % attrs.get("sta_ssid"))
    if attrs.get("uart_baudrate") != 115200:
        issues.append("uart_baudrate=%s" % attrs.get("uart_baudrate"))
    if post.get("sta_has_ip") is not True:
        issues.append("post_sta_has_ip=%s" % post.get("sta_has_ip"))
    if not post.get("sta_ip"):
        issues.append("post_sta_ip_empty")
    if attrs.get("sta_ip") != post.get("sta_ip"):
        issues.append("ip_mismatch attr=%s post=%s" % (attrs.get("sta_ip"), post.get("sta_ip")))
    trial = (post.get("trial") or {}).get("state")
    if trial != "idle":
        issues.append("trial=%s" % trial)
    return (not issues), issues, "terminal=%s/%s attrs=%d post_ip=%s" % (
        terminal.get("state"), terminal.get("error"), len(attrs), post.get("sta_ip"))


def terminal_ok(term_ok, term):
    """ok=False 的响应不得作为有效终态证据。"""
    return bool(term_ok
                and term.get("state") == "failed"
                and term.get("error") in ("wrong_password", "ap_not_found"))


def load_p0(path=P0_PATH):
    data = Path(path).read_bytes()
    got = hashlib.sha256(data).hexdigest()
    if got != P0_SHA256:
        raise RuntimeError("p0_acceptance SHA-256 不符: %s" % got)
    spec = importlib.util.spec_from_file_location("p0_acceptance", path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


class AttrTap:
    """21883 attributes-only 订阅；token 只从环境传入，不记录。"""

    def __init__(self, host, port, token, timeout=15.0):
        self.msgs, self.lock = [], threading.Lock()
        self.connected, self.subacked = threading.Event(), threading.Event()
        self.suback_ts, self.rc, self.codes = None, None, None
        self.cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, protocol=mqtt.MQTTv311)
        self.cli.username_pw_set(token)
        self.cli.on_connect = self._on_connect
        self.cli.on_subscribe = self._on_subscribe
        self.cli.on_message = self._on
        self.cli.connect(host, port, keepalive=30)
        self.cli.loop_start()
        if not self.connected.wait(timeout):
            self.stop(); raise RuntimeError("mqtt CONNACK 未确认")
        rc, _mid = self.cli.subscribe([(ATTR_TOPIC, 1)])
        if int(rc) != int(mqtt.MQTT_ERR_SUCCESS):
            self.stop(); raise RuntimeError("subscribe 本地返回码=%s" % rc)
        if not self.subacked.wait(timeout):
            self.stop(); raise RuntimeError("mqtt SUBACK 未确认")
        if not suback_ok(self.codes):
            self.stop(); raise RuntimeError("SUBACK 被拒: %r" % (self.codes,))

    def _on_connect(self, _c, _u, _f, rc, _p=None):
        self.rc = rc_int(rc)
        self.connect_ts = time.time()
        if self.rc == 0:
            self.connected.set()

    def _on_subscribe(self, _c, _u, _mid, granted, _p=None):
        self.codes = list(granted or [])
        self.suback_ts = time.time()
        self.subacked.set()

    def _on(self, _c, _u, msg):
        try:
            obj = json.loads(msg.payload.decode("utf-8", "replace"))
        except Exception:
            return
        with self.lock:
            self.msgs.append({"topic": msg.topic, "ts": time.time(), "obj": obj})

    def snapshot(self):
        with self.lock:
            return list(self.msgs)

    def stop(self):
        try:
            self.cli.disconnect()
        except Exception:
            pass
        self.cli.loop_stop()


class SerialCapture:
    """只读串口采集；flock + stty，证据落盘。"""

    def __init__(self, port, outdir, require_exists=True):
        self.port, self.outdir, self.require_exists = port, Path(outdir), require_exists
        self.fd = self.file = self.thread = None
        self.stop_ev = threading.Event(); self.locked = False
        self.read_error = None

    def _other_readers(self):
        try:
            r = subprocess.run(["fuser", self.port], capture_output=True, text=True)
            pids = [p for p in r.stdout.split() if p.isdigit() and int(p) != os.getpid()]
            return pids
        except Exception:
            return []

    def start(self):
        if self.require_exists and not os.path.exists(self.port):
            raise RuntimeError("串口不存在: %s" % self.port)
        pids = self._other_readers()
        if pids:
            raise RuntimeError("串口被其他进程占用: %s" % ",".join(pids))
        self.outdir.mkdir(parents=True, exist_ok=True)
        self.fd = os.open(self.port, os.O_RDONLY | os.O_NONBLOCK)
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB); self.locked = True
        except OSError as exc:
            os.close(self.fd); self.fd = None
            raise RuntimeError("串口 flock 失败: %s" % exc)
        subprocess.run(["stty", "-F", self.port, "115200", "raw", "-echo"],
                       check=True, capture_output=True, text=True)
        self.file = open(self.outdir / "serial.txt", "ab")
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()

    def _loop(self):
        import select
        while not self.stop_ev.is_set():
            try:
                r, _, _ = select.select([self.fd], [], [], 0.2)
                if not r:
                    continue
                data = os.read(self.fd, 4096)
                if data and self.file:
                    self.file.write(data); self.file.flush()
            except Exception as exc:
                self.read_error = "%s: %s" % (type(exc).__name__, exc)
                self.stop_ev.set(); return

    def stop(self):
        self.stop_ev.set()
        if self.thread:
            self.thread.join(timeout=3)
        if self.fd is not None:
            try:
                if self.locked:
                    fcntl.flock(self.fd, fcntl.LOCK_UN)
            except Exception:
                pass
            os.close(self.fd)
        if self.file:
            self.file.close()
        if self.thread and self.thread.is_alive():
            return False, "reader 未在 3s 内退出"
        if self.read_error:
            return False, "reader 异常: " + self.read_error
        return True, ""


async def call(sess, cmd, args, rid, timeout=10):
    req = {"id": rid, "version": "1.0", "cmd": cmd, "args": args,
           "token": sess.token}
    resp = await sess.send_request(req, timeout=timeout, retries=0)
    return resp.obj


async def poll_read(sess, cmd, pred, total, prefix, args=None, interval=2.0,
                    timeout_cap=10.0, backoff=0.5, max_backoff=5.0):
    """只读轮询：单次超时/异常记录后按有界退避继续；单次等待不超过剩余预算。"""
    deadline = time.monotonic() + total
    samples = []
    last = {}
    delay = backoff
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0.05:
            break
        per = min(timeout_cap, remaining)
        rid = "%s%d" % (prefix, int(time.time() * 1000))
        try:
            obj = await call(sess, cmd, args or {}, rid, timeout=per)
        except asyncio.TimeoutError:
            samples.append({"ts": time.time(), "kind": "timeout",
                            "budget_left_s": round(remaining, 3),
                            "request_timeout_s": round(per, 3)})
            await asyncio.sleep(min(delay, max(0.0, deadline - time.monotonic())))
            delay = min(max_backoff, delay * 2)
            continue
        except Exception as exc:
            samples.append({"ts": time.time(), "kind": type(exc).__name__,
                            "budget_left_s": round(remaining, 3),
                            "request_timeout_s": round(per, 3)})
            await asyncio.sleep(min(delay, max(0.0, deadline - time.monotonic())))
            delay = min(max_backoff, delay * 2)
            continue
        data = obj.get("data") or {}
        last = data
        samples.append({"ts": time.time(), "kind": "ok", "ok": obj.get("ok"),
                        "state": data.get("state"), "data": data})
        if obj.get("ok") and pred(data):
            return True, data, samples
        delay = backoff
        await asyncio.sleep(min(interval, max(0.0, deadline - time.monotonic())))
    return False, last, samples


def device_preconditions(env, expect_fw, expect_sha, expect_device):
    missing = [k for k in ("IOTHUB_BLE_USER", "IOTHUB_BLE_PASSWORD",
                           "IOTHUB_MQTT_TOKEN") if not env.get(k)]
    if not expect_fw: missing.append("--expect-fw")
    if not expect_sha: missing.append("--expect-sha")
    if not expect_device: missing.append("--expect-device")
    return (not missing), "缺少: " + ", ".join(missing) if missing else ""


async def run_device(expect_fw, expect_sha, expect_device, outdir):
    ok, why = device_preconditions(os.environ, expect_fw, expect_sha, expect_device)
    if not ok:
        raise RuntimeError(why)
    p0 = load_p0()
    outdir = Path(outdir); outdir.mkdir(parents=True, exist_ok=True)
    results, attempts = [], []
    cap = None; sess = None; tap = None

    def rec(case, ok, detail, **extra):
        r = {"case": case, "ok": bool(ok), "detail": detail, "ts": time.time()}
        r.update(extra); results.append(r)
        print(("PASS " if ok else "FAIL ") + case + " :: " + str(detail), flush=True)

    try:
        cap = SerialCapture(os.environ.get("C152_SERIAL", "/dev/ttyUSB0"), outdir)
        cap.start()
        sess = p0.Session(cli=None, address=ADDR,
                          username=os.environ["IOTHUB_BLE_USER"],
                          password=os.environ["IOTHUB_BLE_PASSWORD"])
        await sess._connect_once(login_retries=1)
        st = (await call(sess, "device.status", {}, "pre", timeout=20)).get("data", {})
        ident_ok = (st.get("device_id") == expect_device
                    and st.get("firmware") == expect_fw
                    and st.get("build_sha") == expect_sha)
        rec("设备身份/基线", ident_ok,
            "device=%s fw=%s sha=%s" % (st.get("device_id"), st.get("firmware"),
                                        st.get("build_sha")))
        if not ident_ok:
            raise RuntimeError("身份或基线不符，停止")
        pre = (await call(sess, "network.status", {}, "pre", timeout=20)).get("data", {})
        pre_ok = (pre.get("sta_connected") is True and pre.get("sta_has_ip") is True
                  and pre.get("mqtt_connected") is True
                  and (pre.get("trial") or {}).get("state") == "idle")
        ssid = pre.get("sta_ssid")
        rec("前置状态", pre_ok, "sta=%s ip=%s mqtt=%s trial=%s ssid=%s" % (
            pre.get("sta_connected"), pre.get("sta_has_ip"), pre.get("mqtt_connected"),
            (pre.get("trial") or {}).get("state"), ssid))
        if not pre_ok or not ssid:
            raise RuntimeError("前置状态不满足，停止")
        cfg = (await call(sess, "config.get", {}, "cfg", timeout=20)).get("data", {})
        mqttc = cfg.get("mqtt") or {}
        cfg_ok = (mqttc.get("mqtt_host") == MQTT_HOST
                  and mqttc.get("mqtt_port") == MQTT_PORT)
        rec("MQTT 配置未变更", cfg_ok, "host=%s port=%s" % (
            mqttc.get("mqtt_host"), mqttc.get("mqtt_port")))
        if not cfg_ok:
            raise RuntimeError("MQTT 配置不是预期端口，停止")

        tap = AttrTap(MQTT_HOST, MQTT_PORT, os.environ["IOTHUB_MQTT_TOKEN"])
        rec("21883 attributes 订阅", True,
            "connack_ts=%.3f suback_ts=%.3f" % (
                getattr(tap, "connect_ts", 0) or 0, tap.suback_ts or 0))

        w0 = time.time()
        wrong = "c152-wrong-%d" % int(time.time())
        try:
            res = await call(sess, "network.tryconnect",
                             {"ssid": ssid, "password": wrong,
                              "idempotency_key": "c152-%d" % int(time.time())},
                             "churn", timeout=20)
        except asyncio.TimeoutError:
            attempts.append({"cmd": "network.tryconnect", "action": "outcome_unknown_timeout"})
            rec("C15-2 抖动请求", False, "tryconnect 超时；不重发，停止")
            raise RuntimeError("tryconnect 结果不确定，停止")
        except Exception as exc:
            attempts.append({"cmd": "network.tryconnect",
                             "action": "outcome_unknown_%s" % type(exc).__name__})
            rec("C15-2 抖动请求", False, "tryconnect 异常 %s；不重发，停止" % type(exc).__name__)
            raise RuntimeError("tryconnect 结果不确定，停止")
        data = res.get("data") or {}
        oid = data.get("operation_id")
        rec("C15-2 抖动请求", bool(res.get("ok") and oid),
            "ok=%s code=%s op=%s" % (res.get("ok"), (res.get("error") or {}).get("code"), oid))
        if not (res.get("ok") and oid):
            raise RuntimeError("tryconnect 未被受理，停止")

        term_ok, term, term_samples = await poll_read(
            sess, "operation.get", lambda d: d.get("state") in TERMINAL,
            45, "term", args={"operation_id": oid}, interval=1.0,
            timeout_cap=15.0, backoff=0.5, max_backoff=5.0)
        term_valid = terminal_ok(term_ok, term)
        rec("C15-2 终态", term_valid,
            "state=%s error=%s samples=%d term_ok=%s" % (
                term.get("state"), term.get("error"), len(term_samples), term_ok),
            samples=term_samples)
        if not term_valid:
            raise RuntimeError("终态不符或响应 ok=False，停止")

        recovered, post, samples = await poll_read(
            sess, "network.status",
            lambda d: (d.get("sta_connected") is True and d.get("sta_has_ip") is True
                       and d.get("internet_verified") is True
                       and d.get("mqtt_connected") is True
                       and (d.get("trial") or {}).get("state") == "idle"),
            70, "rec", interval=2.0, timeout_cap=10.0)
        rec("STA 恢复", recovered, "samples=%d last_trial=%s" % (
            len(samples), (post.get("trial") or {}).get("state")))
        if not recovered:
            raise RuntimeError("STA 未在预算内恢复，停止")

        w1 = time.time()
        attrs, n_attr = merge_attrs(tap.snapshot(), w0, w1)
        ok2, issues, detail = evaluate(term, attrs, post, ssid)
        rec("C15-2 属性/状态一致性", ok2, detail + " issues=%s" % issues,
            window={"w0": w0, "w1": w1}, attrs=attrs,
            post={"sta_has_ip": post.get("sta_has_ip"),
                  "sta_ip": post.get("sta_ip"),
                  "trial": (post.get("trial") or {}).get("state")},
            n_attrs=n_attr)
    except Exception as exc:
        results.append({"case": "fatal", "ok": False,
                        "detail": "%s: %s" % (type(exc).__name__, exc), "ts": time.time()})
        print("FATAL", type(exc).__name__, exc, flush=True)
    finally:
        if tap is not None:
            tap.stop()
        if sess is not None:
            try:
                if sess.cli is not None:
                    await sess.cli.disconnect()
            except Exception:
                pass
        if cap is not None:
            okc, whyc = cap.stop()
            if not okc:
                results.append({"case": "串口采集完整性", "ok": False,
                                "detail": whyc, "ts": time.time()})
    verdict = bool(results) and all(r["ok"] for r in results)
    payload = {"verdict": "PASS" if verdict else "FAIL",
               "results": results, "attempts": attempts,
               "preconditions": {"expect_fw": expect_fw, "expect_sha": expect_sha,
                                 "expect_device": expect_device}}
    (outdir / "c152_results.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    print("SUMMARY:", payload["verdict"], flush=True)
    return 0 if verdict else 1


async def selftest():
    checks = []
    def chk(name, cond):
        checks.append(bool(cond)); print(("T " + name + ": " + ("PASS" if cond else "FAIL")))

    class RC:
        value = 0
    chk("rc-int", rc_int(0) == 0 and rc_int(RC()) == 0 and rc_int(None) == -1)
    chk("suback-ok", suback_ok([1]) and not suback_ok([128]) and not suback_ok([]))
    msgs = [{"topic": ATTR_TOPIC, "ts": 5, "obj": {"net_mode": "sta"}},
            {"topic": ATTR_TOPIC, "ts": 15, "obj": {"sta_ssid": "S"}},
            {"topic": "v1/devices/me/telemetry", "ts": 15, "obj": {"x": 1}}]
    merged, n = merge_attrs(msgs, 10, 20)
    chk("merge-window", n == 1 and merged.get("sta_ssid") == "S"
        and "net_mode" not in merged)
    good_term = {"state": "failed", "error": "wrong_password"}
    good_attrs = {"net_mode": "sta", "sta_ssid": "S", "sta_ip": "1.2.3.4",
                  "uart_baudrate": 115200}
    good_post = {"sta_has_ip": True, "sta_ip": "1.2.3.4",
                 "trial": {"state": "idle"}}
    chk("verdict-pass", evaluate(good_term, good_attrs, good_post, "S")[0])
    bad_post = dict(good_post); bad_post["sta_ip"] = "5.6.7.8"
    chk("verdict-ip-mismatch", not evaluate(good_term, good_attrs, bad_post, "S")[0])
    bad_term = {"state": "success", "error": None}
    chk("verdict-terminal", not evaluate(bad_term, good_attrs, good_post, "S")[0])
    ok, why = device_preconditions({}, "fw", "sha", "dev")
    chk("preconditions-refuse", not ok and "IOTHUB_MQTT_TOKEN" in why)

    class FakeSess:
        def __init__(self, script):
            self.script, self.calls, self.token = list(script), 0, "t"

        async def send_request(self, req, timeout=0, retries=0, **kw):
            self.calls += 1
            item = self.script.pop(0) if self.script else {"ok": True, "data": {}}
            if isinstance(item, BaseException):
                raise item
            return type("Resp", (), {"obj": item})()

    class AlwaysTimeout:
        def __init__(self):
            self.calls, self.token = 0, "t"

        async def send_request(self, req, timeout=0, retries=0, **kw):
            self.calls += 1
            raise asyncio.TimeoutError()

    s = FakeSess([asyncio.TimeoutError(), asyncio.TimeoutError(),
                  {"ok": True, "data": {"state": "ok"}}])
    okp, _, ps = await poll_read(s, "device.status", lambda d: True, 2.0, "t",
                                 interval=0.01, timeout_cap=0.05,
                                 backoff=0.01, max_backoff=0.02)
    chk("poll-timeout-recover", okp and s.calls == 3
        and [x["kind"] for x in ps] == ["timeout", "timeout", "ok"])

    s = FakeSess([ValueError("x"), ValueError("x"),
                  {"ok": True, "data": {"state": "ok"}}])
    okp, _, ps = await poll_read(s, "device.status", lambda d: True, 2.0, "t",
                                 interval=0.01, timeout_cap=0.05,
                                 backoff=0.01, max_backoff=0.02)
    chk("poll-exception-recover", okp and s.calls == 3
        and [x["kind"] for x in ps] == ["ValueError", "ValueError", "ok"])

    s = AlwaysTimeout(); t0 = time.monotonic()
    okp, _, ps = await poll_read(s, "network.status", lambda d: True, 0.12, "t",
                                 interval=0.01, timeout_cap=0.05,
                                 backoff=0.02, max_backoff=0.05)
    chk("poll-budget-exhausted", not okp and s.calls <= 6
        and all(x["kind"] == "timeout" for x in ps)
        and time.monotonic() - t0 < 0.5)
    chk("poll-timeout-bounded", all(
        x["request_timeout_s"] <= x["budget_left_s"] + 0.001 for x in ps))

    s = FakeSess([asyncio.TimeoutError(),
                  {"ok": True, "data": {"state": "failed", "error": "wrong_password"}}])
    okp, term, ps = await poll_read(
        s, "operation.get", lambda d: d.get("state") in TERMINAL, 1.0, "t",
        interval=0.01, timeout_cap=0.05, backoff=0.01, max_backoff=0.02)
    chk("opget-timeout-then-terminal", okp and term.get("state") == "failed"
        and [x["kind"] for x in ps] == ["timeout", "ok"])

    s = AlwaysTimeout()
    okp, _, ps = await poll_read(
        s, "operation.get", lambda d: d.get("state") in TERMINAL, 0.12, "t",
        interval=0.01, timeout_cap=0.05, backoff=0.02, max_backoff=0.05)
    chk("opget-budget-exhausted", not okp and s.calls <= 6
        and all(x["kind"] == "timeout" for x in ps))

    class OkFalseTerminal:
        def __init__(self):
            self.calls, self.token = 0, "t"

        async def send_request(self, req, timeout=0, retries=0, **kw):
            self.calls += 1
            return type("Resp", (), {"obj": {"ok": False, "data": {
                "state": "failed", "error": "wrong_password"}}})()

    s = OkFalseTerminal()
    okp, term, ps = await poll_read(
        s, "operation.get", lambda d: d.get("state") in TERMINAL, 0.12, "t",
        interval=0.01, timeout_cap=0.05, backoff=0.02, max_backoff=0.05)
    chk("opget-ok-false-rejected", not okp
        and all(x["kind"] == "ok" and x["ok"] is False for x in ps))
    chk("terminal-requires-ok", terminal_ok(True, good_term)
        and not terminal_ok(False, good_term)
        and not terminal_ok(True, {"state": "success", "error": None}))

    print("SELFTEST:", "PASS" if all(checks) else "FAIL",
          "(%d/%d)" % (sum(checks), len(checks)))
    return 0 if all(checks) else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--expect-device"); ap.add_argument("--expect-fw")
    ap.add_argument("--expect-sha"); ap.add_argument("--out")
    a = ap.parse_args()
    if a.selftest:
        return asyncio.run(selftest())
    if not a.out:
        print("REFUSE: --out required"); return 2
    try:
        return asyncio.run(run_device(a.expect_fw, a.expect_sha, a.expect_device, a.out))
    except RuntimeError as exc:
        print("REFUSE:", exc); return 2


if __name__ == "__main__":
    sys.exit(main())
