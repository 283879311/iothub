#!/usr/bin/env python3
"""T2: operation slot exhaustion / terminal retention / reclaim.

Flow (starts from a hard reset and an empty operation table):
  A: tryconnect wrong password -> active; B: second tryconnect -> busy (not no_slot)
  A terminal; C/D/E: tryconnect wrong + immediate cancel -> cancelled terminals
  4 slots full; F: tryconnect -> no_slot
  Poll A's old id: terminal until ~600s after A terminal, then not_found
  F2 accepted after A reclaim; C old id poll -> not_found; G accepted
  Active-protection, cancelled-terminal reclaim, failed-terminal reclaim, and
  old-id cache removal are all asserted.

    .venv-bleak/bin/python -u tools/ble_p0/t2_slot_reclaim.py --ssid CMCC-502

The target AP only needs to be visible; the script deliberately supplies a bad
password. It does not apply a config, so the correct Wi-Fi password is never
needed or written to the result.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import secrets
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from p0_acceptance import (  # noqa: E402
    TERMINAL_STATES,
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


def op_id_of(res: Response) -> str | None:
    d = data_of(res.obj)
    v = d.get("operation_id")
    return v if isinstance(v, str) else None


def wall() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


async def tryconnect(sess: Any, prefix: str, ssid: str, password: str) -> Response:
    return await sess.v1(
        "network.tryconnect",
        {
            "ssid": ssid,
            "password": password,
            "idempotency_key": "t2-" + secrets.token_hex(6),
        },
        timeout=20,
        id_prefix=prefix,
    )


async def poll_until_terminal(
    sess: Any, op_id: str, timeout: float = 30
) -> tuple[dict[str, Any], list[dict[str, Any]], float]:
    started = time.monotonic()
    states: list[dict[str, Any]] = []
    last: dict[str, Any] = {}
    while time.monotonic() - started < timeout:
        res = await sess.v1(
            "operation.get", {"operation_id": op_id}, timeout=10, id_prefix="t2-poll"
        )
        d = data_of(res.obj)
        states.append(
            {
                "t_s": round(time.monotonic() - started, 3),
                "state": d.get("state"),
                "error": d.get("error"),
            }
        )
        last = d
        if d.get("state") in (TERMINAL_STATES | {"got_ip"}):
            return d, states, time.monotonic() - started
        await asyncio.sleep(0.5)
    return last, states, time.monotonic() - started


async def poll_old_id_reclaimed(
    sess: Any, op_id: str, terminal_mono: float, retention_s: float, margin_s: float
) -> dict[str, Any]:
    """Poll across the retention boundary until the old id returns not_found.

    The worker wakes every 1s, so reclaim is expected at terminal+600s..+601s.
    Poll from 10s before the boundary to catch the transition instead of
    sampling one arbitrary instant.
    """
    start_at = terminal_mono + max(0.0, retention_s - 10.0)
    deadline = terminal_mono + retention_s + margin_s
    while time.monotonic() < start_at:
        await asyncio.sleep(min(2.0, max(0.05, start_at - time.monotonic())))
    samples: list[dict[str, Any]] = []
    while time.monotonic() < deadline:
        res = await sess.v1(
            "operation.get", {"operation_id": op_id}, timeout=10, id_prefix="t2-old"
        )
        code = response_error_code(res.obj)
        d = data_of(res.obj)
        elapsed = time.monotonic() - terminal_mono
        samples.append(
            {
                "at": wall(),
                "elapsed_s_from_terminal": round(elapsed, 3),
                "ok": res.obj.get("ok") is True,
                "state": d.get("state"),
                "error_code": code,
            }
        )
        if code == "not_found":
            return {
                "reclaimed": True,
                "elapsed_s_from_terminal": round(elapsed, 3),
                "boundary_ok": elapsed >= retention_s - 5.0,
                "samples": samples,
            }
        await asyncio.sleep(2.0)
    return {"reclaimed": False, "samples": samples}


async def run(args: argparse.Namespace) -> int:
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    result: dict[str, Any] = {
        "timestamp": stamp(),
        "started_at": wall(),
        "name": "T2_OPERATION_SLOT_RECLAIM",
        "ssid": args.ssid,
        "retention_s": args.retention,
        "margin_s": args.margin,
    }
    if not args.no_reset:
        result["reset"] = reset_board(args.serial_port)
        await asyncio.sleep(9)

    cli = None
    sess = None
    try:
        cli, sess, target = await connect_once(args)
        result["board"] = {"address": target["address"], "name": target.get("name")}
        result["baseline"] = sanitize(
            data_of((await sess.v1("network.status", {}, timeout=15, id_prefix="t2-st")).obj)
        )

        # A: active operation; B while A active must be busy (not no_slot).
        a_res = await tryconnect(sess, "t2-A", args.ssid, args.wrong_password)
        a_op = op_id_of(a_res)
        result["A"] = {
            "ok": a_res.obj.get("ok") is True,
            "operation_id": a_op,
            "state": data_of(a_res.obj).get("state"),
            "error": sanitize(a_res.obj.get("error")),
            "created_at": wall(),
        }
        await asyncio.sleep(0.25)
        b_res = await tryconnect(sess, "t2-B", args.ssid, args.wrong_password)
        result["B_while_active"] = {
            "ok": b_res.obj.get("ok") is True,
            "error_code": response_error_code(b_res.obj),
            "operation_id": op_id_of(b_res),
        }
        if not a_op:
            result["pass"] = False
            result["reason"] = "A did not return operation_id"
            return _finish(result)
        a_term, a_states, a_elapsed = await poll_until_terminal(sess, a_op, timeout=30)
        a_terminal_mono = time.monotonic()
        result["A_terminal"] = {
            "state": a_term.get("state"),
            "error": a_term.get("error"),
            "at": wall(),
            "elapsed_s": round(a_elapsed, 3),
            "states": a_states,
        }

        # C/D/E: immediate cancel -> cancelled terminals. Add A => 4 slots.
        cancelled_ops: list[dict[str, Any]] = []
        for name in ("C", "D", "E"):
            tc = await tryconnect(sess, f"t2-{name}", args.ssid, args.wrong_password)
            op = op_id_of(tc)
            row: dict[str, Any] = {
                "name": name,
                "ok": tc.obj.get("ok") is True,
                "operation_id": op,
                "state": data_of(tc.obj).get("state"),
                "created_at": wall(),
            }
            if op:
                await asyncio.sleep(0.25)
                cancel = await sess.v1(
                    "network.cancel", {"operation_id": op}, timeout=15, id_prefix=f"t2-{name}-cancel"
                )
                row["cancel_ok"] = cancel.obj.get("ok") is True
                row["cancel_state"] = data_of(cancel.obj).get("state")
                row["cancel_error_code"] = response_error_code(cancel.obj)
                term, states, elapsed = await poll_until_terminal(sess, op, timeout=20)
                row["terminal_state"] = term.get("state")
                row["terminal_error"] = term.get("error")
                row["terminal_at"] = wall()
                row["terminal_elapsed_s"] = round(elapsed, 3)
                row["terminal_mono"] = time.monotonic()
                row["states"] = states
            cancelled_ops.append(row)
            await asyncio.sleep(0.25)
        result["C_D_E"] = cancelled_ops
        c_row = next((r for r in cancelled_ops if r.get("name") == "C"), None)
        c_op = c_row.get("operation_id") if c_row else None
        c_terminal_mono = c_row.get("terminal_mono") if c_row else None

        # F: fifth request while 4 terminal slots are retained -> no_slot.
        f_res = await tryconnect(sess, "t2-F", args.ssid, args.wrong_password)
        result["F_no_slot"] = {
            "ok": f_res.obj.get("ok") is True,
            "error_code": response_error_code(f_res.obj),
            "operation_id": op_id_of(f_res),
        }

        # Poll A old id across the 600s retention boundary.
        result["A_reclaim"] = await poll_old_id_reclaimed(
            sess, a_op, a_terminal_mono, args.retention, args.margin
        )
        # F2 accepted after A slot is reclaimed.
        f2_res = await tryconnect(sess, "t2-F2", args.ssid, args.wrong_password)
        f2_op = op_id_of(f2_res)
        result["F2_after_A_reclaim"] = {
            "ok": f2_res.obj.get("ok") is True,
            "operation_id": f2_op,
            "state": data_of(f2_res.obj).get("state"),
            "error_code": response_error_code(f2_res.obj),
            "created_at": wall(),
        }
        if f2_op:
            # Cancel immediately: if F2 were left to run its 15s candidate, the
            # C boundary poll below would start after C had already been
            # reclaimed and the cancel-terminal boundary would be unobserved.
            await asyncio.sleep(0.25)
            f2_cancel = await sess.v1(
                "network.cancel", {"operation_id": f2_op}, timeout=15,
                id_prefix="t2-F2-cancel",
            )
            result["F2_cancel"] = {
                "ok": f2_cancel.obj.get("ok") is True,
                "state": data_of(f2_cancel.obj).get("state"),
                "error_code": response_error_code(f2_cancel.obj),
            }
            f2_term, _, f2_elapsed = await poll_until_terminal(sess, f2_op, timeout=20)
            result["F2_terminal"] = {
                "state": f2_term.get("state"),
                "elapsed_s": round(f2_elapsed, 3),
            }

        # C old id across the retention boundary (cancelled terminal).
        if c_op and c_terminal_mono is not None:
            result["C_reclaim"] = await poll_old_id_reclaimed(
                sess, c_op, c_terminal_mono, args.retention, args.margin
            )
            g_res = await tryconnect(sess, "t2-G", args.ssid, args.wrong_password)
            g_op = op_id_of(g_res)
            result["G_after_C_reclaim"] = {
                "ok": g_res.obj.get("ok") is True,
                "operation_id": g_op,
                "state": data_of(g_res.obj).get("state"),
                "error_code": response_error_code(g_res.obj),
                "created_at": wall(),
            }
            if g_op:
                await asyncio.sleep(0.25)
                await sess.v1(
                    "network.cancel", {"operation_id": g_op}, timeout=15, id_prefix="t2-G-cancel"
                )
        else:
            result["C_reclaim"] = {"reclaimed": False, "reason": "no C operation"}
            result["G_after_C_reclaim"] = {"ok": False}

        checks = {
            "B_busy_not_no_slot": result["B_while_active"].get("error_code") == "busy",
            "F_no_slot": result["F_no_slot"].get("error_code") == "no_slot",
            "A_terminal": result["A_terminal"].get("state") in ("failed", "cancelled", "expired"),
            "C_cancelled": bool(c_row and c_row.get("terminal_state") == "cancelled"),
            "D_cancelled": bool(next((r for r in cancelled_ops if r.get("name") == "D"), {}).get("terminal_state") == "cancelled"),
            "E_cancelled": bool(next((r for r in cancelled_ops if r.get("name") == "E"), {}).get("terminal_state") == "cancelled"),
            "A_reclaimed": result["A_reclaim"].get("reclaimed") is True,
            "A_old_not_found": result["A_reclaim"].get("reclaimed") is True,
            "A_reclaim_not_early": result["A_reclaim"].get("boundary_ok") is True,
            "F2_accepted": result["F2_after_A_reclaim"].get("ok") is True,
            "C_reclaimed": result["C_reclaim"].get("reclaimed") is True,
            "C_old_not_found": result["C_reclaim"].get("reclaimed") is True,
            "C_reclaim_not_early": result["C_reclaim"].get("boundary_ok") is True,
            "G_accepted": result["G_after_C_reclaim"].get("ok") is True,
        }
        result["checks"] = checks
        result["pass"] = all(checks.values())
        return _finish(result)
    except Exception as exc:  # noqa: BLE001
        result["pass"] = False
        result["fatal"] = f"{type(exc).__name__}: {exc}"
        return _finish(result)
    finally:
        if cli is not None and sess is not None:
            await close_session(cli, sess)
        if not args.no_final_reset:
            reset_board(args.serial_port)


def _finish(result: dict[str, Any]) -> int:
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    path = RESULTS_DIR / f"t2_slot_reclaim_{ts}.json"
    result["finished_at"] = wall()
    result["_path"] = str(path)
    path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print("\n== T2 SUMMARY ==")
    print(json.dumps(sanitize(result), ensure_ascii=False, indent=2)[:6000])
    print(f"[results] {path}")
    return 0 if result.get("pass") else 1


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="T2 operation slot retention/reclaim")
    parser.add_argument("--ssid", required=True, help="visible AP used for wrong-password candidates")
    parser.add_argument("--wrong-password", default="t2-wrong-" + secrets.token_hex(6))
    parser.add_argument("--retention", type=float, default=600.0)
    parser.add_argument("--margin", type=float, default=15.0)
    parser.add_argument("--address")
    parser.add_argument("--user", default="admin")
    parser.add_argument("--user-password", default="123456")
    parser.add_argument("--serial-port", default="/dev/ttyUSB0")
    parser.add_argument("--no-reset", action="store_true", help="do not hard-reset before the test")
    parser.add_argument("--no-final-reset", action="store_true", help="leave the device for inspection")
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
