#!/usr/bin/env python3
"""BLE rescue chain test.

1. Set a deliberately bad STA SSID via v0 config.set (device reboots).
2. Connect BLE with STA failed / no IP / MQTT down.
3. network.scan -> network.tryconnect(target) -> network.apply.
4. Verify target DHCP + MQTT recovery.

With --observe-wait > 90 the test also checks that the device stays in STA
mode (no AP auto-fallback) while BLE remains available.

    IOTHUB_WIFI_PASSWORD='<TARGET_WIFI_PASSWORD>' \
      .venv-bleak/bin/python -u tools/ble_p0/ble_rescue.py --target-ssid TQ2018
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import secrets
import sys
from datetime import datetime
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from p0_acceptance import (  # noqa: E402
    TERMINAL_STATES,
    sanitize,
    stamp,
    wait_wifi_ready,
)
from success_path import (  # noqa: E402
    RESULTS_DIR,
    close_session,
    connect_once,
    data_of,
)

async def snapshot(sess: Any) -> dict[str, Any]:
    st = await sess.v1("network.status", {}, timeout=15, id_prefix="rescue-st")
    cfg = await sess.v1("config.get", {}, timeout=20, id_prefix="rescue-cfg")
    cd = data_of(cfg.obj)
    net = cd.get("network") if isinstance(cd.get("network"), dict) else {}
    mqtt = cd.get("mqtt") if isinstance(cd.get("mqtt"), dict) else {}
    return {
        "status": sanitize(data_of(st.obj)),
        "network": sanitize(net),
        "mqtt": sanitize(mqtt),
    }


async def poll_got_ip(sess: Any, op_id: str, timeout: float = 30) -> tuple[dict[str, Any], list[dict[str, Any]], float]:
    started = asyncio.get_event_loop().time()
    states: list[dict[str, Any]] = []
    last: dict[str, Any] = {}
    while asyncio.get_event_loop().time() - started < timeout:
        res = await sess.v1(
            "operation.get", {"operation_id": op_id}, timeout=10, id_prefix="rescue-get"
        )
        d = data_of(res.obj)
        states.append({"t": round(asyncio.get_event_loop().time() - started, 3),
                       "state": d.get("state"), "error": d.get("error")})
        last = d
        if d.get("state") == "got_ip" or d.get("state") in TERMINAL_STATES:
            return d, states, asyncio.get_event_loop().time() - started
        await asyncio.sleep(0.5)
    return last, states, asyncio.get_event_loop().time() - started


async def run(args: argparse.Namespace) -> int:
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    if not os.environ.get("IOTHUB_WIFI_PASSWORD"):
        print("missing IOTHUB_WIFI_PASSWORD (target Wi-Fi password)")
        return 2
    result: dict[str, Any] = {
        "timestamp": stamp(),
        "name": "BLE_RESCUE_CHAIN",
        "target_ssid": args.target_ssid,
        "observe_wait_s": args.observe_wait,
    }
    cli = None
    sess = None
    bad_ssid = args.bad_ssid or ("P0-NOAP-" + secrets.token_hex(4).upper())
    bad_password = args.bad_password or ("p0-rescue-" + secrets.token_hex(6))
    result["bad_ssid"] = bad_ssid

    try:
        # 1. Connect while healthy, then commit a deliberately bad STA config.
        cli, sess, target = await connect_once(args)
        before = await snapshot(sess)
        result["before"] = before
        set_res = await sess.v0(
            "config.set",
            {
                "sta_ssid": bad_ssid,
                "sta_password": bad_password,
                "sta_use_static": False,
                "net_mode": "sta",
            },
            req_id=78,
            timeout=20,
        )
        result["bad_config_set"] = {
            "ok": set_res.obj.get("ok") is True,
            "response": sanitize(set_res.obj),
        }
        if set_res.obj.get("ok") is not True:
            result["pass"] = False
            result["reason"] = "bad config.set was rejected"
            return _finish(result)

        await close_session(cli, sess)
        cli = None
        sess = None
        await asyncio.sleep(args.observe_wait)

        # 2. BLE must still work with STA failed/no IP/MQTT down.
        cli, sess, target = await connect_once(args, clear_cache=True)
        failed = await snapshot(sess)
        failed_net = failed["network"]
        failed_status = failed["status"]
        failed_mqtt = failed["mqtt"]
        result["failed_state"] = failed
        result["failed_state_checks"] = {
            "ble_connected": True,
            "ssid_is_bad": failed_net.get("sta_ssid") == bad_ssid,
            "mode_is_sta": failed_net.get("net_mode") == "sta",
            "no_ip": not failed_status.get("sta_has_ip"),
            "mqtt_down": not failed_mqtt.get("connected"),
            "ap_fallback_field": failed_net.get("startup_fallback_to_ap"),
        }

        # 3. network.scan -> tryconnect -> apply.
        scan = await sess.v1("network.scan", {}, timeout=30, id_prefix="rescue-scan")
        scan_data = data_of(scan.obj)
        aps = scan_data.get("aps") if isinstance(scan_data.get("aps"), list) else []
        ssids = [ap.get("ssid") for ap in aps if isinstance(ap, dict)]
        result["scan"] = {"ok": scan.obj.get("ok") is True,
                          "target_visible": args.target_ssid in ssids,
                          "ssids": ssids}
        if args.target_ssid not in ssids:
            result["pass"] = False
            result["reason"] = "target SSID not visible during BLE rescue"
            return _finish(result)

        tc = await sess.v1(
            "network.tryconnect",
            {
                "ssid": args.target_ssid,
                "password": os.environ["IOTHUB_WIFI_PASSWORD"],
                "idempotency_key": "rescue-" + secrets.token_hex(6),
            },
            timeout=15,
            id_prefix="rescue-tc",
        )
        tc_data = data_of(tc.obj)
        op_id = tc_data.get("operation_id")
        result["tryconnect"] = {"ok": tc.obj.get("ok") is True,
                                "operation_id": op_id,
                                "state": tc_data.get("state")}
        if not op_id:
            result["pass"] = False
            result["reason"] = "rescue tryconnect did not return operation_id"
            return _finish(result)

        terminal, states, elapsed = await poll_got_ip(sess, op_id, timeout=30)
        result["operation_states"] = states
        result["terminal"] = {"state": terminal.get("state"),
                              "error": terminal.get("error"),
                              "elapsed_s": round(elapsed, 3)}
        if terminal.get("state") != "got_ip":
            result["pass"] = False
            result["reason"] = f"rescue tryconnect did not reach got_ip: {terminal.get('error')}"
            return _finish(result)

        await asyncio.sleep(0.3)
        apply_res = await sess.v1(
            "network.apply", {"operation_id": op_id}, timeout=20, id_prefix="rescue-apply"
        )
        apply_data = data_of(apply_res.obj)
        result["apply"] = {"ok": apply_res.obj.get("ok") is True,
                           "state": apply_data.get("state"),
                           "saved": apply_data.get("saved"),
                           "applied": apply_data.get("applied"),
                           "error": sanitize(apply_res.obj.get("error"))}
        if apply_res.obj.get("ok") is not True or apply_data.get("state") != "success":
            result["pass"] = False
            result["reason"] = "rescue apply failed"
            return _finish(result)

        await close_session(cli, sess)
        cli = None
        sess = None
        await asyncio.sleep(9)
        cli, sess, target = await connect_once(args, clear_cache=True)
        final_ready = await wait_wifi_ready(sess, timeout=25)
        final = await snapshot(sess)
        result["final_ready"] = final_ready
        result["final"] = final
        final_net = final["network"]
        final_status = final["status"]
        final_mqtt = final["mqtt"]
        result["final_checks"] = {
            "ssid_target": final_net.get("sta_ssid") == args.target_ssid,
            "has_ip": final_status.get("sta_has_ip") is True,
            "ip": final_status.get("sta_ip"),
            "mqtt": final_mqtt.get("connected") is True,
        }
        checks = result["failed_state_checks"]
        result["pass"] = bool(
            checks["ble_connected"]
            and checks["ssid_is_bad"]
            and checks["mode_is_sta"]
            and checks["no_ip"]
            and checks["mqtt_down"]
            and result["scan"]["target_visible"]
            and terminal.get("state") == "got_ip"
            and result["apply"]["ok"]
            and result["final_checks"]["ssid_target"]
            and result["final_checks"]["has_ip"]
            and result["final_checks"]["mqtt"]
        )
        return _finish(result)
    finally:
        if cli is not None and sess is not None:
            await close_session(cli, sess)


def _finish(result: dict[str, Any]) -> int:
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    path = RESULTS_DIR / f"ble_rescue_{ts}.json"
    result["_path"] = str(path)
    path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print("\n== BLE RESCUE SUMMARY ==")
    print(json.dumps(sanitize(result), ensure_ascii=False, indent=2)[:5000])
    print(f"[results] {path}")
    return 0 if result.get("pass") else 1


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="BLE rescue chain")
    parser.add_argument("--target-ssid", default="TQ2018")
    parser.add_argument("--bad-ssid", default=None, help="SSID for the deliberately bad config (default random)")
    parser.add_argument("--bad-password", default=None, help="wrong password for the bad config (default random)")
    parser.add_argument("--observe-wait", type=float, default=8,
                        help="seconds after the bad-config reboot before the BLE rescue")
    parser.add_argument("--address")
    parser.add_argument("--user", default="admin")
    parser.add_argument("--user-password", default="123456")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        return 130
    except Exception as exc:  # noqa: BLE001
        print(f"FATAL: {type(exc).__name__}: {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
