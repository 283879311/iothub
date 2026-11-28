#!/usr/bin/env python3
"""C15-2 MQTT attributes consistency harness (Host-level; device path separate).

Attributes coherence uses only fields the firmware publishes on
`v1/devices/me/attributes` (net_mode/sta_ssid/sta_ip/uart_baudrate).
`sta_has_ip` is cross-checked with a later BLE `network.status` sample.

Decision logic lives in `c152_policy.py` (pure, unit-tested). This file wires
I/O and exposes `run_case()` with injectable dependencies so the runner can be
tested with fake sessions/taps/clocks (see tests/c152_policy).

Usage (device run requires separate authorization):
  IOTHUB_BLE_USER=... IOTHUB_BLE_PASSWORD=... IOTHUB_MQTT_TOKEN=... \
  IOTHUB_TEST_SSID=... python3 tools/review/c152_mqtt_attrs.py --device IotHub-65D8

Exit codes: 0 PASS, 1 FAIL, 2 NOT_COVERED, 3 STOP.
"""

from __future__ import annotations

import argparse
import asyncio
import importlib.util
import json
import os
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
P0 = ROOT / "tools" / "ble_p0" / "p0_acceptance.py"
POLICY = ROOT / "tools" / "review" / "c152_policy.py"
WRONG_PASSWORD = "race-wrong-pass-0"
EXPECTED = {"device_id": "IotHub-65D8", "firmware": "1.3-956afcd",
            "build_sha": "c231c9501"}


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


policy = load_module("c152_policy", POLICY)


class ReadTimeout(Exception):
    pass


class StopRun(Exception):
    pass


class Deps:
    """Injectable dependencies for run_case()."""

    def __init__(self, sess, tap_factory, wall=time.time,
                 mono=time.monotonic, sleep=asyncio.sleep):
        self.sess = sess
        self.tap_factory = tap_factory
        self.wall = wall
        self.mono = mono
        self.sleep = sleep


class AttrTap:
    """Subscribe to the attributes topic only; CONNACK + SUBACK must succeed."""

    def __init__(self, host: str, port: int, username: str | None,
                 timeout: float = 10.0) -> None:
        # Lazy import: the runner module stays importable for host tests
        # without paho installed; only a real MQTT tap needs it.
        import paho.mqtt.client as mqtt

        self.msgs: list[dict] = []
        self.lock = threading.Lock()
        self.connected = threading.Event()
        self.subacked = threading.Event()
        self.rc = None
        self.codes = None
        self.cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                               protocol=mqtt.MQTTv311)
        if username:
            self.cli.username_pw_set(username)
        self.cli.on_message = self._on_message
        self.cli.on_connect = self._on_connect
        self.cli.on_subscribe = self._on_subscribe
        self.cli.connect(host, port, keepalive=30)
        self.cli.loop_start()
        if not self.connected.wait(timeout):
            self.stop()
            raise StopRun("mqtt CONNACK timeout")
        rc, _mid = self.cli.subscribe([("v1/devices/me/attributes", 1)])
        if int(rc) != int(mqtt.MQTT_ERR_SUCCESS):
            self.stop()
            raise StopRun("mqtt subscribe local rc=%s" % rc)
        if not self.subacked.wait(timeout):
            self.stop()
            raise StopRun("mqtt SUBACK timeout")
        ok, detail = policy.suback_ok(self.codes)
        if not ok:
            self.stop()
            raise StopRun("mqtt SUBACK %s: %r" % (detail, self.codes))

    def _on_connect(self, _client, _userdata, _flags, reason_code, _props):
        self.rc = getattr(reason_code, "value", reason_code)
        if int(self.rc) != 0:
            return
        self.connected.set()

    def _on_subscribe(self, _client, _userdata, _mid, reason_codes, _props):
        self.codes = reason_codes
        self.subacked.set()

    def _on_message(self, _client, _userdata, msg):
        try:
            obj = json.loads(msg.payload.decode("utf-8", "replace"))
        except ValueError:
            return
        with self.lock:
            self.msgs.append({"ts": time.time(), "topic": msg.topic, "obj": obj})

    def snapshot(self) -> list[dict]:
        with self.lock:
            return list(self.msgs)

    def stop(self) -> None:
        try:
            self.cli.loop_stop()
            self.cli.disconnect()
        except Exception:  # noqa: BLE001
            pass


