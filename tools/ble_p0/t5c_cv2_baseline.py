#!/usr/bin/env python3
"""T5 C v2 baseline on the ORIGINAL firmware (pre P1-10), host-driven.

Locks the firmware-side baseline for the C-round adjudication: 10 rounds of
tryconnect(real password, presence known non-empty) -> poll -> cancel, with
per-round evidence mirroring the v2 tester requirements: request id,
operation_id, password_present (boolean only), password_length, terminal
state/error/elapsed, last_disconnect, connected_ssid.

This cannot tell what the mini-program sent in run1; it proves the firmware
path is clean when the password is definitely non-empty.

    IOTHUB_WIFI_PASSWORD='<WIFI_PASSWORD>' \
      .venv-bleak/bin/python -u tools/ble_p0/t5c_cv2_baseline.py --ssid CMCC-502
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
from p0_acceptance import TERMINAL_STATES, sanitize, stamp  # noqa: E402
from success_path import RESULTS_DIR, close_session, connect_once, data_of  # noqa: E402

SETTLE_S = 8.0  # P1-10 churn-window avoidance between rounds


def wall() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


async def run(args: argparse.Namespace) -> int:
    real_pw = os.environ.get("IOTHUB_WIFI_PASSWORD", "")
    if not real_pw:
        print("missing IOTHUB_WIFI_PASSWORD")
        return 2

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    result: dict[str, Any] = {
        "timestamp": stamp(), "started_at": wall(),
        "name": "T5C_V2_BASELINE_ORIG_FW",
        "ssid": args.ssid,
        "password_present": True,          # boolean only, never the value
        "password_length": len(real_pw),
    }
    cli = sess = None
    try:
        cli, sess, target = await connect_once(args)
        result["board"] = {"address": target["address"], "name": target.get("name")}
        rounds: list[dict[str, Any]] = []
        for i in range(1, 11):
            key = "cv2-" + secrets.token_hex(6)
            t0 = time.monotonic()
            tc = await sess.v1(
                "network.tryconnect",
                {"ssid": args.ssid, "password": real_pw, "idempotency_key": key},
                timeout=20, id_prefix=f"cv2-{i}",
            )
            op = data_of(tc.obj).get("operation_id")
            row: dict[str, Any] = {
                "round": i, "request_id": tc.request_id, "operation_id": op,
                "accepted": tc.obj.get("ok") is True,
                "password_present": True, "password_length": len(real_pw),
            }
            last: dict[str, Any] = {}
            if op:
                while time.monotonic() - t0 < 35:
                    g = await sess.v1("operation.get", {"operation_id": op},
                                      timeout=10, id_prefix=f"cv2-{i}-p")
                    last = data_of(g.obj)
                    if last.get("state") in TERMINAL_STATES | {"got_ip"}:
                        break
                    await asyncio.sleep(0.5)
            row["terminal_state"] = last.get("state")
            row["terminal_error"] = last.get("error")
            row["elapsed_s"] = round(time.monotonic() - t0, 3)
            st = data_of(await sess.v1("network.status", {}, timeout=15,
                                       id_prefix=f"cv2-{i}-st"))
            row["last_disconnect"] = st.get("last_disconnect")
            row["connected_ssid"] = st.get("connected_ssid")
            if last.get("state") == "got_ip" and op:
                await asyncio.sleep(0.3)
                c = await sess.v1("network.cancel", {"operation_id": op},
                                  timeout=15, id_prefix=f"cv2-{i}-c")
                row["cancel"] = data_of(c.obj).get("state")
            rounds.append(row)
            print(f"round {i:02d} state={row['terminal_state']} "
                  f"err={row['terminal_error']} t={row['elapsed_s']}s", flush=True)
            await asyncio.sleep(SETTLE_S)
        result["rounds"] = rounds
        ok = sum(1 for r in rounds if r.get("terminal_state") == "got_ip")
        result["got_ip_count"] = ok
        result["pass"] = ok == 10
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
    path = RESULTS_DIR / f"t5c_cv2_{ts}.json"
    result["finished_at"] = wall()
    result["_path"] = str(path)
    path.write_text(json.dumps(sanitize(result), ensure_ascii=False, indent=2),
                    encoding="utf-8")
    print("\n== C v2 BASELINE ==")
    print(json.dumps(sanitize(result), ensure_ascii=False, indent=2)[:5000])
    print(f"[results] {path}")
    return 0 if result.get("pass") else 1


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="T5 C v2 baseline (original fw)")
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
