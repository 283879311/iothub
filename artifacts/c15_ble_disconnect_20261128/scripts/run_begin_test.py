#!/usr/bin/env python3
"""C15-2 begin-branch device probe (test-only).

One first tryconnect, target expire pause, one second tryconnect expected busy.
No config write/apply. The caller supplies WIFI_SSID/WIFI_PASSWORD and BLE env.
"""
import asyncio
import importlib.util
import json
import os
import sys
import time
from pathlib import Path

P0 = Path("/tmp/iothub-c152/tools/ble_p0/p0_acceptance.py")
OUT = Path("/tmp/c15_device_20261127/run_begin.json")
SERIAL = Path("/tmp/c15_device_20261127/serial_test.log")
EXPECT_FW = "1.3-65f6dbb"
EXPECT_SHA = "981fd6552"
TERMINAL = {"success", "failed", "cancelled", "expired"}


def load_p0():
    spec = importlib.util.spec_from_file_location("p0_acceptance", P0)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["p0_acceptance"] = mod
    spec.loader.exec_module(mod)
    return mod


async def call(sess, cmd, args, rid, timeout=15):
    req = {"id": rid, "version": "1.0", "cmd": cmd,
           "args": args, "token": sess.token}
    resp = await sess.send_request(req, timeout=timeout, retries=0)
    return resp.obj


def serial_has(text):
    try:
        return text in SERIAL.read_text(errors="replace")
    except Exception:
        return False


def record(rec, name, obj):
    rec["events"].append({"ts": time.time(), "name": name, "obj": obj})
    print(name, json.dumps(obj, ensure_ascii=False)[:300], flush=True)


async def wait_serial(text, timeout_s):
    end = time.monotonic() + timeout_s
    while time.monotonic() < end:
        if serial_has(text):
            return True
        await asyncio.sleep(0.5)
    return False


async def wait_op(sess, op_id, want, timeout_s):
    end = time.monotonic() + timeout_s
    last = {}
    while time.monotonic() < end:
        try:
            last = (await call(sess, "operation.get", {"operation_id": op_id},
                               "op%d" % int(time.time() * 1000), timeout=10)).get("data", {})
        except Exception:
            await asyncio.sleep(1)
            continue
        if last.get("state") == want:
            return True, last
        if last.get("state") in TERMINAL and last.get("state") != want:
            return False, last
        await asyncio.sleep(1)
    return False, last


async def wait_network(sess, timeout_s):
    end = time.monotonic() + timeout_s
    last = {}
    while time.monotonic() < end:
        try:
            last = (await call(sess, "network.status", {},
                               "ns%d" % int(time.time() * 1000), timeout=12)).get("data", {})
        except Exception:
            await asyncio.sleep(2)
            continue
        if (last.get("sta_connected") is True and last.get("sta_has_ip") is True
                and last.get("mqtt_connected") is True
                and last.get("internet_verified") is True
                and (last.get("trial") or {}).get("state") == "idle"):
            return True, last
        await asyncio.sleep(2)
    return False, last


