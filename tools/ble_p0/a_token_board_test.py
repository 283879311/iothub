#!/usr/bin/env python3
"""A-phase token/session board regression (BLE).

This is a board test, not a protocol change. It covers the A-phase contract:
  - login / protected command
  - wrong-password login must not invalidate the existing token
  - same-source re-login invalidates the old token
  - BLE disconnect invalidates the BLE token; reconnect must re-login
  - password change returns sessions_invalidated and invalidates old tokens
  - token is 32 uppercase hex, case-sensitive
  - v0/v1 unauthorized error codes

Passwords/tokens are never printed or written to the result. The temporary
admin password is generated in memory and restored in a finally path.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import secrets
import string
import sys
from datetime import datetime
from pathlib import Path
from typing import Any

from bleak import BleakClient

sys.path.insert(0, str(Path(__file__).resolve().parent))
from p0_acceptance import (  # noqa: E402
    CMD_TX,
    Session,
    find_token,
    response_error_code,
)

RESULTS_DIR = Path(__file__).resolve().parent / "results"
DEFAULT_ADDRESS = "B0:A7:32:28:C8:E6"
DEFAULT_USER = "admin"
DEFAULT_PASSWORD = os.environ.get("IOTHUB_ADMIN_PASSWORD")  # never commit a credential


def stamp() -> str:
    return datetime.now().strftime("%Y%m%d_%H%M%S")


def is_ok(res: Any) -> bool:
    return isinstance(res.obj, dict) and res.obj.get("ok") is True


def error_code(res: Any) -> str | None:
    return response_error_code(res.obj)


def response_data(res: Any) -> dict[str, Any]:
    if not isinstance(res.obj, dict):
        return {}
    for key in ("data", "result"):
        val = res.obj.get(key)
        if isinstance(val, dict):
            return val
    return {}


def temp_password() -> str:
    env = os.environ.get("IOTHUB_ADMIN_TEMP_PASSWORD")
    if env:
        return env
    alphabet = string.ascii_letters + string.digits
    return "T" + "".join(secrets.choice(alphabet) for _ in range(20))


async def connect_session(address: str) -> tuple[BleakClient, Session]:
    last: Exception | None = None
    for attempt in range(5):
        cli = BleakClient(address, timeout=25)
        try:
            await cli.connect()
            sess = Session(cli, address=address, username=DEFAULT_USER,
                           password=DEFAULT_PASSWORD or "", max_write=20)
            sess.record = lambda _text: None  # keep board output compact
            await cli.start_notify(CMD_TX, sess.on_notify)
            await asyncio.sleep(0.25)
            return cli, sess
        except Exception as exc:  # noqa: BLE001
            last = exc
            try:
                await cli.disconnect()
            except Exception:  # noqa: BLE001
                pass
            if attempt + 1 < 5:
                await asyncio.sleep(1.0)
    raise RuntimeError(f"BLE connect failed: {type(last).__name__}: {last}")


async def close_session(cli: BleakClient | None, sess: Session | None) -> None:
    if sess is not None:
        try:
            await sess.close()
        except Exception:  # noqa: BLE001
            pass
    elif cli is not None:
        try:
            await cli.disconnect()
        except Exception:  # noqa: BLE001
            pass


async def raw_v1(sess: Session, token: str | None, cmd: str,
                 args: dict[str, Any] | None = None,
                 id_prefix: str = "raw") -> Any:
    rid = f"{id_prefix}-{sess.next_seq()}"
    req: dict[str, Any] = {"id": rid, "version": "1.0", "cmd": cmd,
                           "args": args or {}}
    if token is not None:
        req["token"] = token
    return await sess.send_request(
        req, timeout=12, retries=0, allow_relogin=False, allow_reconnect=False
    )


async def raw_v0(sess: Session, token: str, method: str,
                 params: dict[str, Any] | None = None) -> Any:
    # v0 only echoes numeric ids; a string id would be answered with id=-1.
    rid = sess.next_seq()
    req: dict[str, Any] = {"id": rid, "method": method,
                           "params": params or {}, "token": token}
    return await sess.send_request(
        req, timeout=12, retries=0, allow_relogin=False, allow_reconnect=False
    )


async def login_raw(sess: Session, username: str, password: str) -> tuple[Any, str | None]:
    res = await raw_v1(sess, None, "auth.login",
                       {"username": username, "password": password},
                       id_prefix="login")
    return res, find_token(res.obj)


async def restore_password(address: str, temp_pw: str, original_pw: str) -> bool:
    cli = sess = None
    try:
        cli, sess = await connect_session(address)
        _res, token = await login_raw(sess, DEFAULT_USER, temp_pw)
        if not token:
            return False
        res = await raw_v1(sess, token, "auth.admin.set",
                           {"old_password": temp_pw, "new_password": original_pw},
                           id_prefix="restore")
        return is_ok(res)
    except Exception:  # noqa: BLE001
        return False
    finally:
        await close_session(cli, sess)


async def run(args: argparse.Namespace) -> int:
    steps: list[dict[str, Any]] = []
    result: dict[str, Any] = {
        "name": "A_TOKEN_BOARD",
        "timestamp": datetime.now().astimezone().isoformat(timespec="seconds"),
        "address": args.address,
        "steps": steps,
        "password_restored": None,
    }
    cli = sess = None
    password_changed = False
    restored = False
    original_pw = args.password
    temp_pw = temp_password()

    def step(name: str, passed: bool, detail: str = "", code: str | None = None,
             latency: float | None = None) -> None:
        row = {"name": name, "pass": bool(passed), "detail": detail}
        if code is not None:
            row["code"] = code
        if latency is not None:
            row["latency_s"] = round(latency, 3)
        steps.append(row)
        print(f"  {'PASS' if passed else 'FAIL'}  {name}" + (f"  [{code}]" if code else ""))

    try:
        cli, sess = await connect_session(args.address)

        # 0. unauthenticated v1 device.info remains exempt
        res = await raw_v1(sess, None, "device.info", {}, id_prefix="info")
        step("0. v1 device.info 无 token", is_ok(res), code=error_code(res))

        # 1. login + protected command
        res, token1 = await login_raw(sess, DEFAULT_USER, original_pw)
        login_ok = is_ok(res) and token1 is not None and len(token1) == 32
        step("1a. 正确口令登录", login_ok,
             detail=f"token_len={len(token1) if token1 else 0}",
             code=error_code(res), latency=res.latency if hasattr(res, "latency") else None)
        if not login_ok:
            raise RuntimeError("login failed; cannot continue")
        sess.token = token1
        expires = response_data(res).get("expires_in")
        step("1b. login expires_in 正常", expires in (600, 3),
             detail=f"expires_in={expires}")
        res = await raw_v1(sess, token1, "network.status", {}, id_prefix="status")
        step("1c. 带 token 的受保护命令", is_ok(res), code=error_code(res))

        # 2. wrong-password login must not invalidate the existing token
        wrong_pw = "wrong-" + secrets.token_hex(4)
        res, wrong_token = await login_raw(sess, DEFAULT_USER, wrong_pw)
        step("2a. 错误口令登录被拒", (not is_ok(res)) and wrong_token is None,
             code=error_code(res))
        res = await raw_v1(sess, token1, "network.status", {}, id_prefix="status2")
        step("2b. 错误口令后旧 token 仍有效", is_ok(res), code=error_code(res))

        # 3. same-source re-login invalidates old token, new token works
        res, token2 = await login_raw(sess, DEFAULT_USER, original_pw)
        step("3a. 正确口令重登", is_ok(res) and token2 is not None,
             code=error_code(res))
        if not token2:
            raise RuntimeError("re-login failed")
        res = await raw_v1(sess, token1, "network.status", {}, id_prefix="old")
        step("3b. 重登后旧 token 失效", error_code(res) == "unauthorized",
             code=error_code(res))
        res = await raw_v1(sess, token2, "network.status", {}, id_prefix="new")
        step("3c. 重登后新 token 有效", is_ok(res), code=error_code(res))

        # 4. disconnect must invalidate BLE token; reconnect requires login
        await close_session(cli, sess)
        cli = sess = None
        cli, sess = await connect_session(args.address)
        res = await raw_v1(sess, token2, "network.status", {}, id_prefix="after-disconnect")
        step("4a. 断连重连后旧 token 失效", error_code(res) == "unauthorized",
             code=error_code(res))
        res, token3 = await login_raw(sess, DEFAULT_USER, original_pw)
        step("4b. 断连重连后重新登录", is_ok(res) and token3 is not None,
             code=error_code(res))
        if not token3:
            raise RuntimeError("post-disconnect login failed")
        sess.token = token3
        res = await raw_v1(sess, token3, "network.status", {}, id_prefix="relogin")
        step("4c. 重连后新 token 有效", is_ok(res), code=error_code(res))

        # 5. password change: response flag, old token/password invalid, new works
        token_before_change = token3
        res = await raw_v1(sess, token3, "auth.admin.set",
                           {"old_password": original_pw, "new_password": temp_pw},
                           id_prefix="chpw")
        data = response_data(res)
        changed_ok = is_ok(res) and data.get("sessions_invalidated") is True
        if is_ok(res):
            password_changed = True
        step("5a. 改密成功且 sessions_invalidated=true", changed_ok,
             detail=f"sessions_invalidated={data.get('sessions_invalidated')}",
             code=error_code(res))
        if not changed_ok:
            # Cannot safely test old/new password if the change did not happen.
            raise RuntimeError("password change did not succeed")
        res = await raw_v1(sess, token_before_change, "network.status", {},
                           id_prefix="old-after-chpw")
        step("5b. 改密后旧 token 失效", error_code(res) == "unauthorized",
             code=error_code(res))
        res_old_pw, old_pw_token = await login_raw(sess, DEFAULT_USER, original_pw)
        step("5c. 改密后旧口令被拒", (not is_ok(res_old_pw)) and old_pw_token is None,
             code=error_code(res_old_pw))
        res_new_pw, token4 = await login_raw(sess, DEFAULT_USER, temp_pw)
        step("5d. 改密后新口令可登录", is_ok(res_new_pw) and token4 is not None,
             code=error_code(res_new_pw))
        if not token4:
            raise RuntimeError("login with new password failed")
        sess.token = token4
        res = await raw_v1(sess, token4, "network.status", {}, id_prefix="new-pw")
        step("5e. 新口令 token 可用", is_ok(res), code=error_code(res))

        # restore original password, then verify old/new semantics
        res = await raw_v1(sess, token4, "auth.admin.set",
                           {"old_password": temp_pw, "new_password": original_pw},
                           id_prefix="restore-inline")
        data = response_data(res)
        restored = is_ok(res) and data.get("sessions_invalidated") is True
        step("5f. 恢复原口令成功", restored, code=error_code(res))
        if not restored:
            raise RuntimeError("password restore failed")
        res = await raw_v1(sess, token4, "network.status", {}, id_prefix="old-restore")
        step("5g. 恢复后临时 token 失效", error_code(res) == "unauthorized",
             code=error_code(res))
        res_restored, token5 = await login_raw(sess, DEFAULT_USER, original_pw)
        step("5h. 恢复后原口令可登录", is_ok(res_restored) and token5 is not None,
             code=error_code(res_restored))
        if not token5:
            raise RuntimeError("login after restore failed")
        sess.token = token5

        # 6. token case sensitivity and v0/v1 unauthorized codes
        res = await raw_v1(sess, token5, "network.status", {}, id_prefix="upper")
        step("6a. 大写 token 有效", is_ok(res), code=error_code(res))
        lower = token5.lower()
        res = await raw_v1(sess, lower, "network.status", {}, id_prefix="lower-v1")
        step("6b. 小写 token v1 拒绝", error_code(res) == "unauthorized",
             code=error_code(res))
        res = await raw_v0(sess, lower, "network.status", {})
        step("6c. 小写 token v0 拒绝", error_code(res) == "auth_required",
             code=error_code(res))
        res = await raw_v0(sess, token5, "network.status", {})
        step("6d. 大写 token v0 有效", is_ok(res), code=error_code(res))

        result["token_hex_len"] = len(token5)
        result["token_upper_hex"] = all(c in "0123456789ABCDEF" for c in token5)
        return 0 if all(s["pass"] for s in steps) and restored else 1
    except Exception as exc:  # noqa: BLE001
        step("ABORT", False, detail=f"{type(exc).__name__}: {str(exc)[:160]}")
        return 1
    finally:
        if password_changed and not restored:
            print("  WARN  改密已发生但未确认恢复,尝试最后恢复...")
            restored = await restore_password(args.address, temp_pw, original_pw)
            step("FINALLY. 恢复原口令", restored)
        result["password_restored"] = restored if password_changed else None
        result["pass"] = bool(steps) and all(s["pass"] for s in steps)
        await close_session(cli, sess)
        RESULTS_DIR.mkdir(parents=True, exist_ok=True)
        path = RESULTS_DIR / f"a_token_board_{stamp()}.json"
        path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n",
                        encoding="utf-8")
        print(f"\n结果: {path}")
        print(f"PASS={result.get('pass')} password_restored={result.get('password_restored')}")


async def run_ttl(args: argparse.Namespace) -> int:
    """Run against a temporary 3000ms-TTL build."""
    steps: list[dict[str, Any]] = []
    result: dict[str, Any] = {
        "name": "A_TOKEN_BOARD_TTL",
        "timestamp": datetime.now().astimezone().isoformat(timespec="seconds"),
        "address": args.address,
        "steps": steps,
    }
    cli = sess = None
    try:
        cli, sess = await connect_session(args.address)
        res, token = await login_raw(sess, DEFAULT_USER, args.password)
        step = {"name": "ttl.login", "pass": is_ok(res) and token is not None,
                "detail": f"expires_in={response_data(res).get('expires_in')}"}
        steps.append(step)
        print(f"  {'PASS' if step['pass'] else 'FAIL'}  ttl.login")
        if not token:
            return 1
        await asyncio.sleep(4.2)
        res = await raw_v1(sess, token, "network.status", {}, id_prefix="ttl-expired")
        passed = error_code(res) == "unauthorized"
        steps.append({"name": "ttl.expired", "pass": passed,
                      "detail": f"code={error_code(res)}"})
        print(f"  {'PASS' if passed else 'FAIL'}  ttl.expired  [{error_code(res)}]")
        result["pass"] = all(s["pass"] for s in steps)
        return 0 if result["pass"] else 1
    except Exception as exc:  # noqa: BLE001
        steps.append({"name": "ABORT", "pass": False,
                      "detail": f"{type(exc).__name__}: {str(exc)[:160]}"})
        return 1
    finally:
        await close_session(cli, sess)
        RESULTS_DIR.mkdir(parents=True, exist_ok=True)
        path = RESULTS_DIR / f"a_token_board_ttl_{stamp()}.json"
        path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n",
                        encoding="utf-8")
        print(f"\n结果: {path}")


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser()
    p.add_argument("--address", default=DEFAULT_ADDRESS)
    p.add_argument("--password", default=DEFAULT_PASSWORD,
                   help="original admin password; env IOTHUB_ADMIN_PASSWORD preferred")
    p.add_argument("--ttl-test", action="store_true",
                   help="run against temporary 3000ms-TTL firmware")
    args = p.parse_args()
    if not args.password:
        p.error("set IOTHUB_ADMIN_PASSWORD or pass --password")
    return args


def main() -> int:
    args = parse_args()
    return asyncio.run(run_ttl(args) if args.ttl_test else run(args))


if __name__ == "__main__":
    raise SystemExit(main())