def req(rid: str, cmd: str, args: dict, token: str | None) -> dict:
    return {"id": rid, "cmd": cmd, "args": args, "version": "1.0", "token": token}


async def read_cmd(deps: Deps, cmd: str, args: dict, base_rid: str,
                   timeout: float = 15.0) -> dict:
    """Read-only command; unique id per attempt; bounded timeout retries."""
    attempt = 1
    while True:
        rid = "%s-%d" % (base_rid, attempt)
        try:
            resp = await deps.sess.send_request(
                req(rid, cmd, args, deps.sess.token), timeout=timeout, retries=0)
            return resp.obj or {}
        except asyncio.TimeoutError:
            if not policy.should_retry_read(attempt):
                raise ReadTimeout("%s timed out after %d attempts" % (cmd, attempt))
            attempt += 1
            await deps.sleep(policy.READ_RETRY_DELAY_S)


async def identity_gate(deps: Deps) -> tuple[dict, list[str]]:
    dev = (await read_cmd(deps, "device.status", {}, "ident")).get("data") or {}
    return dev, policy.identity_errors(dev, EXPECTED)


async def wait_ready(deps: Deps, stable_s: float) -> bool:
    """Real-time stability gate: monotonic clock, gap resets the streak."""
    start = None
    last_ok = None
    deadline = deps.mono() + 60.0
    while deps.mono() < deadline:
        data = (await read_cmd(deps, "network.status", {}, "ready", 30)).get("data") or {}
        ok = (data.get("trial", {}).get("state") == "idle"
              and data.get("mqtt_connected") is True
              and data.get("sta_connected") is True)
        now = deps.mono()
        if ok:
            if start is None or (last_ok is not None
                                 and now - last_ok > policy.READY_MAX_GAP_S):
                start = now
            last_ok = now
            if now - start >= stable_s:
                return True
        else:
            start = None
            last_ok = None
        await deps.sleep(1.0)
    return False


async def wait_terminal(deps: Deps, op_id: str, timeout_s: float) -> dict:
    deadline = deps.mono() + timeout_s
    last: dict = {}
    while deps.mono() < deadline:
        last = (await read_cmd(deps, "operation.get", {"operation_id": op_id},
                               "term", 10)).get("data") or {}
        if last.get("state") in policy.TERMINAL_STATES:
            return last
        await deps.sleep(0.3)
    return last


async def wait_sta(deps: Deps, timeout_s: float) -> bool:
    deadline = deps.mono() + timeout_s
    while deps.mono() < deadline:
        net = ((await read_cmd(deps, "device.status", {}, "sta", 30)).get("data")
               or {}).get("network") or {}
        if net.get("sta_connected") is True:
            return True
        await deps.sleep(1.5)
    return False


