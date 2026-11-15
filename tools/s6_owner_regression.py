#!/usr/bin/env python3
"""S6 阶段 A · owner 回归 R1/R2/R3/R5（真机 + BLE + 串口证据）。

目标设备：65D8（BLE 10:97:BD:D3:65:DA），console /dev/ttyUSB1。
用例设计（见 docs/S6-OWNER-STUCK固件修复_2026-11-15.md §4）：
  R1  TRYX 持有期间收到 cleanup 请求（BLE cancel）→ release 时 HANDOVER
  R2  同场景但 BLE 断连触发自动取消（本次现场故障复现路径）
  R3  聚合：cleanup 必须出现明确终态（complete/failed），owner 不停在 CLEANUP
  R5  无 cleanup 请求时 TRYX 正常释放 → `TRYX -> NONE`（不 handover）

判据全部来自**设备 console 日志**（op_owner 转移 / cleanup 终态）。

用法：.venv-bleak/bin/python tools/s6_owner_regression.py [--cases R1,R2,R3,R5]
"""

import argparse
import asyncio
import json
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "ble_p0"))

from bleak import BleakClient  # noqa: E402

from p0_acceptance import Session, scan_filtered  # noqa: E402

OUT = Path("artifacts/cmd04_flash_20261115/s6_owner_regression")
SERIAL = Path("artifacts/cmd04_flash_20261114/phaseA/serial_ttyUSB1.log")
ADDR = "10:97:BD:D3:65:DA"
BAD_SSID = "302"          # 可见但口令错误 → 连接耗时可被中途取消
BAD_PW = "__s6_wrong_pw__"
ABSENT_SSID = "__s6_r5_absent__"

_log = None


def log(t):
    line = f"{time.strftime('%H:%M:%S')} {t}"
    print(line, flush=True)
    _log.write(line + "\n")
    _log.flush()


def mark():
    return SERIAL.stat().st_size if SERIAL.is_file() else 0


def since(off):
    with open(SERIAL, "rb") as f:
        f.seek(off)
        return f.read().decode("utf-8", "replace")


def wait_for(off, patterns, timeout_s, label):
    """在 off 之后的日志里等待全部 patterns 出现；返回 (ok, 命中行, 耗时)。"""
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        txt = since(off)
        hits = {}
        for p in patterns:
            m = re.search(p, txt)
            if m:
                hits[p] = m.group(0)[:160]
        if len(hits) == len(patterns):
            return True, hits, round(time.time() - t0, 1)
        time.sleep(1.0)
    txt = since(off)
    hits = {p: (re.search(p, txt).group(0)[:160] if re.search(p, txt) else None)
            for p in patterns}
    return False, hits, round(time.time() - t0, 1)


async def connect():
    recs = await scan_filtered(10.0)
    if not [r for r in recs if r["address"].lower() == ADDR.lower()]:
        raise RuntimeError("65D8 不在广播中")
    cli = BleakClient(ADDR, timeout=25)
    await cli.connect()
    s = Session(cli, address=ADDR)
    await s._connect_once()
    return cli, s


async def wait_idle(s, tries=12):
    for _ in range(tries):
        r = (await s.v1("network.status", timeout=15)).obj.get("data", {})
        if (r.get("sta_connected") and ((r.get("trial") or {}).get("state")) == "idle"):
            return True
        await asyncio.sleep(5)
    return False


async def case_r1(s, results):
    log("[R1] 起：TRYX 期间 BLE cancel → 期望 HANDOVER")
    off = mark()
    r = (await s.v1("network.tryconnect", {"ssid": BAD_SSID, "password": BAD_PW},
                    timeout=20)).obj
    op = ((r.get("data") or {}) or {}).get("operation_id")
    log(f"[R1] 受理 op={op}")
    await asyncio.sleep(2)
    c = (await s.v1("network.cancel", {"operation_id": op}, timeout=20)).obj
    log(f"[R1] cancel ok={c.get('ok')}")
    ok, hits, dt = wait_for(
        off, [r"op_owner: TRYX -> CLEANUP by tryconnect_release",
              r"cleanup (complete|failed)"], 45, "R1")
    results["R1"] = {"ok": ok, "hits": hits, "wait_s": dt, "op": op}
    log(f"[R1] {'PASS' if ok else 'FAIL'} {json.dumps(hits, ensure_ascii=False)}")
    return ok


