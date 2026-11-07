#!/usr/bin/env python3
"""DEF-BLE-001 取证：异常终止的 BLE 会话复现器。

模拟现场"主机侧进程被杀、不发 disconnect"的形态：每个子进程连接设备并
（可选）发一条 device.info，然后 **SIGKILL 自己**，不留任何断开报文。
父进程按轮次拉起子进程，观察设备是否出现 adv=0 conn=1（连接槽悬挂）。

用法：
  .venv-bleak/bin/python tools/ble_p0/ble_leak_repro.py --rounds 40 --gap 1.0
  # 串口另开 capture_serial.py 采集 diag 行（含 cf= / rx_age=）
  .venv-bleak/bin/python tools/ble_p0/ble_leak_repro.py --rounds 5 --mode clean   # 对照

注意：本脚本只做连接/断开与一条只读查询，不写配置、不改设备状态。
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import signal
import subprocess
import sys
import time

ADDR_DEFAULT = "B0:A7:32:28:C8:E6"
NUS_CMD_TX = "6E400005-B5A3-F393-E0A9-E50E24DCCA9E"
NUS_CMD_RX = "6E400004-B5A3-F393-E0A9-E50E24DCCA9E"


async def child_once(args: argparse.Namespace) -> int:
    from bleak import BleakClient

    cli = BleakClient(args.address, timeout=20)
    await cli.connect()
    try:
        if args.skip_write:
            # R-BLE-2:连接后不做任何写,直接进入 kill/clean
            if args.mode == "kill":
                os.kill(os.getpid(), signal.SIGKILL)
            await cli.disconnect()
            return 0
        await cli.start_notify(NUS_CMD_TX, lambda _c, _d: None)
        if args.mode == "idle-sub":
            # R-BLE-3:连接 + 订阅后静默挂着,验证"不误杀订阅会话"
            print(f"[child] idle-sub: holding {args.hold_long}s without writes", flush=True)
            await asyncio.sleep(args.hold_long)
            await cli.disconnect()
            return 0
        payload = (json.dumps({"id": "leak-probe", "version": "1.0",
                               "cmd": "device.info", "args": {}},
                              separators=(",", ":")) + "\n").encode()
        for i in range(0, len(payload), 20):
            await cli.write_gatt_char(NUS_CMD_RX, payload[i:i + 20], response=True)
        await asyncio.sleep(args.hold)
    finally:
        if args.mode == "clean":
            await cli.disconnect()
            return 0
    if args.mode == "kill":
        os.kill(os.getpid(), signal.SIGKILL)   # 不发 DISCONNECT
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--child", action="store_true", help="内部使用")
    ap.add_argument("--mode", choices=("kill", "clean", "idle-sub"), default="kill")
    ap.add_argument("--address", default=ADDR_DEFAULT)
    ap.add_argument("--rounds", type=int, default=40)
    ap.add_argument("--gap", type=float, default=1.0)
    ap.add_argument("--hold", type=float, default=0.4)
    ap.add_argument("--skip-write", action="store_true",
                    help="连接后不写任何帧(R-BLE-2 连接年龄兜底场景)")
    ap.add_argument("--hold-long", type=float, default=120.0,
                    help="idle-sub 模式静默保持秒数")
    ap.add_argument("--stop-on-fail", type=int, default=3,
                    help="连续连接失败达到该次数即停止(疑似已悬挂)")
    ns = ap.parse_args()

    if ns.child:
        try:
            asyncio.run(child_once(ns))
        except Exception as exc:   # noqa: BLE001
            print(f"[child] fail: {type(exc).__name__}: {exc}", flush=True)
            return 2
        return 0

    fails = 0
    for i in range(1, ns.rounds + 1):
        cmd = [sys.executable, os.path.abspath(__file__), "--child",
               "--mode", ns.mode, "--address", ns.address,
               "--hold", str(ns.hold), "--hold-long", str(ns.hold_long)]
        if ns.skip_write:
            cmd.append("--skip-write")
        t0 = time.time()
        rc = subprocess.call(cmd)
        dt = time.time() - t0
        # kill 模式下子进程被 SIGKILL 是**预期**结果(不发 DISCONNECT)
        expected_kill = (ns.mode == "kill" and rc == -signal.SIGKILL)
        if rc == 0 or expected_kill:
            fails = 0
            print(f"[{i}/{ns.rounds}] ok ({dt:.1f}s{', killed' if expected_kill else ''})", flush=True)
        else:
            fails += 1
            print(f"[{i}/{ns.rounds}] FAIL rc={rc} ({dt:.1f}s) consecutive={fails}", flush=True)
            if fails >= ns.stop_on_fail:
                print("连续失败达阈值:疑似连接槽悬挂,停止本轮", flush=True)
                return 3
        time.sleep(ns.gap)
    print("rounds done", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
