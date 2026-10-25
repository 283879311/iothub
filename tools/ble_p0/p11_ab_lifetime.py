#!/usr/bin/env python3
"""P1-1 A/B: connection-lifetime probe (5 independent connections).

Each round: fresh connect + CMD TX subscribe only, zero commands, observe
until drop or OBSERVE_S. One observation per connection (no reuse).

  A (pre-guard firmware): expect drops at ~44.8s  -> alive_at_60 false
  B (guard firmware):     expect >=OBSERVE_S alive -> alive_at_60 true

    .venv-bleak/bin/python -u tools/ble_p0/p11_ab_lifetime.py --rounds 5
"""
from __future__ import annotations

import argparse
import asyncio
import json
import time
from datetime import datetime
from pathlib import Path

from bleak import BleakClient, BleakScanner

ADDR_DEFAULT = "B0:A7:32:28:C8:E6"  # BLE OTA address (not the USB/MAC E4)


async def one_round(i: int, addr: str, observe_s: float) -> dict:
    drop_t: dict[str, float | None] = {"t": None}

    def on_drop(_client) -> None:
        if drop_t["t"] is None:
            drop_t["t"] = time.monotonic()

    rm = await asyncio.create_subprocess_exec(
        "bluetoothctl", "remove", addr,
        stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL)
    await rm.wait()
    hits = await BleakScanner.discover(
        timeout=8, service_uuids=["6e400001-b5a3-f393-e0a9-e50e24dcca9e"],
        return_adv=True)
    # bleak keys are uppercase OTA addresses; compare case-insensitively
    if not any(a.upper() == addr.upper() for a in hits):
        return {"round": i, "error": "not found in scan"}
    t0 = time.monotonic()
    cli = BleakClient(addr, timeout=15, disconnected_callback=on_drop)
    await cli.connect()
    conn_t = time.monotonic()
    await cli.start_notify("6e400005-b5a3-f393-e0a9-e50e24dcca9e",
                           lambda c, b: None)
    deadline = conn_t + observe_s
    while time.monotonic() < deadline and drop_t["t"] is None and cli.is_connected:
        await asyncio.sleep(0.2)
    drop = drop_t["t"]
    lifetime = (drop or time.monotonic()) - conn_t
    alive_at_60 = (drop is None and (time.monotonic() - conn_t) >= 60.0)
    try:
        await cli.disconnect()
    except Exception:  # noqa: BLE001
        pass
    return {"round": i, "setup_s": round(conn_t - t0, 2),
            "lifetime_s": round(lifetime, 2), "dropped": drop is not None,
            "alive_at_60": alive_at_60}


async def run(args: argparse.Namespace) -> int:
    rows = []
    for i in range(1, args.rounds + 1):
        row = await one_round(i, args.address, args.observe_s)
        rows.append(row)
        print(json.dumps(row, ensure_ascii=False), flush=True)
        await asyncio.sleep(2.0)
    alive = sum(1 for r in rows if r.get("alive_at_60"))
    dropped = sum(1 for r in rows if r.get("dropped"))
    result = {
        "timestamp": datetime.now().astimezone().isoformat(timespec="seconds"),
        "name": f"P1_1_AB_{args.label.upper()}",
        "firmware": args.label, "rounds": args.rounds,
        "observe_s": args.observe_s,
        "alive_at_60_count": alive, "dropped_count": dropped,
        "pass": alive == args.rounds if args.expect == "alive"
                else dropped == args.rounds,
        "rounds": rows,
    }
    out = Path(__file__).resolve().parent / "results" / \
        f"p1_1_ab_{args.label}_{datetime.now().strftime('%Y%m%d_%H%M%S')}.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, ensure_ascii=False, indent=2),
                   encoding="utf-8")
    print(json.dumps({k: v for k, v in result.items() if k != "rounds"},
                     ensure_ascii=False))
    print(f"[results] {out}")
    return 0 if result["pass"] else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--label", required=True, help="a or b (firmware tag)")
    parser.add_argument("--expect", choices=["alive", "drop"], required=True)
    parser.add_argument("--address", default=ADDR_DEFAULT)
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--observe-s", type=float, default=65.0)
    args = parser.parse_args()
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys_exit = __import__("sys")
    sys_exit.exit(main())
