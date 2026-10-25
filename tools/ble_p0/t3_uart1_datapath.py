#!/usr/bin/env python3
"""T3 addendum: UART1 <-> BLE data path over a real external UART source.

Closes the T3 archive gap: the DATA channel was only tested BLE<->BLE
loopback; the UART1 leg (external source) - inbound reader, data_pipe
routing, DATA TX multi-fragment notify - was source-audited but never
exercised. Requires /dev/ttyUSB1 wired to the device UART1 (GPIO17 TX /
GPIO18 RX, 115200 8N1).

Phases:
  U2B-1: 256B pattern UART1 -> BLE DATA TX (byte-exact, notify sizes)
  U2B-2: 2048B burst UART1 -> BLE (multi-fragment external source; watch
         data_pipe queue-16 overrun on the serial console)
  B2U-1: 200B pattern BLE DATA RX -> UART1 (byte-exact)
  B2U-2: 1000B burst BLE DATA RX -> UART1

Default routes apply: uart1->BLE, ble->UART1|MQTT (BLE-sent bytes are also
published to MQTT by design; a few KB of test traffic to the broker is
expected and noted).

    .venv-bleak/bin/python -u tools/ble_p0/t3_uart1_datapath.py \
      --address b0:a7:32:28:c8:e4 --uart-port /dev/ttyUSB1
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

from bleak import BleakClient

sys.path.insert(0, str(Path(__file__).resolve().parent))
import serial  # noqa: E402  (IDF env provides pyserial; see runner note)

from p0_acceptance import (  # noqa: E402
    CMD_TX,
    DATA_RX,
    DATA_TX,
    Session,
    choose_target,
    clear_cached_iot_targets,
    sanitize,
    scan_filtered,
    stamp,
)
from success_path import RESULTS_DIR, data_of  # noqa: E402


def wall() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


def pattern(n: int, seed: int) -> bytes:
    return bytes(((i * 41 + seed * 17 + 0x3C) & 0xFF) for i in range(n))


class UartSide:
    """Async-ish wrapper over the blocking pyserial port (reader thread)."""

    def __init__(self, port: str, baud: int = 115200) -> None:
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.reset_input_buffer()
        self.rx = bytearray()
        self.stop = False
        import threading

        self._t = threading.Thread(target=self._reader, daemon=True)
        self._t.start()

    def _reader(self) -> None:
        while not self.stop:
            chunk = self.ser.read(512)
            if chunk:
                self.rx.extend(chunk)

    def write(self, data: bytes) -> None:
        self.ser.write(data)
        self.ser.flush()

    def drain(self) -> bytes:
        out = bytes(self.rx)
        self.rx.clear()
        return out

    def close(self) -> None:
        self.stop = True
        self._t.join(timeout=1)
        self.ser.close()


class DataCollector:
    def __init__(self) -> None:
        self.items: list[bytes] = []

    def on_notify(self, _char: Any, data: bytearray) -> None:
        self.items.append(bytes(data))

    def payload(self) -> bytes:
        return b"".join(self.items)

    def sizes(self) -> list[int]:
        return [len(x) for x in self.items]

    def clear(self) -> None:
        self.items.clear()


async def collect_for(dc: DataCollector, seconds: float) -> bytes:
    await asyncio.sleep(seconds)
    return dc.payload()


async def run(args: argparse.Namespace) -> int:
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    result: dict[str, Any] = {
        "timestamp": stamp(), "started_at": wall(),
        "name": "T3_UART1_DATA_PATH",
        "uart_port": args.uart_port,
    }
    uart = None
    cli = None
    try:
        clear_cached_iot_targets()
        rows = await scan_filtered(8)
        target = choose_target(rows, args.address)
        if target is None:
            raise RuntimeError("device not found")
        cli = BleakClient(target["address"], timeout=25)
        await cli.connect()
        sess = Session(cli, address=target["address"],
                       username=args.user, password=args.user_password,
                       max_write=20)
        dc = DataCollector()
        await cli.start_notify(CMD_TX, sess.on_notify)
        await cli.start_notify(DATA_TX, dc.on_notify)
        await asyncio.sleep(0.2)
        await sess.login()
        uart = UartSide(args.uart_port)
        result["board"] = {"address": target["address"]}

        # idle drain: anything stale on either side
        await asyncio.sleep(0.5)
        uart.drain()
        dc.clear()

        # ---- U2B-1: 256B UART1 -> BLE ----
        p1 = pattern(256, 1)
        t0 = time.monotonic()
        uart.write(p1)
        got = b""
        deadline = time.monotonic() + 8
        while len(got) < len(p1) and time.monotonic() < deadline:
            await asyncio.sleep(0.1)
            got = dc.payload()
        result["U2B_256B"] = {
            "sent": len(p1), "recv": len(got), "exact": got == p1,
            "notify_count": len(dc.items), "notify_sizes": dc.sizes(),
            "latency_s": round(time.monotonic() - t0, 3),
        }
        dc.clear()
        await asyncio.sleep(0.5)
        uart.drain()

        # ---- U2B-2: 2048B burst UART1 -> BLE ----
        p2 = pattern(2048, 2)
        t0 = time.monotonic()
        for off in range(0, len(p2), 256):
            uart.write(p2[off:off + 256])   # paced 256B frames (8 frames)
            await asyncio.sleep(0.05)
        got = b""
        deadline = time.monotonic() + 15
        while len(got) < len(p2) and time.monotonic() < deadline:
            await asyncio.sleep(0.15)
            got = dc.payload()
        exact_pos = got.find(p2[:64]) >= 0 if got else False
        result["U2B_2048B_burst"] = {
            "sent": len(p2), "recv": len(got),
            "exact": got == p2,
            "prefix_match": exact_pos,
            "loss_bytes": len(p2) - len(got),
            "notify_count": len(dc.items),
            "notify_sizes_head": dc.sizes()[:12],
            "latency_s": round(time.monotonic() - t0, 3),
            "note": "best-effort pipe: queue-16 overrun may drop bytes; "
                    "serial console checked separately",
        }
        dc.clear()
        await asyncio.sleep(0.5)
        uart.drain()

        # ---- B2U-1: 200B BLE -> UART1 ----
        p3 = pattern(200, 3)
        t0 = time.monotonic()
        for off in range(0, len(p3), 20):
            await cli.write_gatt_char(DATA_RX, p3[off:off + 20], response=True)
        got = b""
        deadline = time.monotonic() + 8
        while len(got) < len(p3) and time.monotonic() < deadline:
            await asyncio.sleep(0.1)
            got = uart.drain()
        result["B2U_200B"] = {
            "sent": len(p3), "recv": len(got), "exact": got == p3,
            "latency_s": round(time.monotonic() - t0, 3),
        }
        uart.drain()
        dc.clear()

        # ---- B2U-2: 1000B burst BLE -> UART1 ----
        p4 = pattern(1000, 4)
        t0 = time.monotonic()
        for off in range(0, len(p4), 20):
            await cli.write_gatt_char(DATA_RX, p4[off:off + 20], response=True)
        got = b""
        deadline = time.monotonic() + 15
        while len(got) < len(p4) and time.monotonic() < deadline:
            await asyncio.sleep(0.1)
            got = uart.drain()
        result["B2U_1000B_burst"] = {
            "sent": len(p4), "recv": len(got), "exact": got == p4,
            "loss_bytes": len(p4) - len(got),
            "latency_s": round(time.monotonic() - t0, 3),
        }
        uart.drain()

        checks = {
            "u2b_256_exact": result["U2B_256B"]["exact"],
            "u2b_2048_prefix": result["U2B_2048B_burst"]["exact"] or
                               result["U2B_2048B_burst"]["prefix_match"],
            "b2u_200_exact": result["B2U_200B"]["exact"],
            "b2u_1000_exact": result["B2U_1000B_burst"]["exact"],
        }
        result["checks"] = checks
        result["pass"] = all(checks.values())
        return finish(result)
    except Exception as exc:  # noqa: BLE001
        result["pass"] = False
        result["fatal"] = f"{type(exc).__name__}: {exc}"
        return finish(result)
    finally:
        if uart is not None:
            uart.close()
        if cli is not None:
            try:
                await cli.disconnect()
            except Exception:  # noqa: BLE001
                pass


def finish(result: dict[str, Any]) -> int:
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    path = RESULTS_DIR / f"t3_uart1_{ts}.json"
    result["finished_at"] = wall()
    result["_path"] = str(path)
    path.write_text(json.dumps(sanitize(result), ensure_ascii=False, indent=2),
                    encoding="utf-8")
    print("\n== T3 UART1 DATA PATH ==")
    print(json.dumps(sanitize(result), ensure_ascii=False, indent=2)[:6000])
    print(f"[results] {path}")
    return 0 if result.get("pass") else 1


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="UART1<->BLE data path test")
    parser.add_argument("--address")
    parser.add_argument("--uart-port", default="/dev/ttyUSB1")
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
