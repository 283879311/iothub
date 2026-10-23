#!/usr/bin/env python3
"""P0-4 regression: formal config snapshot/restore consistency.

Requires the device to already be on TQ2018 with static 192.168.43.213
(run static_ip_check.py first). The test does not write the static config itself.

Checks:
  1. baseline TQ2018/static .43.213;
  2. wrong-password candidate -> failed / wrong_password;
  3. at 0/4/12/20s: formal config must stay TQ2018/static .43.213;
     runtime IP/MQTT must recover;
  4. reboot: the same committed config must persist;
  5. success-candidate interleaving: after a failed candidate, a correct
     TQ2018 apply must commit DHCP and must not be overwritten by the old
     delayed cleanup; observe 0/4/12/20s and reboot.
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
    reset_board,
)

TIMES = (0, 4, 12, 20)


async def sample(sess: Any) -> dict[str, Any]:
    st = await sess.v1("network.status", {}, timeout=15, id_prefix="p04-st")
    cfg = await sess.v1("config.get", {}, timeout=20, id_prefix="p04-cfg")
    cfg_data = data_of(cfg.obj)
    net = cfg_data.get("network") if isinstance(cfg_data.get("network"), dict) else {}
    mqtt = cfg_data.get("mqtt") if isinstance(cfg_data.get("mqtt"), dict) else {}
    return {
        "status": sanitize(data_of(st.obj)),
        "network": sanitize(net),
        "mqtt": sanitize(mqtt),
    }


def formal_ok(sample_: dict[str, Any], static: bool) -> bool:
    net = sample_.get("network", {})
    if net.get("sta_ssid") != "TQ2018":
        return False
    if bool(net.get("sta_use_static")) != static:
        return False
    if static:
        return (
            net.get("sta_static_ip") == "192.168.43.213"
            and net.get("sta_static_gateway") == "192.168.43.1"
        )
    return True


def runtime_ok(sample_: dict[str, Any], static: bool) -> bool:
    st = sample_.get("status", {})
    if not st.get("sta_has_ip"):
        return False
    ip = str(st.get("sta_ip") or "")
    if ip in ("", "0.0.0.0"):
        return False
    if static and ip != "192.168.43.213":
        return False
    if not (sample_.get("mqtt") or {}).get("connected"):
        return False
    return True


async def observe(sess: Any, label: str) -> list[dict[str, Any]]:
    started = asyncio.get_event_loop().time()
    samples: list[dict[str, Any]] = []
    for t in TIMES:
        wait = started + t - asyncio.get_event_loop().time()
        if wait > 0:
            await asyncio.sleep(wait)
        s = await sample(sess)
        s["t_s"] = t
        samples.append(s)
        print(
            f"[p0-4 {label} t={t}s] "
            f"ssid={s['network'].get('sta_ssid')} "
            f"static={s['network'].get('sta_use_static')} "
            f"ip={s['status'].get('sta_ip')} "
            f"mqtt={s['mqtt'].get('connected')}"
        )
    return samples


async def wrong_candidate(sess: Any, tag: str) -> dict[str, Any]:
    wrong = "p04-wrong-" + secrets.token_hex(8)
    tc = await sess.v1(
        "network.tryconnect",
        {"ssid": "TQ2018", "password": wrong, "idempotency_key": "p04-" + secrets.token_hex(6)},
        timeout=15,
        id_prefix=f"{tag}-wrong",
    )
    data = data_of(tc.obj)
    op = data.get("operation_id")
    out: dict[str, Any] = {"tryconnect": sanitize(tc.obj), "operation_id": op}
    if not op:
        return out
    started = asyncio.get_event_loop().time()
    terminal: dict[str, Any] = {}
    states: list[dict[str, Any]] = []
    while asyncio.get_event_loop().time() - started < 30:
        res = await sess.v1(
            "operation.get", {"operation_id": op}, timeout=10, id_prefix=f"{tag}-get"
        )
        d = data_of(res.obj)
        states.append({"t": round(asyncio.get_event_loop().time() - started, 3),
                       "state": d.get("state"), "error": d.get("error")})
        terminal = d
        if d.get("state") in (TERMINAL_STATES | {"got_ip"}):
            break
        await asyncio.sleep(0.5)
    out["states"] = states
    out["terminal"] = sanitize(terminal)
    out["elapsed_s"] = round(asyncio.get_event_loop().time() - started, 3)
    if op:
        await asyncio.sleep(0.3)
        apply_res = await sess.v1(
            "network.apply", {"operation_id": op}, timeout=10, id_prefix=f"{tag}-apply"
        )
        cancel_res = await sess.v1(
            "network.cancel", {"operation_id": op}, timeout=10, id_prefix=f"{tag}-cancel"
        )
        out["apply_after"] = sanitize(apply_res.obj)
        out["cancel_after"] = sanitize(cancel_res.obj)
    return out


async def success_candidate(sess: Any) -> dict[str, Any]:
    tc = await sess.v1(
        "network.tryconnect",
        {
            "ssid": "TQ2018",
            "password": os.environ.get("IOTHUB_WIFI_PASSWORD"),
            "idempotency_key": "p04-ok-" + secrets.token_hex(6),
        },
        timeout=15,
        id_prefix="p04-ok",
    )
    data = data_of(tc.obj)
    op = data.get("operation_id")
    out: dict[str, Any] = {"tryconnect": sanitize(tc.obj), "operation_id": op}
    if not op:
        return out
    started = asyncio.get_event_loop().time()
    terminal: dict[str, Any] = {}
    states: list[dict[str, Any]] = []
    while asyncio.get_event_loop().time() - started < 30:
        res = await sess.v1(
            "operation.get", {"operation_id": op}, timeout=10, id_prefix="p04-ok-get"
        )
        d = data_of(res.obj)
        states.append({"t": round(asyncio.get_event_loop().time() - started, 3),
                       "state": d.get("state"), "error": d.get("error")})
        terminal = d
        if d.get("state") == "got_ip" or d.get("state") in TERMINAL_STATES:
            break
        await asyncio.sleep(0.5)
    out["states"] = states
    out["terminal"] = sanitize(terminal)
    out["elapsed_s"] = round(asyncio.get_event_loop().time() - started, 3)
    if terminal.get("state") == "got_ip":
        await asyncio.sleep(0.3)
        apply_res = await sess.v1(
            "network.apply", {"operation_id": op}, timeout=20, id_prefix="p04-ok-apply"
        )
        out["apply"] = sanitize(apply_res.obj)
    return out


async def run(args: argparse.Namespace) -> int:
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    if not os.environ.get("IOTHUB_WIFI_PASSWORD"):
        print("missing IOTHUB_WIFI_PASSWORD (needed for the interleaving success candidate)")
        return 2
    result: dict[str, Any] = {
        "timestamp": stamp(),
        "name": "P0_4_REGRESSION",
        "times_s": list(TIMES),
    }
    cli = None
    sess = None
    try:
        cli, sess, target = await connect_once(args)
        base = await sample(sess)
        result["baseline"] = base
        result["baseline_formal_ok"] = formal_ok(base, static=True)
        if not result["baseline_formal_ok"]:
            result["pass"] = False
            result["reason"] = "baseline is not TQ2018/static .43.213; run static_ip_check.py first"
            return _finish(result)

        failed = await wrong_candidate(sess, "failed")
        result["failed_candidate"] = failed
        failed_observations = await observe(sess, "after-fail")
        result["after_fail"] = failed_observations
        fail_formal = all(formal_ok(s, static=True) for s in failed_observations)
        fail_runtime = runtime_ok(failed_observations[-1], static=True)
        result["after_fail_formal_ok"] = fail_formal
        result["after_fail_runtime_ok"] = fail_runtime

        # Reboot persistence of the formal config.
        await close_session(cli, sess)
        cli = None
        sess = None
        reboot = {"reset": reset_board(args.serial_port)}
        await asyncio.sleep(9)
        cli, sess, target = await connect_once(args, clear_cache=True)
        reboot["ready"] = await wait_wifi_ready(sess, timeout=25)
        reboot["sample"] = await sample(sess)
        reboot["formal_ok"] = formal_ok(reboot["sample"], static=True)
        reboot["runtime_ok"] = runtime_ok(reboot["sample"], static=True)
        result["reboot_after_fail"] = reboot

        # Interleaving: another failed candidate, then a successful apply.
        failed2 = await wrong_candidate(sess, "interleave")
        result["interleave_failed_candidate"] = failed2
        success = await success_candidate(sess)
        result["interleave_success_candidate"] = success
        if success.get("terminal", {}).get("state") == "got_ip":
            await asyncio.sleep(4)
            await wait_wifi_ready(sess, timeout=25)
            ok_obs = await observe(sess, "after-success-apply")
            result["after_success_apply"] = ok_obs
            ok_formal = all(formal_ok(s, static=False) for s in ok_obs)
            ok_runtime = runtime_ok(ok_obs[-1], static=False)
            result["after_success_formal_ok"] = ok_formal
            result["after_success_runtime_ok"] = ok_runtime
            await close_session(cli, sess)
            cli = None
            sess = None
            reboot2 = {"reset": reset_board(args.serial_port)}
            await asyncio.sleep(9)
            cli, sess, target = await connect_once(args, clear_cache=True)
            reboot2["ready"] = await wait_wifi_ready(sess, timeout=25)
            reboot2["sample"] = await sample(sess)
            reboot2["formal_ok"] = formal_ok(reboot2["sample"], static=False)
            reboot2["runtime_ok"] = runtime_ok(reboot2["sample"], static=False)
            result["reboot_after_success"] = reboot2
            result["pass"] = bool(
                failed.get("terminal", {}).get("state") == "failed"
                and failed.get("terminal", {}).get("error") == "wrong_password"
                and fail_formal
                and fail_runtime
                and reboot["formal_ok"]
                and reboot["runtime_ok"]
                and success.get("terminal", {}).get("state") == "got_ip"
                and success.get("apply", {}).get("ok") is True
                and ok_formal
                and ok_runtime
                and reboot2["formal_ok"]
                and reboot2["runtime_ok"]
            )
        else:
            result["pass"] = False
            result["reason"] = "success candidate did not reach got_ip"
        return _finish(result)
    finally:
        if cli is not None and sess is not None:
            await close_session(cli, sess)


def _finish(result: dict[str, Any]) -> int:
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    path = RESULTS_DIR / f"p0_4_regression_{ts}.json"
    result["_path"] = str(path)
    path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print("\n== P0-4 SUMMARY ==")
    print(json.dumps(sanitize(result), ensure_ascii=False, indent=2)[:4000])
    print(f"[results] {path}")
    return 0 if result.get("pass") else 1


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="P0-4 formal-config regression")
    parser.add_argument("--address")
    parser.add_argument("--user", default="admin")
    parser.add_argument("--user-password", default="123456")
    parser.add_argument("--serial-port", default="/dev/ttyUSB0")
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
