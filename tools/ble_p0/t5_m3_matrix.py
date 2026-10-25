#!/usr/bin/env python3
"""T5 M3: anomaly-injection matrix (host-side automation of the T5 plan §3).

Scenarios (T5 doc numbering) and what is actually exercised here:
  M3-1 candidate in progress -> BLE drop : reconnect, old token rejected,
       old operation still queryable (BLE-disconnect cancels active candidate)
  M3-2 target AP gone (AUTOMATED SUBSET): nonexistent SSID -> ap_not_found,
       reason traceable. True mid-connect AP loss needs a real AP toggle ->
       recorded as pending_manual.
  M3-3 wrong password (clean state): wrong_password classification, formal
       config + profile count unchanged, NOT misreported as busy/no_slot
  M3-4 client goes silent after accepted: candidate still terminates and is
       queryable later; a new operation is accepted afterwards. Literal
       EXPIRED is not reachable with current APs (all candidates terminate
       <20s by event); noted in the report.
  M3-5 got_ip without apply: trial expires per design (~300s), later apply
       refuses, formal config not committed, trial idle, new op accepted
  M3-6 cancel then repeat cancel: idempotent; cancel after terminal ->
       operation_state; cancel unknown id -> not_found
  M3-7 device reboot (observational): three-tier recovery timing
       (manageable/network/business), config identical, operation table reset

P1-10 caveat honoured: >=8s settle after any candidate teardown before the
next classification-sensitive scenario.

    IOTHUB_WIFI_PASSWORD='<WIFI_PASSWORD>' \
      .venv-bleak/bin/python -u tools/ble_p0/t5_m3_matrix.py --ssid CMCC-502
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import re
import secrets
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bleak import BleakClient  # noqa: E402

from p0_acceptance import (  # noqa: E402
    CMD_TX,
    NUS_SVC,
    TERMINAL_STATES,
    Session,
    choose_target,
    clear_cached_iot_targets,
    response_error_code,
    sanitize,
    scan_filtered,
    stamp,
)
from success_path import RESULTS_DIR, close_session, connect_once, data_of  # noqa: E402

ESPTOOL = "/home/tc/.espressif/python_env/idf6.0_py3.12_env/bin/esptool.py"
SETTLE_S = 8.0


def wall() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


async def status(sess) -> dict[str, Any]:
    res = await sess.v1("network.status", {}, timeout=15, id_prefix="m3-st")
    return sanitize(data_of(res.obj))


async def config_network(sess) -> dict[str, Any]:
    res = await sess.v1("config.get", {"domains": ["network"]}, timeout=25, id_prefix="m3-cg")
    net = data_of(res.obj).get("network") or {}
    formal = {k: net.get(k) for k in (
        "net_mode", "sta_ssid", "sta_password_set", "sta_use_static",
        "sta_static_ip", "sta_static_gateway", "sta_static_netmask",
        "sta_static_dns1", "sta_static_dns2")}
    return sanitize(formal)


async def profiles(sess) -> list[str]:
    res = await sess.v1("network.profile.list", {}, timeout=15, id_prefix="m3-pl")
    items = data_of(res.obj).get("profiles") or []
    return [p.get("ssid", "") for p in items]


async def tryconnect(sess, ssid: str, pw: str, prefix: str) -> str | None:
    key = "m3-" + secrets.token_hex(6)
    res = await sess.v1(
        "network.tryconnect",
        {"ssid": ssid, "password": pw, "idempotency_key": key},
        timeout=20, id_prefix=prefix,
    )
    return data_of(res.obj).get("operation_id")


async def poll_terminal(sess, op: str, timeout: float = 30.0,
                        interval: float = 0.5) -> dict[str, Any]:
    t0 = time.monotonic()
    last: dict[str, Any] = {}
    while time.monotonic() - t0 < timeout:
        res = await sess.v1("operation.get", {"operation_id": op},
                            timeout=10, id_prefix="m3-poll")
        last = data_of(res.obj)
        if last.get("state") in TERMINAL_STATES | {"got_ip"}:
            break
        await asyncio.sleep(interval)
    last["elapsed_s"] = round(time.monotonic() - t0, 3)
    return last


async def reboot_wait_and_measure(args) -> dict[str, Any]:
    """Reset the board, then time three-tier recovery (M3-7)."""
    t_reset = time.monotonic()
    try:
        proc = subprocess.run(
            [ESPTOOL, "--port", args.serial_port, "--baud", "115200", "read_mac"],
            capture_output=True, text=True, timeout=45,
        )
    except subprocess.TimeoutExpired as exc:
        proc = None
        return {"esptool_error": f"timeout: {exc}"}
    timing: dict[str, Any] = {
        "esptool_rc": proc.returncode,
        "esptool_tail": "\n".join((proc.stdout + proc.stderr).splitlines()[-4:]),
    }

    # tier 1: manageable = advertising visible again
    deadline = t_reset + 60
    adv_seen = None
    while time.monotonic() < deadline:
        hits = await scan_filtered(3)
        if any(("IotHub" in (h.get("name") or "")) or ("IH-" in (h.get("name") or "")) for h in hits):
            adv_seen = time.monotonic() - t_reset
            break
    timing["manageable_adv_s"] = round(adv_seen, 2) if adv_seen else None

    cli = sess = None
    login_s = None
    try:
        target = None
        clear_cached_iot_targets()
        rows = await scan_filtered(6)
        target = choose_target(rows, args.address)
        if target is None:
            timing["error"] = "device not found after reboot"
            return timing
        t_conn = time.monotonic()
        cli = BleakClient(target["address"], timeout=25)
        await cli.connect()
        sess = Session(cli, address=target["address"],
                       username=args.user, password=args.user_password,
                       max_write=20)
        await cli.start_notify(CMD_TX, sess.on_notify)
        await asyncio.sleep(0.2)
        await sess.login()
        login_s = time.monotonic() - t_conn
        timing["manageable_login_s"] = round(login_s, 2)

        # tier 2/3: network + business recovery (every poll recorded)
        ip_s = mqtt_s = None
        poll_log: list[dict[str, Any]] = []
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline and (ip_s is None or mqtt_s is None):
            res = await sess.v1("network.status", {}, timeout=15,
                                id_prefix="m3-rb-st")
            st = data_of(res.obj)
            poll_log.append({
                "t": round(time.monotonic() - t_reset, 2),
                "sta_has_ip": st.get("sta_has_ip"),
                "mqtt_connected": st.get("mqtt_connected"),
            })
            if ip_s is None and st.get("sta_has_ip"):
                ip_s = time.monotonic() - t_reset
            if mqtt_s is None and st.get("mqtt_connected"):
                mqtt_s = time.monotonic() - t_reset
            if ip_s is None or mqtt_s is None:
                await asyncio.sleep(2.0)
        timing["poll_log"] = poll_log
        timing["network_ip_s"] = round(ip_s, 2) if ip_s else None
        timing["business_mqtt_s"] = round(mqtt_s, 2) if mqtt_s else None
        timing["_sess"] = sess
        timing["_cli"] = cli
        return timing
    except Exception as exc:  # noqa: BLE001
        timing["error"] = f"{type(exc).__name__}: {exc}"
        if cli is not None:
            try:
                await cli.disconnect()
            except Exception:  # noqa: BLE001
                pass
        return timing


async def run(args: argparse.Namespace) -> int:
    real_pw = os.environ.get("IOTHUB_WIFI_PASSWORD", "")
    if not real_pw:
        print("missing IOTHUB_WIFI_PASSWORD")
        return 2
    wrong_pw = "m3-wrong-" + secrets.token_hex(5)

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    result: dict[str, Any] = {
        "timestamp": stamp(), "started_at": wall(),
        "name": "T5_M3_ANOMALY_MATRIX", "ssid": args.ssid,
    }
    cli = sess = None
    try:
        if args.only == "m3-7":
            result["only"] = "m3-7"
            cli, sess, _target = await connect_once(args)
            cfg_pre = await config_network(sess)
            op_pre = await tryconnect(sess, args.ssid, wrong_pw, "m3x7")
            await poll_terminal(sess, op_pre, timeout=30)
            await close_session(cli, sess)
            cli = sess = None
            timing = await reboot_wait_and_measure(args)
            sess = timing.pop("_sess", None)
            cli = timing.pop("_cli", None)
            if sess is not None:
                cfg_post = await config_network(sess)
                old_op = data_of((await sess.v1(
                    "operation.get", {"operation_id": op_pre},
                    timeout=15, id_prefix="m3x7-old")).obj)
                timing["config_identical"] = cfg_post == cfg_pre
                timing["old_op_query"] = old_op
                timing["op_table_reset"] = not old_op.get("state")
            result["M3_7_reboot"] = timing
            checks = {
                "m3_7_config_identical": timing.get("config_identical") is True,
                "m3_7_manageable": timing.get("manageable_login_s") is not None,
                "m3_7_network": timing.get("network_ip_s") is not None,
                "m3_7_business": timing.get("business_mqtt_s") is not None,
                "m3_7_op_table_reset": timing.get("op_table_reset") is True,
            }
            result["checks"] = checks
            result["pass"] = all(checks.values())
            return finish(result)

        cli, sess, target = await connect_once(args)
        result["board"] = {"address": target["address"], "name": target.get("name")}
        cfg_before = await config_network(sess)
        prof_before = await profiles(sess)
        result["baseline"] = {"config": cfg_before, "profiles": prof_before,
                              "status": await status(sess)}

        # ---------- M3-6: cancel semantics ----------
        m3 = {}
        op = await tryconnect(sess, args.ssid, wrong_pw, "m3x6")
        await asyncio.sleep(3.0)  # inside connecting window
        c1 = await sess.v1("network.cancel", {"operation_id": op},
                           timeout=15, id_prefix="m3x6-c1")
        c2 = await sess.v1("network.cancel", {"operation_id": op},
                           timeout=15, id_prefix="m3x6-c2")
        c3 = await sess.v1("network.cancel", {"operation_id": "op-00000000DEADBEEF"},
                           timeout=15, id_prefix="m3x6-c3")
        m3["M3_6"] = {
            "cancel_during_connecting": sanitize(data_of(c1.obj)),
            "cancel_repeat": sanitize(data_of(c2.obj)),
            "cancel_repeat_idempotent": (
                c2.obj.get("ok") is True
                and data_of(c2.obj).get("state") == "cancelled"
            ),
            "cancel_unknown": response_error_code(c3.obj),
            "unknown_expect_not_found": response_error_code(c3.obj) == "not_found",
        }
        await asyncio.sleep(SETTLE_S)

        # ---------- M3-3: wrong password (clean state) ----------
        st_before = await status(sess)
        op = await tryconnect(sess, args.ssid, wrong_pw, "m3x3")
        term = await poll_terminal(sess, op, timeout=30)
        cfg_after = await config_network(sess)
        prof_after = await profiles(sess)
        m3["M3_3"] = {
            "terminal": term,
            "classified_wrong_password": term.get("error") == "wrong_password",
            "not_busy_or_no_slot": term.get("error") not in ("busy", "no_slot"),
            "formal_config_unchanged": cfg_after == cfg_before,
            "profiles_unchanged": prof_after == prof_before,
            "runtime_recovered": (await status(sess)).get("sta_connected") is True,
            "_st_before": st_before,
        }
        await asyncio.sleep(SETTLE_S)

        # ---------- M3-1: BLE drop mid-candidate ----------
        op = await tryconnect(sess, args.ssid, wrong_pw, "m3x1")
        old_token = sess.token
        await asyncio.sleep(3.0)  # inside connecting
        await cli.disconnect()          # injection: central goes away
        await asyncio.sleep(8.0)        # let device process disconnect+teardown
        cli2, sess2, _ = await connect_once(args)
        # old token must be rejected on the new session
        rej = await sess2.send_request(
            {"id": "m3x1-oldtok", "version": "1.0", "cmd": "network.status",
             "args": {}, "token": old_token},
            timeout=15, allow_relogin=False, allow_reconnect=False,
        )
        old_op = await sess2.v1("operation.get", {"operation_id": op},
                                timeout=15, id_prefix="m3x1-oldop")
        m3["M3_1"] = {
            "old_token_rejected": response_error_code(rej.obj) == "unauthorized",
            "old_token_error": response_error_code(rej.obj),
            "relogin_ok": sess2.token is not None,
            "old_op_queryable": old_op.obj.get("ok") is True,
            "old_op_state": data_of(old_op.obj).get("state"),
            "old_op_cancelled_by_ble_drop": data_of(old_op.obj).get("state") == "cancelled",
        }
        await close_session(cli, sess)
        cli, sess = cli2, sess2
        await asyncio.sleep(SETTLE_S)

        # ---------- M3-4: client goes silent after accepted ----------
        op = await tryconnect(sess, args.ssid, wrong_pw, "m3x4")
        await asyncio.sleep(25.0)  # no polling at all
        late = data_of((await sess.v1("operation.get", {"operation_id": op},
                                      timeout=15, id_prefix="m3x4-late")).obj)
        new_op = await tryconnect(sess, args.ssid, wrong_pw, "m3x4b")
        m3["M3_4"] = {
            "late_query_state": late.get("state"),
            "late_query_error": late.get("error"),
            "terminal_without_polling": late.get("state") in TERMINAL_STATES,
            "new_op_accepted": isinstance(new_op, str),
            "expired_note": "literal EXPIRED unreachable: all candidates terminate "
                            "<20s by event with current APs (accepted TTL 20s caps all)",
        }
        if new_op:
            await poll_terminal(sess, new_op, timeout=30)
        await asyncio.sleep(SETTLE_S)

        # ---------- M3-5: got_ip then no apply; trial 300s ----------
        op = await tryconnect(sess, args.ssid, real_pw, "m3x5")
        term = await poll_terminal(sess, op, timeout=35)
        trial_at_got = (await status(sess)).get("trial")
        cfg_at_got = await config_network(sess)
        # wait for the 300s trial/got_ip TTL to lapse
        samples = []
        for _ in range(11):  # ~330s
            await asyncio.sleep(30.0)
            cur = data_of((await sess.v1("operation.get", {"operation_id": op},
                                         timeout=15, id_prefix="m3x5-w")).obj)
            samples.append({"t": wall(), "state": cur.get("state"),
                            "error": cur.get("error")})
            if cur.get("state") in TERMINAL_STATES:
                break
        apply_late = await sess.v1("network.apply", {"operation_id": op},
                                   timeout=15, id_prefix="m3x5-apply")
        st_after = await status(sess)
        cfg_after5 = await config_network(sess)
        m3["M3_5"] = {
            "got_ip": term.get("state") == "got_ip",
            "trial_at_got": trial_at_got,
            "expiry_samples": samples,
            "op_expired": samples and samples[-1]["state"] == "expired",
            "late_apply_error": response_error_code(apply_late.obj),
            "late_apply_refused": apply_late.obj.get("ok") is False,
            "formal_config_not_committed": cfg_after5 == cfg_at_got == cfg_before,
            "trial_idle_after": (st_after.get("trial") or {}).get("state") == "idle",
        }
        await asyncio.sleep(SETTLE_S)

        # ---------- M3-2 (automated subset): AP absent ----------
        op = await tryconnect(sess, f"M3-NOAP-{secrets.token_hex(4)}", "dummy-pw", "m3x2")
        term = await poll_terminal(sess, op, timeout=30)
        m3["M3_2_subset"] = {
            "terminal": term,
            "ap_not_found": term.get("error") == "ap_not_found",
            "reason_traceable": term.get("error") is not None,
            "pending_manual": "mid-connect AP loss needs a real AP toggle; "
                              "run with a controllable hotspot and record "
                              "operation.get + last_disconnect",
        }
        await asyncio.sleep(SETTLE_S)

        result["scenarios"] = m3

        # ---------- M3-7: reboot (observational) ----------
        cfg_pre = await config_network(sess)
        op_pre = await tryconnect(sess, args.ssid, wrong_pw, "m3x7")
        await poll_terminal(sess, op_pre, timeout=30)
        await close_session(cli, sess)
        cli = sess = None
        timing = await reboot_wait_and_measure(args)
        sess = timing.pop("_sess", None)
        cli = timing.pop("_cli", None)
        if sess is not None:
            cfg_post = await config_network(sess)
            old_op = data_of((await sess.v1("operation.get", {"operation_id": op_pre},
                                            timeout=15, id_prefix="m3x7-old")).obj)
            timing["config_identical"] = cfg_post == cfg_pre
            timing["op_table_reset"] = response_error_code({"error": {"code": old_op.get("error") if old_op.get("error") else "not_found"}}) if not old_op.get("state") else (old_op.get("state") is None)
            timing["old_op_query"] = old_op
        result["M3_7_reboot"] = timing

        # flatten checks
        checks = {
            "m3_6_cancel_idempotent": m3["M3_6"]["cancel_repeat_idempotent"],
            "m3_6_unknown_not_found": m3["M3_6"]["unknown_expect_not_found"],
            "m3_3_wrong_password": m3["M3_3"]["classified_wrong_password"],
            "m3_3_config_unchanged": m3["M3_3"]["formal_config_unchanged"],
            "m3_3_profiles_unchanged": m3["M3_3"]["profiles_unchanged"],
            "m3_1_old_token_rejected": m3["M3_1"]["old_token_rejected"],
            "m3_1_old_op_cancelled": m3["M3_1"]["old_op_cancelled_by_ble_drop"],
            "m3_4_terminal_without_polling": m3["M3_4"]["terminal_without_polling"],
            "m3_4_new_op_accepted": m3["M3_4"]["new_op_accepted"],
            "m3_5_got_ip": m3["M3_5"]["got_ip"],
            "m3_5_op_expired": m3["M3_5"]["op_expired"],
            "m3_5_late_apply_refused": m3["M3_5"]["late_apply_refused"],
            "m3_5_config_not_committed": m3["M3_5"]["formal_config_not_committed"],
            "m3_5_trial_idle": m3["M3_5"]["trial_idle_after"],
            "m3_2_ap_not_found": m3["M3_2_subset"]["ap_not_found"],
            "m3_7_config_identical": result["M3_7_reboot"].get("config_identical") is True,
            "m3_7_manageable": result["M3_7_reboot"].get("manageable_login_s") is not None,
            "m3_7_network": result["M3_7_reboot"].get("network_ip_s") is not None,
            "m3_7_business": result["M3_7_reboot"].get("business_mqtt_s") is not None,
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
    path = RESULTS_DIR / f"t5_m3_{ts}.json"
    result["finished_at"] = wall()
    result["_path"] = str(path)
    path.write_text(json.dumps(sanitize(result), ensure_ascii=False, indent=2),
                    encoding="utf-8")
    print("\n== T5 M3 SUMMARY ==")
    print(json.dumps(sanitize(result), ensure_ascii=False, indent=2)[:9000])
    print(f"[results] {path}")
    return 0 if result.get("pass") else 1


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="T5 M3 anomaly matrix")
    parser.add_argument("--ssid", required=True)
    parser.add_argument("--address")
    parser.add_argument("--user", default="admin")
    parser.add_argument("--user-password", default="123456")
    parser.add_argument("--serial-port", default="/dev/ttyUSB0")
    parser.add_argument("--only", choices=["m3-7"], default=None,
                        help="run a single scenario (currently m3-7)")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
