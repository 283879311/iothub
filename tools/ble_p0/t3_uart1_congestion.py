#!/usr/bin/env python3
"""P1-11 residual: dynamic queue-full injection on the UART1 inbound leg.

UART1 RX at 115200 (~11.5KB/s) naturally exceeds the BLE DATA TX drain
(~5KB/s at a 50ms connection interval), so the data_pipe queue (16 x 256B)
must overflow. Verifies the documented drop semantics dynamically:
  - received stream is an ordered subsequence of the sent stream (no
    reordering/corruption);
  - device-side "queue full (port 1)" warnings each report <=256 dropped
    bytes (whole-chunk granularity; serial console audit);
  - accounting closes: sent ~= received + warned-dropped (within console
    log-loss tolerance).

Feed-chunk note: the device reader emits up-to-256B chunks on timing
boundaries, so byte-level subsequence + warning granularity is the
correct invariant (not fixed 256B alignment).

    .venv-bleak/bin/python -u tools/ble_p0/t3_uart1_congestion.py \
      --address B0:A7:32:28:C8:E6 --uart-port /dev/ttyUSB1
"""
from __future__ import annotations

import argparse
import asyncio
import json
import time
from datetime import datetime
from pathlib import Path
from typing import Any

from bleak import BleakClient, BleakScanner

import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
import serial  # noqa: E402

from p0_acceptance import CMD_TX, DATA_TX, Session, sanitize, stamp  # noqa: E402
from success_path import RESULTS_DIR  # noqa: E402


def wall() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


def pattern(n: int, seed: int) -> bytes:
    return bytes(((i * 43 + seed * 19 + 0x5A) & 0xFF) for i in range(n))


def is_ordered_subsequence(sent: bytes, recv: bytes) -> bool:
    """Strict monotone mapping: every received byte matched forward in sent."""
    pos = 0
    for b in recv:
        idx = sent.find(b, pos)
        if idx < 0:
            return False
        pos = idx + 1
    return True


class UartSide:
    def __init__(self, port: str) -> None:
        self.ser = serial.Serial(port, 115200, timeout=0.02)
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.reset_input_buffer()

    def write(self, data: bytes) -> None:
        self.ser.write(data)
        self.ser.flush()

    def close(self) -> None:
        self.ser.close()


async def run(args: argparse.Namespace) -> int:
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    total = args.total_bytes
    expected = pattern(total, 21)
    uart = None
    cli = None
    try:
        hits = await BleakScanner.discover(
            timeout=8,
            service_uuids=["6e400001-b5a3-f393-e0a9-e50e24dcca9e"],
            return_adv=True)
        addr = next((a for a in hits if a.upper() == args.address.upper()), None)
        if addr is None:
            raise RuntimeError("device not found")
        cli = BleakClient(addr, timeout=25)
        await cli.connect()
        sess = Session(cli, address=addr, username=args.user,
                       password=args.user_password, max_write=20)
        collected: list[bytes] = []
        await cli.start_notify(CMD_TX, sess.on_notify)
        await cli.start_notify(DATA_TX, lambda c, d: collected.append(bytes(d)))
        await asyncio.sleep(0.2)
        await sess.login()
        uart = UartSide(args.uart_port)

        t0 = time.monotonic()
        # continuous write at line rate: 4KB blocks back-to-back
        for off in range(0, total, 4096):
            uart.write(expected[off:off + 4096])
        write_s = time.monotonic() - t0
        # drain window for the queue to flush
        got = b""
        deadline = time.monotonic() + args.drain_s
        while time.monotonic() < deadline:
            await asyncio.sleep(0.15)
            got += b"".join(collected)
            collected.clear()
            if len(got) >= total:
                break
        drain_s = time.monotonic() - t0

        result: dict[str, Any] = {
            "timestamp": stamp(), "started_at": wall(),
            "name": "T3_UART1_INBOUND_CONGESTION",
            "sent": total, "recv": len(got),
            "recv_is_ordered_subsequence": is_ordered_subsequence(expected, got),
            "write_s": round(write_s, 2),
            "input_rate_Bps": round(total / write_s, 1),
            "output_rate_Bps": round(len(got) / drain_s, 1),
            "drain_s": round(drain_s, 2),
            "note": "device-side 'queue full (port 1)' warning count and "
                    "per-event byte granularity audited from the serial "
                    "console capture (separate file); accounting tolerance "
                    "covers console log loss",
        }
        # self-check: recv rate should approximate BLE drain, not input rate
        result["checks"] = {
            "ordered_subsequence": result["recv_is_ordered_subsequence"],
            "drops_occurred": len(got) < total,
            "output_throttled_to_ble_rate": result["output_rate_Bps"] < 7000,
        }
        result["pass"] = all(result["checks"].values())
        out = RESULTS_DIR / f"t3_uart1_congestion_{datetime.now().strftime('%Y%m%d_%H%M%S')}.json"
        result["_path"] = str(out)
        out.write_text(json.dumps(sanitize(result), ensure_ascii=False, indent=2),
                       encoding="utf-8")
        print(json.dumps(sanitize(result), ensure_ascii=False, indent=2))
        print(f"[results] {out}")
        return 0 if result["pass"] else 1
    finally:
        if uart is not None:
            uart.close()
        if cli is not None:
            try:
                await cli.disconnect()
            except Exception:  # noqa: BLE001
                pass


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--address", default="B0:A7:32:28:C8:E6")
    parser.add_argument("--uart-port", default="/dev/ttyUSB1")
    parser.add_argument("--total-bytes", type=int, default=32768)
    parser.add_argument("--drain-s", type=float, default=8.0)
    parser.add_argument("--user", default="admin")
    parser.add_argument("--user-password", default="123456")
    args = parser.parse_args()
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
