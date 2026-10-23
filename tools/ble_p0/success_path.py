#!/usr/bin/env python3
"""T1: successful v1 network transaction.

Flow: tryconnect(accepted) -> operation.get(connecting) -> got_ip ->
network.apply(success) -> duplicate apply -> cancel after success ->
config/status verification -> optional reboot persistence check.

The Wi-Fi password is read from IOTHUB_WIFI_PASSWORD (or --password) and is
never written to the result file. Only use an AP that is visible to the device.

    IOTHUB_WIFI_PASSWORD='<WIFI_PASSWORD>' \
      .venv-bleak/bin/python -u tools/ble_p0/success_path.py --ssid CMCC-502
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import secrets
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from typing import Any

from bleak import BleakClient

sys.path.insert(0, str(Path(__file__).resolve().parent))
from p0_acceptance import (  # noqa: E402
    CMD_TX,
    TERMINAL_STATES,
    Session,
    choose_target,
    clear_cached_iot_targets,
    now,
    response_error_code,
    response_error_text,
    sanitize,
    scan_filtered,
    stamp,
    wait_wifi_ready,
)

RESULTS_DIR = Path(__file__).resolve().parent / "results"


def data_of(obj: Any) -> dict[str, Any]:
    if not isinstance(obj, dict):
        return {}
    data = obj.get("data")
    return data if isinstance(data, dict) else {}


async def connect_once(args: argparse.Namespace, clear_cache: bool = True):
    if clear_cache:
        clear_cached_iot_targets()
    rows = await scan_filtered(8)
    target = choose_target(rows, args.address)
    if target is None:
        raise RuntimeError("IotHub NUS device not found")
    cli = BleakClient(target["address"], timeout=25)
    await cli.connect()
    sess = Session(
        cli,
        address=target["address"],
        username=args.user,
        password=args.user_password,
        max_write=getattr(args, "max_write", 20),
    )
    await cli.start_notify(CMD_TX, sess.on_notify)
    await asyncio.sleep(0.2)
    await sess.login()
    return cli, sess, target


async def close_session(cli: BleakClient, sess: Session) -> None:
    try:
        await sess.close()
    except Exception:  # noqa: BLE001
        pass
    try:
        await cli.disconnect()
    except Exception:  # noqa: BLE001
        pass


async def poll_until_got_ip(sess: Session, op_id: str, timeout: float = 30):
    started = now()
    states: list[dict[str, Any]] = []
    last: dict[str, Any] = {}
    while now() - started < timeout:
        res = await sess.v1(
            "operation.get", {"operation_id": op_id}, timeout=10, id_prefix="opget"
        )
        data = data_of(res.obj)
        last = data
        states.append(
            {
                "t": round(now() - started, 3),
                "state": data.get("state"),
                "error": data.get("error"),
                "trial_id": data.get("trial_id"),
                "ssid": data.get("ssid"),
            }
        )
        sess.record(
            f"[t1] operation.get state={data.get('state')} error={data.get('error')}"
        )
        if data.get("state") == "got_ip" or data.get("state") in TERMINAL_STATES:
            return data, states, now() - started
        await asyncio.sleep(0.5)
    return last, states, now() - started


def reset_board(port: str) -> dict[str, Any]:
    try:
        proc = subprocess.run(
            ["esptool.py", "--port", port, "--baud", "115200", "read_mac"],
            capture_output=True,
            text=True,
            timeout=45,
        )
        tail = "\n".join((proc.stdout + proc.stderr).splitlines()[-8:])
        print(f"[t1] board reset rc={proc.returncode}\n{tail}")
        return {"rc": proc.returncode, "tail": tail}
    except Exception as exc:  # noqa: BLE001
        print(f"[t1] board reset failed: {exc}")
        return {"rc": 1, "error": str(exc)}


async def run(args: argparse.Namespace) -> int:
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    result: dict[str, Any] = {
        "timestamp": stamp(),
        "name": "T1_SUCCESS_TRYCONNECT_GOT_IP_APPLY",
        "ssid": args.ssid,
        "password_redacted": True,
        "caveats": [],
    }
    cli = None
    sess = None

    if not args.wifi_password:
        print("missing Wi-Fi password: set IOTHUB_WIFI_PASSWORD")
        return 2

    try:
        cli, sess, target = await connect_once(args)
        result["board"] = {
            "address": target["address"],
            "name": target.get("name"),
            "adv_local_name": target.get("local_name"),
        }

        info = await sess.v1("device.info", {}, timeout=10, token=False, id_prefix="info")
        result["device_info"] = sanitize(info.obj)

        before_status = await sess.v1("network.status", {}, timeout=15, id_prefix="status-before")
        before = data_of(before_status.obj)
        result["before"] = sanitize(before)
        sess.record(f"[t1] before: {json.dumps(sanitize(before), ensure_ascii=False)}")

        scan_res = await sess.v1("network.scan", {}, timeout=30, id_prefix="scan")
        scan_data = data_of(scan_res.obj)
        aps = scan_data.get("aps") if isinstance(scan_data.get("aps"), list) else []
        ssids = [ap.get("ssid") for ap in aps if isinstance(ap, dict)]
        result["scan"] = {
            "ok": scan_res.obj.get("ok") is True,
            "target_visible": args.ssid in ssids,
            "count": scan_data.get("count"),
            "ssids": ssids,
            "bytes": scan_res.raw_len,
            "frags": len(scan_res.frag_sizes),
        }
        sess.record(
            f"[t1] scan: target_visible={result['scan']['target_visible']} ssids={ssids}"
        )

        await asyncio.sleep(0.25)
        key = "t1-" + secrets.token_hex(6)
        tc = await sess.v1(
            "network.tryconnect",
            {"ssid": args.ssid, "password": args.wifi_password, "idempotency_key": key},
            timeout=15,
            id_prefix="tryconnect",
        )
        tc_data = data_of(tc.obj)
        op_id = tc_data.get("operation_id")
        result["tryconnect"] = {
            "ok": tc.obj.get("ok") is True,
            "state": tc_data.get("state"),
            "operation_id": op_id,
            "error": response_error_text(tc.obj),
            "latency_ms": round((tc.latency or 0) * 1000, 1),
        }
        if not op_id:
            result["pass"] = False
            result["reason"] = "tryconnect did not return operation_id"
            return _finish(result)

        terminal, states, elapsed = await poll_until_got_ip(sess, op_id, timeout=30)
        result["operation_states"] = states
        result["terminal"] = {
            "state": terminal.get("state"),
            "error": terminal.get("error"),
            "data": sanitize(terminal),
            "elapsed_s": round(elapsed, 3),
            "observed_connecting": any(s.get("state") == "connecting" for s in states),
        }
        if terminal.get("state") != "got_ip":
            result["pass"] = False
            result["reason"] = f"tryconnect did not reach got_ip: {terminal.get('error')}"
            return _finish(result)

        # The candidate is a DHCP candidate. Query status while it is active.
        cand_status = await sess.v1("network.status", {}, timeout=15, id_prefix="status-cand")
        result["candidate_status"] = sanitize(data_of(cand_status.obj))

        await asyncio.sleep(0.25)
        apply_res = await sess.v1(
            "network.apply", {"operation_id": op_id}, timeout=20, id_prefix="apply"
        )
        apply_data = data_of(apply_res.obj)
        result["apply"] = {
            "ok": apply_res.obj.get("ok") is True,
            "state": apply_data.get("state"),
            "saved": apply_data.get("saved"),
            "applied": apply_data.get("applied"),
            "error": response_error_text(apply_res.obj),
            "data": sanitize(apply_res.obj),
        }
        if apply_res.obj.get("ok") is not True or apply_data.get("state") != "success":
            result["pass"] = False
            result["reason"] = "network.apply did not succeed"
            return _finish(result)

        await asyncio.sleep(0.25)
        apply_dup = await sess.v1(
            "network.apply", {"operation_id": op_id}, timeout=20, id_prefix="apply-dup"
        )
        result["apply_duplicate"] = {
            "ok": apply_dup.obj.get("ok") is True,
            "state": data_of(apply_dup.obj).get("state"),
            "error": response_error_text(apply_dup.obj),
        }

        await asyncio.sleep(0.25)
        cancel = await sess.v1(
            "network.cancel", {"operation_id": op_id}, timeout=15, id_prefix="cancel-after"
        )
        result["cancel_after_success"] = {
            "ok": cancel.obj.get("ok") is True,
            "error_code": response_error_code(cancel.obj),
            "error": response_error_text(cancel.obj),
            "data": sanitize(cancel.obj.get("data")),
        }

        # Wait for the asynchronous Wi-Fi restart after apply.
        await asyncio.sleep(4)
        ready = await wait_wifi_ready(sess, timeout=25)
        result["after_apply_ready"] = ready
        after_status = await sess.v1("network.status", {}, timeout=15, id_prefix="status-after")
        after = data_of(after_status.obj)
        result["after_apply"] = sanitize(after)

        cfg = await sess.v1("config.get", {}, timeout=20, id_prefix="cfg-after")
        cfg_data = data_of(cfg.obj)
        network = cfg_data.get("network") if isinstance(cfg_data.get("network"), dict) else {}
        result["after_config_network"] = sanitize(network)
        result["after_mqtt"] = sanitize(cfg_data.get("mqtt"))

        # Give MQTT a short window to recover after the Wi-Fi restart.
        if not (isinstance(cfg_data.get("mqtt"), dict) and cfg_data["mqtt"].get("connected")):
            await asyncio.sleep(5)
            cfg2 = await sess.v1("config.get", {}, timeout=20, id_prefix="cfg-after2")
            cfg2_data = data_of(cfg2.obj)
            result["after_mqtt_retry"] = sanitize(cfg2_data.get("mqtt"))
            result["after_config_network_retry"] = sanitize(
                cfg2_data.get("network") if isinstance(cfg2_data.get("network"), dict) else {}
            )

        # Reboot persistence check.
        reboot_result: dict[str, Any] = {}
        if args.reboot_verify:
            await close_session(cli, sess)
            cli = None
            sess = None
            reboot_result["reset"] = reset_board(args.serial_port)
            await asyncio.sleep(9)
            cli, sess, target2 = await connect_once(args, clear_cache=True)
            reboot_result["ready"] = await wait_wifi_ready(sess, timeout=25)
            boot_status = await sess.v1(
                "network.status", {}, timeout=20, id_prefix="status-boot"
            )
            boot_data = data_of(boot_status.obj)
            boot_cfg = await sess.v1("config.get", {}, timeout=20, id_prefix="cfg-boot")
            boot_cfg_data = data_of(boot_cfg.obj)
            boot_net = (
                boot_cfg_data.get("network")
                if isinstance(boot_cfg_data.get("network"), dict)
                else {}
            )
            reboot_result.update(
                {
                    "board": {"address": target2["address"], "name": target2.get("name")},
                    "status": sanitize(boot_data),
                    "config_network": sanitize(boot_net),
                    "mqtt": sanitize(boot_cfg_data.get("mqtt")),
                }
            )
            result["reboot_verify"] = reboot_result

        after_net = result.get("after_config_network", {})
        boot_net = (
            result.get("reboot_verify", {}).get("config_network", {})
            if args.reboot_verify
            else after_net
        )
        result["pass"] = bool(
            result["tryconnect"]["ok"]
            and result["terminal"]["state"] == "got_ip"
            and result["terminal"].get("observed_connecting")
            and result["apply"]["ok"]
            and result["apply"].get("state") == "success"
            and result["apply_duplicate"]["ok"]
            and result["cancel_after_success"]["error_code"] == "operation_state"
            and after_net.get("sta_ssid") == args.ssid
            and after_net.get("sta_use_static") is False
            and after_net.get("sta_has_ip") is True
            and str(after_net.get("sta_ip") or "") not in ("", "0.0.0.0")
            and (not args.reboot_verify or boot_net.get("sta_ssid") == args.ssid)
            and (not args.reboot_verify or boot_net.get("sta_use_static") is False)
            and (
                not args.reboot_verify
                or result["reboot_verify"].get("status", {}).get("sta_has_ip") is True
            )
            and (
                not args.reboot_verify
                or str(result["reboot_verify"].get("status", {}).get("sta_ip") or "")
                not in ("", "0.0.0.0")
            )
        )
        before_ssid = before.get("sta_ssid")
        cross_ssid = before_ssid != args.ssid
        result["notes"] = [
            f"candidate SSID={args.ssid}; before SSID={before_ssid}; "
            f"cross_ssid={cross_ssid}; target_visible={result['scan']['target_visible']}",
            "apply intentionally commits sta_use_static=false (DHCP); the device IP may change from a static address.",
            "apply success is not MQTT connected; the script retries the MQTT check and verifies after reboot.",
        ]
        if not result["scan"]["target_visible"]:
            result["notes"].append(
                "target SSID was not visible in the device scan; tryconnect may fail with ap_not_found."
            )
        return _finish(result)
    finally:
        if cli is not None and sess is not None:
            await close_session(cli, sess)


def _finish(result: dict[str, Any]) -> int:
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    path = RESULTS_DIR / f"success_path_{ts}.json"
    result["_path"] = str(path)
    path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print("\n== T1 SUMMARY ==")
    print(json.dumps(sanitize({k: result.get(k) for k in (
        "ssid", "tryconnect", "terminal", "apply", "apply_duplicate",
        "cancel_after_success", "after_apply", "after_config_network",
        "after_mqtt", "reboot_verify", "pass", "reason",
    )}), ensure_ascii=False, indent=2))
    print(f"[results] {path}")
    return 0 if result.get("pass") else 1


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="T1 successful network transaction")
    parser.add_argument("--ssid", required=True, help="visible Wi-Fi SSID")
    parser.add_argument(
        "--wifi-password",
        default=os.environ.get("IOTHUB_WIFI_PASSWORD"),
        help="Wi-Fi password; prefer env IOTHUB_WIFI_PASSWORD (not written to results)",
    )
    parser.add_argument("--address", help="BLE address (default: name/NUS scan)")
    parser.add_argument("--user", default="admin", help="BLE admin username")
    parser.add_argument("--user-password", default="123456", help="BLE admin password")
    parser.add_argument("--serial-port", default="/dev/ttyUSB0", help="ESP32 serial port for reset")
    parser.add_argument(
        "--no-reboot-verify", dest="reboot_verify", action="store_false", default=True,
        help="skip the post-apply reboot persistence check",
    )
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
