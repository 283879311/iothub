#!/usr/bin/env python3
"""T4 #22 硬件侧等价：跨设备 token 隔离（双机在线注入，wire 级判据）。

与手工卡的关系：卡片前端判据（switched 日志）在正常 A→B 路径永不出现——断链即清
token，客户端没有"A 的 token 仍在手"的状态。本工具用**双机同时在线**的注入法拿
设备侧第一手证据，避免"断连作废"混淆：

  1. A 登录 → tokenA（只记 FNV-1a 8 位指纹，不落明文）；
  2. B 连接并登录 → tokenB；
  3. **在 B 上以 A 的 token 显式注入**发 network.status（禁自动重登/重试）
     → 必须 unauthorized，且响应 id = 请求 id（证明是设备判定，非客户端超时）；
  4. B 用 tokenB 业务请求 → ok；
  5. A 的 tokenA 复查 → 仍 ok（B 的注入/重登不影响 A 的会话槽）；
  6. tokenA ≠ tokenB。

前置：两台设备都在广播（无手机占用 BLE 槽）、已配不同 SSID（身份判别用，
network.status.connected_ssid 佐证归属）。

用法：
  .venv-bleak/bin/python tools/t4_22_token_isolation.py [--json out.json]
"""
from __future__ import annotations

import argparse
import asyncio
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "ble_p0"))
from bleak import BleakClient, BleakScanner  # noqa: E402

import p0_acceptance as P  # noqa: E402

DEVICES = [("IotHubBLE-C8E4", "A"), ("IotHubBLE-65D8", "B")]


def fp(tok: str | None) -> str:
    """FNV-1a 8 位指纹：可跨轮比对、不可逆、不落明文。"""
    h = 0x811C9DC5
    for ch in tok or "":
        h = ((h ^ ord(ch)) * 0x01000193) & 0xFFFFFFFF
    return f"len={len(tok or '')}/fp={h:08x}"


async def discover(names: list[str], timeout: float = 8.0) -> dict[str, str]:
    hits = await BleakScanner.discover(timeout=timeout, return_adv=True)
    found = {}
    for dev, _adv in hits.values():
        if dev.name in names:
            found[dev.name] = dev.address
    missing = [n for n in names if n not in found]
    if missing:
        raise SystemExit(f"未发现设备（可能被手机占用 BLE 槽，不广播）：{missing}")
    return found


async def open_session(addr: str, name: str) -> tuple[BleakClient, P.Session]:
    cli = BleakClient(addr, timeout=25)
    await cli.connect()
    sess = P.Session(cli, address=addr)
    sess.cli = cli
    await cli.start_notify(P.CMD_TX, sess.on_notify)
    await asyncio.sleep(0.3)
    r = await sess.login()
    if not (r.obj.get("ok") is True):
        raise SystemExit(f"{name} 登录失败：{r.obj}")
    return cli, sess


async def biz(sess: P.Session, prefix: str, token_override: str | None = None):
    """业务请求；token_override 时显式注入（禁自动重登/重试），拿第一手判决。

    注意（2026-11-10 harness 自伤）：p0_acceptance 的 send_request **首发不附 token**
    （只在 401 重试分支附 self.token），v1() 才自动附。所以这里必须显式写
    req["token"]——裸发会被设备按 auth_required/unauthorized 拒绝，误判成
    "token 失效"。"""
    rid = f"{prefix}-{sess.next_seq()}"
    tok = token_override if token_override is not None else sess.token
    req = {"id": rid, "version": "1.0", "cmd": "network.status", "args": {}, "token": tok or ""}
    r = await sess.send_request(req, timeout=10, retries=0,
                                allow_relogin=False, allow_reconnect=False)
    code = ""
    if r.obj.get("ok") is not True:
        err = r.obj.get("error")
        code = err.get("code", "") if isinstance(err, dict) else str(err or "")
    return r.obj, code


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", default=None, help="证据 JSON 输出路径")
    opts = ap.parse_args()

    names = [n for n, _ in DEVICES]
    addr = await discover(names)
    started = time.strftime("%Y-%m-%d %H:%M:%S")
    results = []

    cli_a, sess_a = await open_session(addr[names[0]], "A(C8E4)")
    tok_a = sess_a.token
    obj, code = await biz(sess_a, "biza0")
    results.append({"case": "A tokenA 业务请求", "ok": obj.get("ok") is True,
                    "detail": fp(tok_a) + (f" code={code}" if code else "")})

    cli_b, sess_b = await open_session(addr[names[1]], "B(65D8)")
    tok_b = sess_b.token

    # 3) B 上注入 A 的 token —— 必须 unauthorized
    obj, code = await biz(sess_b, "inject", token_override=tok_a)
    inj_ok = obj.get("ok") is False and code == "unauthorized"
    results.append({"case": "B 注入 A 的 token → unauthorized", "ok": inj_ok,
                    "detail": f"id={obj.get('id')} code={code or '-'}"})

    # 4) B 自己的 token 正常
    obj, code = await biz(sess_b, "bizb")
    results.append({"case": "B tokenB 业务请求", "ok": obj.get("ok") is True,
                    "detail": code or "ok"})

    # 5) A 不受 B 影响
    obj, code = await biz(sess_a, "biza")
    results.append({"case": "A tokenA 复查（B 活动后）", "ok": obj.get("ok") is True,
                    "detail": code or "ok"})

    # 6) 指纹不同
    results.append({"case": "tokenA ≠ tokenB", "ok": tok_a != tok_b,
                    "detail": f"A {fp(tok_a)} / B {fp(tok_b)}"})

    await cli_b.disconnect()
    await cli_a.disconnect()

    n_pass = sum(1 for r in results if r["ok"])
    report = {"time": started, "devices": {"A": names[0], "B": names[1]},
              "cases": results, "summary": f"{n_pass}/{len(results)} PASS"}
    for r in results:
        print(f"{'PASS' if r['ok'] else 'FAIL'} [{r['case']}] {r['detail']}", flush=True)
    print(f"== T4#22 硬件侧等价: {report['summary']} ==", flush=True)
    if opts.json:
        Path(opts.json).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"证据已写入 {opts.json}", flush=True)
    return 0 if n_pass == len(results) else 1


sys.exit(asyncio.run(main()))