async def run_case(deps: Deps, ssid: str) -> dict:
    """Run C15-2; returns the result dict with verdict/reason."""
    result: dict = {"verdict": "STOP", "reason": "unfinished"}
    tap = None
    try:
        dev, errs = await identity_gate(deps)
        result["identity"] = dev
        if errs:
            result.update(verdict="STOP", reason="identity", errors=errs)
            return result

        if not await wait_ready(deps, policy.READY_STABLE_S):
            result.update(verdict="STOP", reason="readiness")
            return result

        tap = deps.tap_factory()
        w0 = deps.wall()
        try:
            tc = await deps.sess.send_request(
                req("tc1", "network.tryconnect",
                    {"ssid": ssid, "password": WRONG_PASSWORD}, deps.sess.token),
                timeout=20, retries=0)
        except Exception as exc:  # noqa: BLE001
            result.update(verdict="STOP", reason="write_outcome_unknown",
                          detail=type(exc).__name__)
            return result

        obj = tc.obj or {}
        op_id = (obj.get("data") or {}).get("operation_id")
        cls = policy.tryconnect_class(obj.get("ok"), op_id,
                                      (obj.get("error") or {}).get("code"))
        result["tryconnect"] = {"class": cls, "operation_id": op_id}
        if cls == "busy":
            result.update(verdict="NOT_COVERED", reason="tryconnect_busy", w0=w0)
            return result
        if cls != "accepted":
            result.update(verdict="FAIL", reason="tryconnect_rejected", w0=w0)
            return result

        term = await wait_terminal(deps, op_id, policy.TERMINAL_TIMEOUT_S)
        result["terminal"] = {"state": term.get("state"), "error": term.get("error")}
        restored = await wait_sta(deps, policy.RECOVERY_TIMEOUT_S)
        result["sta_restored"] = restored

        dev2, errs2 = await identity_gate(deps)
        result["identity_after"] = dev2
        identity_ok = not errs2

        w1 = deps.wall()
        status = (await read_cmd(deps, "network.status", {}, "post", 30)).get("data") or {}
        status_ts = deps.wall()  # response sample time, after the read returns

        win = policy.window_eval(tap.snapshot(), w0, w1)
        cross = policy.crosscheck(win["last"], win["last_ts"], status, status_ts)
        verdict, reason = policy.decide(
            n_attr=win["n_attr"], violations=win["violations"],
            terminal_state=term.get("state"), terminal_error=term.get("error"),
            sta_restored=restored, identity_ok=identity_ok, cross_errors=cross)
        result.update(verdict=verdict, reason=reason, w0=w0, w1=w1,
                      window=win, cross_errors=cross, status_ts=status_ts,
                      status={"sta_has_ip": status.get("sta_has_ip"),
                              "sta_ip": status.get("sta_ip")})
        return result
    except ReadTimeout as exc:
        result.update(verdict="STOP", reason="read_timeout", detail=str(exc))
        return result
    except StopRun as exc:
        result.update(verdict="STOP", reason="stop", detail=str(exc))
        return result
    except Exception as exc:  # noqa: BLE001
        result.update(verdict="STOP", reason="exception",
                      detail="%s: %s" % (type(exc).__name__, exc))
        return result
    finally:
        if tap is not None:
            tap.stop()


async def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("IOTHUB_MQTT_HOST",
                                                         "139.9.93.245"))
    parser.add_argument("--port", type=int, default=int(os.environ.get(
        "IOTHUB_MQTT_PORT", "21883")))
    parser.add_argument("--ssid", default=os.environ.get("IOTHUB_TEST_SSID"))
    parser.add_argument("--device", default="IotHub-65D8")
    parser.add_argument("--out", default="/tmp/c152_mqtt_attrs.json")
    args = parser.parse_args()

    user = os.environ.get("IOTHUB_BLE_USER")
    password = os.environ.get("IOTHUB_BLE_PASSWORD")
    token = os.environ.get("IOTHUB_MQTT_TOKEN")
    if not user or not password:
        print("REFUSE: IOTHUB_BLE_USER/IOTHUB_BLE_PASSWORD missing")
        return 3
    if not token:
        print("REFUSE: IOTHUB_MQTT_TOKEN missing (21883 requires device token)")
        return 3
    if not args.ssid:
        print("REFUSE: --ssid or IOTHUB_TEST_SSID missing")
        return 3

    p0 = load_module("p0_acceptance", P0)
    result: dict = {"verdict": "STOP", "reason": "unfinished"}
    cli = None
    try:
        recs = await p0.scan_filtered(10)
        target = p0.choose_target(recs, None)
        if not target:
            raise StopRun("device not found in scan")
        cli = p0.BleakClient(target["address"], timeout=25)
        await cli.connect()
        sess = p0.Session(cli, address=target["address"],
                          username=user, password=password)
        await cli.start_notify(p0.CMD_TX, sess.on_notify)
        await asyncio.sleep(0.3)
        await sess.login(user, password, retries=0)

        deps = Deps(sess, lambda: AttrTap(args.host, args.port, token))
        result = await run_case(deps, args.ssid)
        return policy.exit_code(result.get("verdict", "STOP"))
    except StopRun as exc:
        result.update(verdict="STOP", reason="stop", detail=str(exc))
        return 3
    except Exception as exc:  # noqa: BLE001
        result.update(verdict="STOP", reason="exception",
                      detail="%s: %s" % (type(exc).__name__, exc))
        return 3
    finally:
        if cli is not None:
            try:
                await cli.disconnect()
            except Exception:  # noqa: BLE001
                pass
        Path(args.out).write_text(
            json.dumps(result, ensure_ascii=False, indent=1) + "\n",
            encoding="utf-8")
        print(json.dumps({"verdict": result.get("verdict"),
                          "reason": result.get("reason")}, ensure_ascii=False))


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