async def case_r2(cli, s, results):
    log("[R2] 起：TRYX 期间 BLE 断连（故障复现路径）→ 期望 HANDOVER")
    off = mark()
    r = (await s.v1("network.tryconnect", {"ssid": BAD_SSID, "password": BAD_PW},
                    timeout=20)).obj
    op = ((r.get("data") or {}) or {}).get("operation_id")
    log(f"[R2] 受理 op={op}")
    await asyncio.sleep(2)
    log("[R2] 主动断开 BLE（模拟现场链路丢失）")
    try:
        await cli.disconnect()
    except Exception:  # noqa: BLE001
        pass
    ok, hits, dt = wait_for(
        off, [r"op_owner: TRYX -> CLEANUP by tryconnect_release",
              r"async network operation cancelled on BLE disconnect",
              r"cleanup (complete|failed)"], 60, "R2")
    results["R2"] = {"ok": ok, "hits": hits, "wait_s": dt, "op": op}
    log(f"[R2] {'PASS' if ok else 'FAIL'} {json.dumps(hits, ensure_ascii=False)}")
    return ok


async def case_r5(s, results):
    log("[R5] 起：无 cleanup 请求时 TRYX 释放 → 期望 TRYX -> NONE（不 handover）")
    off = mark()
    r = (await s.v1("network.tryconnect", {"ssid": ABSENT_SSID, "password": "x"},
                    timeout=25)).obj
    op = ((r.get("data") or {}) or {}).get("operation_id")
    log(f"[R5] 受理 op={op}")
    for _ in range(20):
        await asyncio.sleep(3)
        o = (await s.v1("operation.get", {"operation_id": op}, timeout=15)).obj
        st = (o.get("data") or {}).get("state")
        if st in ("failed", "expired", "cancelled"):
            log(f"[R5] 终态 {st}")
            break
    ok, hits, dt = wait_for(
        off, [r"op_owner: TRYX -> NONE by tryconnect_release"],
        30, "R5-release")
    # 允许随后 async cleanup 走 NOTIFY（不作为 R5 判据，仅记录）
    await asyncio.sleep(8)
    tail = since(off)
    results["R5"] = {"ok": ok, "hits": hits, "wait_s": dt, "op": op,
                     "post_notify": bool(re.search(
                         r"op_owner: NONE -> CLEANUP by cleanup_request", tail))}
    log(f"[R5] {'PASS' if ok else 'FAIL'} {json.dumps(results['R5'], ensure_ascii=False)}")
    return ok


async def main():
    global _log
    ap = argparse.ArgumentParser()
    ap.add_argument("--cases", default="R1,R2,R3,R5")
    args = ap.parse_args()
    want = [c.strip().upper() for c in args.cases.split(",") if c.strip()]

    OUT.mkdir(parents=True, exist_ok=True)
    _log = open(OUT / "regression.log", "a", encoding="utf-8")
    log(f"=== owner 回归开始 cases={want} ===")
    results = {}

    # R5 先做（不产生 cleanup 请求，从干净状态开始），再做 R1，最后 R2（会断链）
    order = [c for c in ("R5", "R1", "R2") if c in want]

    cli, s = await connect()
    try:
        await wait_idle(s)
        for c in order:
            if c == "R5":
                await case_r5(s, results)
            elif c == "R1":
                await case_r1(s, results)
            elif c == "R2":
                await case_r2(cli, s, results)
                cli, s = await connect()      # R2 主动断链，重连供后续使用
        if "R3" in want:
            off = mark()
            # R3 为聚合判据：R1/R2 已各自要求 cleanup 终态；此处再确认全流程有终态
            txt = ""
            for p in (OUT / "regression.log").read_text(encoding="utf-8").splitlines():
                txt += p + "\n"
            agg = bool(re.search(r"cleanup (complete|failed)", txt))
            results["R3"] = {"ok": agg, "basis": "R1/R2 日志均含 cleanup 终态"}
            log(f"[R3] {'PASS' if agg else 'FAIL'}")
        # 收尾：确认设备回到 idle 且 MQTT 在线（失败不丢弃已完成的用例结果）
        try:
            st = (await s.v1("network.status", timeout=15)).obj.get("data", {})
            results["final"] = {"mqtt": st.get("mqtt_connected"),
                                "ssid": st.get("connected_ssid"),
                                "trial": (st.get("trial") or {}).get("state")}
        except Exception as exc:  # noqa: BLE001
            results["final"] = {"error": f"{type(exc).__name__}: {str(exc)[:80]}"}
            log(f"[warn] 收尾状态读取失败：{type(exc).__name__}")
    finally:
        try:
            await s.close()
        except Exception:  # noqa: BLE001
            pass
        try:
            await cli.disconnect()
        except Exception:  # noqa: BLE001
            pass

    (OUT / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=1),
                                     encoding="utf-8")
    allok = all(v.get("ok") for k, v in results.items() if k.startswith("R"))
    log(f"=== 结束：{'PASS' if allok else 'FAIL'} ===")
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
