#!/usr/bin/env python3
"""T5 C-round anomaly reproduction (host side, no mini-program needed).

T5 run1 archive: every C round (tryconnect+apply to the real AP) failed with
connect_failed within 0.9-3.6s of acceptance, each immediately following a B3
(ap_not_found) round, while B1 (wrong password, same AP) completed the full
16-20s handshake. The tester lost operation ids and did not record whether a
password was sent, so the firmware could not be cross-checked.

This script replays the suspect sequences against the device and records what
the v2 tester would have captured: operation.get terminal state/elapsed plus
network.status.last_disconnect (which carries the Wi-Fi reason code).

Sequences:
  S0 baseline status
  S1 B3-sim: tryconnect to a non-existent SSID        -> expect ap_not_found
  S2 C-sim (correct password, ~1s after S1): tryconnect to the real AP
     -> repro test: does the B3->C sequence itself produce connect_failed?
  S3 B3-sim + C-sim (wrong password)                   -> expect wrong_password
     (event-driven ~13-15s, i.e. the B1 behaviour)
  S4 C-sim (EMPTY password)                            -> tests the missing-
     password hypothesis: threshold drops to OPEN against a WPA2 AP
  S5 final status (link must be restored by auto-reconnect)

    IOTHUB_WIFI_PASSWORD='<WIFI_PASSWORD>' \
      .venv-bleak/bin/python -u tools/ble_p0/t5c_anomaly_repro.py --ssid CMCC-502
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import secrets
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from p0_acceptance import TERMINAL_STATES, response_error_code, sanitize, stamp  # noqa: E402
from success_path import RESULTS_DIR, close_session, connect_once, data_of  # noqa: E402


def wall() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


async def status(sess) -> dict[str, Any]:
    res = await sess.v1("network.status", {}, timeout=15, id_prefix="t5c-st")
    return sanitize(data_of(res.obj))


async def tryconnect_poll(sess, prefix: str, ssid: str, password: str,
                          timeout: float = 30.0) -> dict[str, Any]:
    key = "t5c-" + secrets.token_hex(6)
    t0 = time.monotonic()
    tc = await sess.v1(
        "network.tryconnect",
        {"ssid": ssid, "password": password, "idempotency_key": key},
        timeout=20, id_prefix=prefix,
    )
    tc_data = data_of(tc.obj)
    op = tc_data.get("operation_id")
    accepted_lat = time.monotonic() - t0
    row: dict[str, Any] = {
        "ssid": ssid if ssid != os.environ.get("TARGET_SSID", ssid) else ssid,
        "password_kind": ("empty" if password == "" else
                          "correct" if password == os.environ.get("IOTHUB_WIFI_PASSWORD")
                          else "wrong"),
        "accepted": tc.obj.get("ok") is True,
        "operation_id": op,
        "accept_latency_s": round(accepted_lat, 3),
        "error_code": response_error_code(tc.obj),
    }
    if not op:
        return row
    last: dict[str, Any] = {}
    while time.monotonic() - t0 < timeout:
        res = await sess.v1(
            "operation.get", {"operation_id": op}, timeout=10, id_prefix=f"{prefix}-p"
        )
        last = data_of(res.obj)
        if last.get("state") in TERMINAL_STATES | {"got_ip"}:
            break
        await asyncio.sleep(0.5)
    row["terminal_state"] = last.get("state")
    row["terminal_error"] = last.get("error")
    row["elapsed_s"] = round(time.monotonic() - t0, 3)
    row["matching_t5c"] = (
        last.get("state") == "failed"
        and last.get("error") == "connect_failed"
        and row["elapsed_s"] <= 4.0
    )
    return row


async def run(args: argparse.Namespace) -> int:
    real_pw = os.environ.get("IOTHUB_WIFI_PASSWORD", "")
    if not real_pw:
        print("missing IOTHUB_WIFI_PASSWORD")
        return 2
    os.environ.setdefault("TARGET_SSID", args.ssid)

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    result: dict[str, Any] = {
        "timestamp": stamp(),
        "started_at": wall(),
        "name": "T5C_ANOMALY_REPRO",
        "ssid": args.ssid,
    }
    cli = sess = None
    try:
        cli, sess, target = await connect_once(args)
        result["board"] = {"address": target["address"], "name": target.get("name")}
        result["S0_baseline"] = await status(sess)

        # S1: B3 simulation (ap_not_found), link stays up
        result["S1_b3_noap"] = await tryconnect_poll(
            sess, "t5c-s1", f"T5-NOAP-{secrets.token_hex(4)}", "dummy-password"
        )
        await asyncio.sleep(1.0)
        # S2: C simulation with CORRECT password ~1s after B3 (T5 pace)
        result["S2_c_after_b3_correct"] = await tryconnect_poll(
            sess, "t5c-s2", args.ssid, real_pw
        )
        # got_ip holds a 300s trial that makes further tryconnects busy
        # (T5 A rounds cancel at got_ip the same way). Cancel to release.
        s2_op = result["S2_c_after_b3_correct"].get("operation_id")
        if s2_op and result["S2_c_after_b3_correct"].get("terminal_state") == "got_ip":
            await asyncio.sleep(0.3)
            cancel = await sess.v1(
                "network.cancel", {"operation_id": s2_op}, timeout=15, id_prefix="t5c-s2c"
            )
            result["S2_cancel"] = {
                "ok": cancel.obj.get("ok") is True,
                "state": data_of(cancel.obj).get("state"),
            }
        result["S2_status"] = await status(sess)

        # S3: B3 then C with WRONG password -> expect B1-style wrong_password
        result["S3a_b3_noap"] = await tryconnect_poll(
            sess, "t5c-s3a", f"T5-NOAP-{secrets.token_hex(4)}", "dummy-password"
        )
        await asyncio.sleep(1.0)
        result["S3b_c_wrong_pw"] = await tryconnect_poll(
            sess, "t5c-s3b", args.ssid, "wrong-" + secrets.token_hex(6)
        )
        result["S3b_status"] = await status(sess)

        # S4: C with EMPTY password (tester-missing-password hypothesis)
        result["S4_c_empty_pw"] = await tryconnect_poll(
            sess, "t5c-s4", args.ssid, ""
        )
        result["S4_status"] = await status(sess)

        # S5: link restored by auto-reconnect after all candidates
        await asyncio.sleep(12)
        result["S5_final_status"] = await status(sess)

        checks = {
            "S1_ap_not_found": result["S1_b3_noap"].get("terminal_error") == "ap_not_found",
            "S2_correct_pw_got_ip": result["S2_c_after_b3_correct"].get("terminal_state") == "got_ip",
            "S2_not_t5c_failure": not result["S2_c_after_b3_correct"].get("matching_t5c", False),
            "S3b_wrong_password": result["S3b_c_wrong_pw"].get("terminal_error") == "wrong_password",
            "S5_link_restored": result["S5_final_status"].get("sta_connected") is True,
        }
        result["checks"] = checks
        result["pass"] = all(checks.values())
        result["verdict_hint"] = {
            "S2_reproduces": result["S2_c_after_b3_correct"].get("matching_t5c", False),
            "S4_reproduces_t5c": result["S4_c_empty_pw"].get("matching_t5c", False),
            "note": "S2 reproducing => firmware B3->C state bug; "
                    "only S4 reproducing => tester sent no password (firmware fine)",
        }
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
    path = RESULTS_DIR / f"t5c_repro_{ts}.json"
    result["finished_at"] = wall()
    result["_path"] = str(path)
    path.write_text(json.dumps(sanitize(result), ensure_ascii=False, indent=2), encoding="utf-8")
    print("\n== T5C REPRO SUMMARY ==")
    print(json.dumps(sanitize(result), ensure_ascii=False, indent=2)[:7000])
    print(f"[results] {path}")
    return 0 if result.get("pass") else 1


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="T5 C-round anomaly repro")
    parser.add_argument("--ssid", required=True)
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


if __name__ == "__main__":
    sys.exit(main())
