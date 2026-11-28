#!/usr/bin/env python3
"""A0 BLE parameter precheck (test-only, production firmware, read-only).

Connect to 65D8, verify identity via device.info (v1 pre-auth entry, no
token needed), hold 60 s with a device.info heartbeat every 5 s.

No login, no config writes, no tryconnect, no flashing.
Serial capture must already be running (A0_SERIAL, default a0_serial_r4.log).

Environment:
  A0_SERIAL  serial log path   (default /tmp/c15_device_20261127/a0_serial_r4.log)
  A0_OUT     result json path  (default /tmp/c15_device_20261127/a0_hold_r4.json)
"""
import asyncio
import importlib.util
import json
import os
import sys
import time
from pathlib import Path

P0 = Path("/tmp/iothub-c152/tools/ble_p0/p0_acceptance.py")
OUT = Path(os.environ.get("A0_OUT",
                          "/tmp/c15_device_20261127/a0_hold_r4.json"))
SERIAL = Path(os.environ.get("A0_SERIAL",
                             "/tmp/c15_device_20261127/a0_serial_r4.log"))
HOLD_S = 60
HB_S = 5.0
EXPECT_DEVICE_ID = "IotHub-65D8"
EXPECT_FIRMWARE = "1.3-956afcd"


def load_p0():
    spec = importlib.util.spec_from_file_location("p0_acceptance", P0)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["p0_acceptance"] = mod
    spec.loader.exec_module(mod)
    return mod


def record(rec, name, obj):
    rec["events"].append({"ts": time.time(), "name": name, "obj": obj})
    print(name, json.dumps(obj, ensure_ascii=False)[:250], flush=True)


def serial_has(text):
    try:
        return text in SERIAL.read_text(errors="replace")
    except Exception:
        return False


async def wait_serial(text, timeout_s):
    end = time.monotonic() + timeout_s
    while time.monotonic() < end:
        if serial_has(text):
            return True
        await asyncio.sleep(0.5)
    return False


async def main():
    rec = {"ok": False, "mode": "no_auth_device_info_only", "events": []}
    cli = sess = None
    p0 = load_p0()

    async def info(req_id):
        return await sess.send_request(
            {"id": req_id, "version": "1.0", "cmd": "device.info", "args": {}},
            timeout=8, retries=0)

    try:
        recs = await p0.scan_filtered(10)
        target = p0.choose_target(recs, None)
        record(rec, "target", {"addr": target["address"] if target else None})
        if not target:
            return 2
        t0 = time.time()
        cli = p0.BleakClient(target["address"], timeout=25)
        await cli.connect()
        record(rec, "connected", {"after_s": round(time.time() - t0, 2)})
        # Session 构造器要求非空凭据；本轮不登录、不发送任何需鉴权命令，
        # 占位符仅用于满足构造器，不是任何真实账号。
        sess = p0.Session(cli, address=target["address"],
                          username="-", password="-")
        await cli.start_notify(p0.CMD_TX, sess.on_notify)
        await asyncio.sleep(0.3)

        # 设备身份核验（v1 device.info 是免 token 探测入口）
        ident = {}
        try:
            resp = await info("ident")
            data = resp.obj.get("data") or {}
            ident = {"ok": resp.obj.get("ok"),
                     "device_id": data.get("device_id"),
                     "firmware": data.get("firmware"),
                     "build_sha": data.get("build_sha")}
        except Exception as exc:
            ident = {"error": type(exc).__name__}
        record(rec, "identity", ident)
        if (ident.get("device_id") != EXPECT_DEVICE_ID or
                ident.get("firmware") != EXPECT_FIRMWARE):
            record(rec, "identity_mismatch", {"expected": {
                "device_id": EXPECT_DEVICE_ID, "firmware": EXPECT_FIRMWARE}})
            return 4

        # 等设备连接参数策略日志出现
        satisfied = await wait_serial("satisfied, skip update request", 8)
        out_of_policy = serial_has("out of policy, requesting update")
        record(rec, "device_policy", {"satisfied": satisfied,
                                      "out_of_policy": out_of_policy})
        if out_of_policy and not satisfied:
            record(rec, "policy_out_of_range", {"abort_before_hold": True})
            return 5

        hold_start = time.time()
        hb_ok = hb_err = 0
        i = 0
        while time.time() - hold_start < HOLD_S:
            i += 1
            try:
                resp = await info("hb%d" % i)
                if resp.obj.get("ok"):
                    hb_ok += 1
                else:
                    hb_err += 1
            except Exception as exc:
                hb_err += 1
                record(rec, "hb_error", {"i": i, "err": type(exc).__name__})
            remaining = HOLD_S - (time.time() - hold_start)
            if remaining > 0:
                await asyncio.sleep(min(HB_S, remaining))
        record(rec, "hold_done", {"hold_s": round(time.time() - hold_start, 1),
                                  "hb_ok": hb_ok, "hb_err": hb_err})

        connected_still = not serial_has("central disconnected")
        record(rec, "link_still_up", {"no_disconnect": connected_still})

        await cli.disconnect()
        record(rec, "disconnected", {})
        rec["ok"] = bool(satisfied and not out_of_policy and
                         hb_err == 0 and connected_still)
        return 0 if rec["ok"] else 1
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
