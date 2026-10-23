#!/usr/bin/env python3
"""Verify the v0 config.set static-IP command.

The firmware's static fields are network-domain fields, so v1 config.set
rejects them; the supported path is v0 method/params. These fields have
CFG_EFFECT_REBOOT, so the device reboots after the response.

    .venv-bleak/bin/python -u tools/ble_p0/static_ip_check.py --ssid TQ2018

Default static plan is derived from the current DHCP IP:
    192.168.43.63 -> 192.168.43.213 / gw 192.168.43.1 / /24
It checks command acceptance + persistence, runtime static IP, MQTT, and
reverts to DHCP if the static runtime fails.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
from datetime import datetime
from pathlib import Path
from typing import Any

from bleak import BleakClient

sys.path.insert(0, str(Path(__file__).resolve().parent))
from p0_acceptance import (  # noqa: E402
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


async def reconnect_after(args: argparse.Namespace, cli: BleakClient, sess: Any, delay: float = 10):
    await close_session(cli, sess)
    await asyncio.sleep(delay)
    last_exc: Exception | None = None
    for attempt in range(3):
        try:
            return await connect_once(args, clear_cache=True)
        except Exception as exc:  # noqa: BLE001
            last_exc = exc
            print(f"[static] reconnect attempt {attempt + 1}/3 failed: {exc}")
            await asyncio.sleep(5)
    raise last_exc if last_exc is not None else ConnectionError("reconnect failed")


def derive_plan(ip: str, args: argparse.Namespace) -> dict[str, str]:
    parts = ip.split(".")
    if len(parts) != 4 or parts[3] in ("", "0"):
        raise RuntimeError(f"cannot derive static plan from current IP {ip!r}")
    prefix = ".".join(parts[:3])
    static_ip = args.static_ip or f"{prefix}.213"
    if args.static_ip is None and static_ip == ip:
        static_ip = f"{prefix}.212"
    gateway = args.gateway or f"{prefix}.1"
    return {
        "sta_static_ip": static_ip,
        "sta_static_gateway": gateway,
        "sta_static_netmask": args.netmask,
        "sta_static_dns1": args.dns1 or gateway,
        "sta_static_dns2": args.dns2,
    }


async def run(args: argparse.Namespace) -> int:
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    result: dict[str, Any] = {
        "timestamp": stamp(),
        "name": "STATIC_IP_V0_CONFIG_SET",
        "ssid": args.ssid,
    }
    cli = None
    sess = None

    try:
        cli, sess, target = await connect_once(args)
        result["board"] = {"address": target["address"], "name": target.get("name")}

        before_status_res = await sess.v1(
            "network.status", {}, timeout=15, id_prefix="st-before"
        )
        before_status = data_of(before_status_res.obj)
        before_cfg_res = await sess.v1(
            "config.get", {}, timeout=20, id_prefix="cfg-before"
        )
        before_cfg = data_of(before_cfg_res.obj)
        before_net = before_cfg.get("network") if isinstance(before_cfg.get("network"), dict) else {}
        result["before"] = {
            "status": sanitize(before_status),
            "network": sanitize(before_net),
        }
        if before_status.get("sta_ssid") != args.ssid:
            result["pass"] = False
            result["reason"] = (
                f"current SSID {before_status.get('sta_ssid')!r} != requested {args.ssid!r}"
            )
            return _finish(result)
        current_ip = str(before_status.get("sta_ip") or "")
        plan = derive_plan(current_ip, args)
        result["plan"] = plan
        print(f"[static] before IP={current_ip}; plan={plan}")

        params = {"sta_use_static": True, **plan}
        set_res = await sess.v0("config.set", params, req_id=76, timeout=20)
        result["config_set"] = {
            "ok": set_res.obj.get("ok") is True,
            "response": sanitize(set_res.obj),
            "latency_ms": round((set_res.latency or 0) * 1000, 1),
        }
        command_accepted = set_res.obj.get("ok") is True
        if not command_accepted:
            result["pass"] = False
            result["reason"] = "v0 config.set static command was rejected"
            return _finish(result)

        # These fields are CFG_EFFECT_REBOOT; reconnect after the reboot.
        cli, sess, target = await reconnect_after(args, cli, sess, delay=10)
        ready = await wait_wifi_ready(sess, timeout=25)
        after_status_res = await sess.v1(
            "network.status", {}, timeout=15, id_prefix="st-after"
        )
        after_cfg_res = await sess.v1("config.get", {}, timeout=20, id_prefix="cfg-after")
        after_status = data_of(after_status_res.obj)
        after_cfg = data_of(after_cfg_res.obj)
        after_net = after_cfg.get("network") if isinstance(after_cfg.get("network"), dict) else {}
        after_mqtt = after_cfg.get("mqtt") if isinstance(after_cfg.get("mqtt"), dict) else {}
        result["after"] = {
            "ready": ready,
            "status": sanitize(after_status),
            "network": sanitize(after_net),
            "mqtt": sanitize(after_mqtt),
        }

        persisted = bool(
            after_net.get("sta_use_static") is True
            and after_net.get("sta_static_ip") == plan["sta_static_ip"]
            and after_net.get("sta_static_gateway") == plan["sta_static_gateway"]
        )
        runtime_static = bool(
            after_status.get("sta_has_ip") is True
            and after_status.get("sta_ip") == plan["sta_static_ip"]
        )
        result["command_available"] = command_accepted and persisted
        result["runtime_static_ok"] = runtime_static
        result["pass"] = result["command_available"] and runtime_static

        if not result["pass"] and args.revert_on_fail:
            print("[static] static runtime failed; reverting to DHCP")
            await sess.close()
            await asyncio.sleep(1)
            # Reconnect after the failed static config may still be up; try a
            # fresh session so the revert command is independent.
            try:
                cli, sess, target = await connect_once(args, clear_cache=True)
            except Exception as exc:  # noqa: BLE001
                result["revert"] = {"ok": False, "error": str(exc)}
                return _finish(result)
            revert_res = await sess.v0(
                "config.set", {"sta_use_static": False}, req_id=77, timeout=20
            )
            result["revert"] = {
                "ok": revert_res.obj.get("ok") is True,
                "response": sanitize(revert_res.obj),
            }
            cli, sess, target = await reconnect_after(args, cli, sess, delay=10)
            await wait_wifi_ready(sess, timeout=25)
            revert_status_res = await sess.v1(
                "network.status", {}, timeout=15, id_prefix="st-revert"
            )
            revert_cfg_res = await sess.v1(
                "config.get", {}, timeout=20, id_prefix="cfg-revert"
            )
            revert_status = data_of(revert_status_res.obj)
            revert_cfg = data_of(revert_cfg_res.obj)
            revert_net = (
                revert_cfg.get("network") if isinstance(revert_cfg.get("network"), dict) else {}
            )
            result["after_revert"] = {
                "status": sanitize(revert_status),
                "network": sanitize(revert_net),
                "mqtt": sanitize(revert_cfg.get("mqtt")),
            }
        return _finish(result)
    finally:
        if cli is not None and sess is not None:
            await close_session(cli, sess)


def _finish(result: dict[str, Any]) -> int:
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    path = RESULTS_DIR / f"static_ip_{ts}.json"
    result["_path"] = str(path)
    path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print("\n== STATIC IP SUMMARY ==")
    print(
        json.dumps(
            sanitize(
                {
                    k: result.get(k)
                    for k in (
                        "ssid",
                        "before",
                        "plan",
                        "config_set",
                        "after",
                        "command_available",
                        "runtime_static_ok",
                        "revert",
                        "after_revert",
                        "pass",
                        "reason",
                    )
                }
            ),
            ensure_ascii=False,
            indent=2,
        )
    )
    print(f"[results] {path}")
    return 0 if result.get("pass") else 1


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Verify v0 config.set static IP")
    parser.add_argument("--ssid", required=True, help="current SSID (must match)")
    parser.add_argument("--static-ip", help="static IP (default: current /24 + .213)")
    parser.add_argument("--gateway", help="gateway (default: current /24 + .1)")
    parser.add_argument("--netmask", default="255.255.255.0")
    parser.add_argument("--dns1", help="DNS1 (default: gateway)")
    parser.add_argument("--dns2", default="8.8.8.8")
    parser.add_argument("--address", help="BLE address")
    parser.add_argument("--user", default="admin", help="BLE admin username")
    parser.add_argument("--user-password", default="123456", help="BLE admin password")
    parser.add_argument(
        "--no-revert-on-fail", dest="revert_on_fail", action="store_false", default=True,
        help="do not revert to DHCP if runtime static fails",
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