async def main():
    rec = {"ok": False, "events": []}
    cli = sess = None
    p0 = load_p0()
    try:
        ssid = os.environ["WIFI_SSID"]
        password = os.environ["WIFI_PASSWORD"]
        recs = await p0.scan_filtered(10)
        target = p0.choose_target(recs, None)
        if not target:
            record(rec, "no_target", {})
            return 2
        cli = p0.BleakClient(target["address"], timeout=25)
        await cli.connect()
        sess = p0.Session(cli, address=target["address"],
                          username=os.environ["IOTHUB_BLE_USER"],
                          password=os.environ["IOTHUB_BLE_PASSWORD"])
        await cli.start_notify(p0.CMD_TX, sess.on_notify)
        await asyncio.sleep(0.3)
        await sess.login(os.environ["IOTHUB_BLE_USER"],
                         os.environ["IOTHUB_BLE_PASSWORD"], retries=0)
        await asyncio.sleep(0.3)

        info = (await call(sess, "device.info", {}, "info", timeout=10)).get("data", {})
        record(rec, "device_info", {"device_id": info.get("device_id"),
                                    "firmware": info.get("firmware"),
                                    "build_sha": info.get("build_sha")})
        if info.get("device_id") != "IotHub-65D8" or info.get("firmware") != EXPECT_FW \
                or info.get("build_sha") != EXPECT_SHA:
            record(rec, "device_identity_mismatch", {})
            return 3

        pre_ok, pre = await wait_network(sess, 30)
        record(rec, "pre_network", {"ok": pre_ok, "sta": pre.get("sta_connected"),
                                    "ip": pre.get("sta_has_ip"),
                                    "mqtt": pre.get("mqtt_connected"),
                                    "inet": pre.get("internet_verified"),
                                    "trial": (pre.get("trial") or {}).get("state")})
        if not pre_ok:
            return 4

        first = await call(sess, "network.tryconnect",
                           {"ssid": ssid, "password": password,
                            "idempotency_key": "c15begin-%d" % int(time.time())},
                           "tc1", timeout=20)
        record(rec, "first_tryconnect", {"ok": first.get("ok"),
                                         "error": (first.get("error") or {}).get("code")})
        op_id = ((first.get("data") or {}).get("operation_id"))
        if not first.get("ok") or not op_id:
            return 5

        got, term = await wait_op(sess, op_id, "got_ip", 60)
        record(rec, "got_ip", {"ok": got, "state": term.get("state"),
                               "cleanup": term.get("cleanup_state")})
        if not got:
            return 6

        arm = await call(sess, "diag.testhook",
                         {"action": "arm", "operation_id": op_id,
                          "hold_ms": 310000},
                         "arm", timeout=10)
        record(rec, "arm", {"ok": arm.get("ok"),
                            "data": arm.get("data"),
                            "error": (arm.get("error") or {}).get("code")})
        if not arm.get("ok") or not (arm.get("data") or {}).get("armed"):
            return 7

        pause_ok = await wait_serial("C1 hook: pause expire_post_cleanup enter", 330)
        record(rec, "pause_entered", {"ok": pause_ok})
        if not pause_ok:
            return 8

        second = await call(sess, "network.tryconnect",
                            {"ssid": ssid, "password": password,
                             "idempotency_key": "c15begin2-%d" % int(time.time())},
                            "tc2", timeout=10)
        second_err = (second.get("error") or {}).get("code")
        record(rec, "second_tryconnect", {"ok": second.get("ok"),
                                          "error": second_err,
                                          "operation_id": (second.get("data") or {}).get("operation_id")})
        if second.get("ok"):
            record(rec, "second_accepted_unexpected", {})
            return 9
        if second_err != "busy":
            record(rec, "second_not_busy", {"error": second_err})
            return 10

        done, final = await wait_op(sess, op_id, "expired", 60)
        record(rec, "terminal", {"ok": done, "state": final.get("state"),
                                 "error": final.get("error"),
                                 "cleanup": final.get("cleanup_state")})
        if not done:
            return 11

        post_ok, post = await wait_network(sess, 90)
        record(rec, "post_network", {"ok": post_ok,
                                     "sta": post.get("sta_connected"),
                                     "ip": post.get("sta_has_ip"),
                                     "mqtt": post.get("mqtt_connected"),
                                     "inet": post.get("internet_verified"),
                                     "trial": (post.get("trial") or {}).get("state")})
        rec["ok"] = bool(post_ok and done and second_err == "busy")
        return 0 if rec["ok"] else 12
    finally:
        if sess is not None and getattr(sess, "cli", None) is not None:
            try:
                await sess.cli.disconnect()
            except Exception:
                pass
        rec["finished_at"] = time.time()
        OUT.write_text(json.dumps(rec, ensure_ascii=False, indent=1) + "\n",
                       encoding="utf-8")
        print("RESULT:", "PASS" if rec.get("ok") else "FAIL", flush=True)


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
