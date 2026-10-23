#!/usr/bin/env python3
"""AP schema cleanup check: old NVS layout, config.get, old AP patches.

Read-only except two deliberately ignored/rejected config.set attempts:
  - v0 config.set with ap_ssid/ap_password: old clients sent this; AP runtime is
    gone, so the fields must have no effect and config.get must not expose them.
  - v0 config.set with net_mode=ap: must be rejected.

Run after flashing the AP-schema firmware without erasing NVS:
    .venv-bleak/bin/python -u tools/ble_p0/config_schema_check.py \
      --address b0:a7:32:28:c8:e4 --no-reset
"""
from __future__ import annotations

import argparse
import asyncio
import json
import sys
from datetime import datetime
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from p0_acceptance import (  # noqa: E402
    Response,
    response_error_code,
    sanitize,
    stamp,
)
from success_path import (  # noqa: E402
    RESULTS_DIR,
    close_session,
    connect_once,
    data_of,
    reset_board,
)

AP_KEYS = ("ap_ssid", "ap_password_set", "startup_fallback_to_ap")


def network_of(res: Response) -> dict[str, Any]:
    data = data_of(res.obj)
    net = data.get("network")
    return net if isinstance(net, dict) else {}


def formal_view(net: dict[str, Any]) -> dict[str, Any]:
    keys = (
        "net_mode",
        "sta_ssid",
        "sta_password_set",
        "sta_use_static",
        "sta_static_ip",
        "sta_static_gateway",
        "sta_static_netmask",
        "sta_static_dns1",
        "sta_static_dns2",
    )
    return {key: net.get(key) for key in keys}


async def run(args: argparse.Namespace) -> int:
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    result: dict[str, Any] = {
        "timestamp": stamp(),
        "started_at": datetime.now().astimezone().isoformat(timespec="seconds"),
        "name": "AP_SCHEMA_CLEANUP_CHECK",
        "address": args.address,
    }
    if not args.no_reset:
        result["reset"] = reset_board(args.serial_port)
        await asyncio.sleep(9)

    cli = sess = None
    try:
        cli, sess, target = await connect_once(args)
        result["board"] = {"address": target["address"], "name": target.get("name")}
        before = await sess.v1(
            "config.get", {}, timeout=25, id_prefix="ap-schema-get-before"
        )
        before_net = network_of(before)
        result["before"] = {
            "ok": before.obj.get("ok") is True,
            "network": sanitize(before_net),
        }
        result["ap_fields_present_before"] = [k for k in AP_KEYS if k in before_net]

        old_patch = await sess.v0(
            "config.set",
            {
                "ap_ssid": "legacy-ap-should-not-apply",
                "ap_password": "legacy-dummy-password",
            },
            req_id=901,
            timeout=20,
        )
        result["old_ap_patch"] = {
            "ok": old_patch.obj.get("ok") is True,
            "error_code": response_error_code(old_patch.obj),
            "response": sanitize(old_patch.obj),
        }
        after = await sess.v1(
            "config.get", {}, timeout=25, id_prefix="ap-schema-get-after"
        )
        after_net = network_of(after)
        result["after_old_ap_patch"] = {
            "ok": after.obj.get("ok") is True,
            "network": sanitize(after_net),
        }
        result["ap_fields_present_after"] = [k for k in AP_KEYS if k in after_net]
        result["formal_unchanged_by_old_ap_patch"] = formal_view(before_net) == formal_view(after_net)

        ap_mode = await sess.v0(
            "config.set", {"net_mode": "ap"}, req_id=902, timeout=20
        )
        result["ap_mode_patch"] = {
            "ok": ap_mode.obj.get("ok") is True,
            "error_code": response_error_code(ap_mode.obj),
            "response": sanitize(ap_mode.obj),
        }

        v1_net = await sess.v1(
            "config.get", {"domains": ["network"]}, timeout=25, id_prefix="ap-schema-v1"
        )
        v1_net_data = network_of(v1_net)
        result["v1_network"] = {
            "ok": v1_net.obj.get("ok") is True,
            "network": sanitize(v1_net_data),
            "ap_fields": [k for k in AP_KEYS if k in v1_net_data],
        }

        status = await sess.v1(
            "network.status", {}, timeout=15, id_prefix="ap-schema-status"
        )
        result["network_status"] = sanitize(data_of(status.obj))

        checks = {
            "no_ap_ssid_before": "ap_ssid" not in before_net,
            "no_ap_password_set_before": "ap_password_set" not in before_net,
            "no_startup_fallback_before": "startup_fallback_to_ap" not in before_net,
            "sta_ssid_preserved": bool(before_net.get("sta_ssid")),
            "net_mode_sta": before_net.get("net_mode") == "sta",
            "old_ap_patch_no_effect": result["formal_unchanged_by_old_ap_patch"],
            "ap_mode_rejected": ap_mode.obj.get("ok") is False,
            "no_ap_ssid_after": "ap_ssid" not in after_net,
            "no_ap_fields_v1": not result["v1_network"]["ap_fields"],
            "network_sta": result["network_status"].get("mode") == "sta",
        }
        result["checks"] = checks
        result["pass"] = all(checks.values())
        return finish(result)
    except Exception as exc:  # noqa: BLE001
        result["pass"] = False
        result["fatal"] = f"{type(exc).__name__}: {exc}"
        return finish(result)
    finally:
        if cli is not None and sess is not None:
            await close_session(cli, sess)


def finish(result: dict[str, Any]) -> int:
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    path = RESULTS_DIR / f"config_schema_{ts}.json"
    result["finished_at"] = datetime.now().astimezone().isoformat(timespec="seconds")
    result["_path"] = str(path)
    path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print("\n== AP SCHEMA SUMMARY ==")
    print(json.dumps(sanitize(result), ensure_ascii=False, indent=2)[:8000])
    print(f"[results] {path}")
    return 0 if result.get("pass") else 1


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="AP schema cleanup check")
    parser.add_argument("--address")
    parser.add_argument("--user", default="admin")
    parser.add_argument("--user-password", default="123456")
    parser.add_argument("--serial-port", default="/dev/ttyUSB0")
    parser.add_argument("--no-reset", action="store_true")
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
