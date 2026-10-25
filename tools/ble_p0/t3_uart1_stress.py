#!/usr/bin/env python3
"""T3 addendum: single-path UART stress (BLE DATA RX -> data_pipe -> UART1 TX).

Scope per decision: module-level stress of the ONE working UART leg only.
No routing/loopback involved. U2B (UART inbound) is out of scope (P1-11).

Phases:
  S1 no-pace 32KB @240B : BLE feeds faster than UART drains (115200 ≈
     11.5KB/s) -> data_pipe queue-16 overrun expected. Verify drops are
     whole-chunk (no tearing), bounded, and the received stream is a
     correct in-order subsequence of 240B chunks.
  S2 paced 64KB @240B/24ms (~10KB/s < baud): expect ZERO loss.
  S3 8KB @20B small-frame stress: framing overhead pressure, expect zero
     loss at the resulting rate.
  S4 60s endurance @240B/24ms: sustained zero-loss + stability (serial
     console checked separately for heap/overrun).

MTU is 256: single DATA RX writes up to 240B are legal (<=253).

    .venv-bleak/bin/python -u tools/ble_p0/t3_uart1_stress.py \
      --address b0:a7:32:28:c8:e4 --uart-port /dev/ttyUSB1
"""
from __future__ import annotations

import argparse
import asyncio
import json
import time
from datetime import datetime
from pathlib import Path
from typing import Any

from bleak import BleakClient

sys_path = str(Path(__file__).resolve().parent)
import sys  # noqa: E402

sys.path.insert(0, sys_path)
import serial  # noqa: E402

from p0_acceptance import (  # noqa: E402
    CMD_TX,
    DATA_RX,
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
    def __init__(self, port: str, baud: int) -> None:
        self.ser = serial.Serial(port, baud, timeout=0.02)
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
            chunk = self.ser.read(4096)
            if chunk:
                self.rx.extend(chunk)

    def read_all(self) -> bytes:
        out = bytes(self.rx)
        self.rx.clear()
        return out

    def close(self) -> None:
        self.stop = True
        self._t.join(timeout=1)
        self.ser.close()


def verify_subsequence(expected: bytes, received: bytes, chunk: int,
                       seed: int) -> dict[str, Any]:
    """Walk received data as in-order chunks of `expected`.

    data_pipe queues inbound as <=256B messages; a 240B write lands as one
    message, so with drops the received stream must be a subsequence of
    whole 240B chunks (last partial allowed). Any mismatch = tearing.
    """
    pos = 0            # index into received
    epos = 0           # index into expected
    lost_chunks: list[int] = []
    chunk_index = 0
    total_chunks = (len(expected) + chunk - 1) // chunk
    torn = False
    while pos < len(received) and chunk_index < total_chunks:
        take = min(chunk, len(expected) - epos)
        want = expected[epos:epos + take]
        have = received[pos:pos + take]
        if have == want:
            pos += take
            epos += take
            chunk_index += 1
            continue
        # not aligned here: assume this expected chunk was dropped
        lost_chunks.append(chunk_index)
        epos += take
        chunk_index += 1
        if pos + take > len(received):
            break
        if expected[epos - take:epos] != received[pos:pos + take]:
            # neither aligned nor a drop ahead -> tearing/corruption
            torn = True
            break
    return {
        "torn": torn or pos < len(received),
        "consumed": pos,
        "lost_chunks": len(lost_chunks),
        "lost_runs": _runs(lost_chunks),
        "first_lost_chunk": lost_chunks[0] if lost_chunks else None,
    }


def _runs(indices: list[int]) -> list[tuple[int, int]]:
    runs: list[tuple[int, int]] = []
    for i in indices:
        if runs and i == runs[-1][1] + 1:
            runs[-1] = (runs[-1][0], i)
        else:
            runs.append((i, i))
    return runs


async def resilient_write(sess: Session, data: bytes, response: bool,
                          drops: list[str]) -> None:
    """One retry via Session.reconnect on host link drop (P1-1 today)."""
    for attempt in range(2):
        try:
            await sess.cli.write_gatt_char(DATA_RX, data, response=response)
            return
        except Exception as exc:  # noqa: BLE001
            if attempt == 0:
                drops.append(f"{type(exc).__name__}@{time.monotonic():.0f}")
                await sess.reconnect()
                continue
            raise


async def stream_phase(sess: Session, uart: UartSide, name: str, seed: int,
                       total: int, chunk: int, pace_ms: float,
                       timeout_s: float, response: bool) -> dict[str, Any]:
    expected = pattern(total, seed)
    t0 = time.monotonic()
    sent = 0
    link_drops: list[str] = []
    for off in range(0, total, chunk):
        await resilient_write(sess, expected[off:off + chunk], response, link_drops)
        sent = min(off + chunk, total)
        if pace_ms:
            await asyncio.sleep(pace_ms / 1000.0)
    write_s = time.monotonic() - t0
    # drain window: UART needs up to total/baud seconds after last write
    got = b""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        await asyncio.sleep(0.15)
        got += uart.read_all()   # accumulate (v1 overwrote -> recv=0 artifact)
        if len(got) >= total:
            break
    recv_s = time.monotonic() - t0
    res: dict[str, Any] = {
        "phase": name, "chunk": chunk, "pace_ms": pace_ms,
        "write_mode": "with-response" if response else "without-response",
        "host_link_drops": link_drops,
        "sent": sent, "recv": len(got),
        "loss_bytes": sent - min(len(got), sent),
        "write_s": round(write_s, 3), "total_s": round(recv_s, 3),
        "ble_feed_goodput_Bps": round(sent / write_s, 1),
        "uart_goodput_Bps": round(min(len(got), sent) / recv_s, 1),
    }
    if len(got) == sent:
        res["exact"] = got == expected
        res["integrity"] = "exact" if got == expected else "CORRUPT"
    else:
        sub = verify_subsequence(expected, got[:sent], chunk, seed)
        res["exact"] = False
        res["integrity"] = "TORN" if sub["torn"] else "chunk-drops-only"
        res["subsequence"] = sub
    return res


async def run(args: argparse.Namespace) -> int:
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    result: dict[str, Any] = {
        "timestamp": stamp(), "started_at": wall(),
        "name": "T3_UART1_STRESS_SINGLE_PATH",
        "uart_port": args.uart_port, "baud": args.baud,
        "scope": "BLE DATA RX -> data_pipe -> UART1 TX (single leg only)",
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
                       max_write=240)
        await cli.start_notify(CMD_TX, sess.on_notify)
        await asyncio.sleep(0.2)
        await sess.login()
        uart = UartSide(args.uart_port, args.baud)
        result["board"] = {"address": target["address"]}
        await asyncio.sleep(0.5)
        uart.read_all()

        result["S1_no_pace_reliable"] = await stream_phase(
            sess, uart, "S1-no-pace-32KB-rel", 11, 32768, 240, 0,
            timeout_s=12, response=True)
        await asyncio.sleep(1.0); uart.read_all()
        result["S1b_no_pace_unack"] = await stream_phase(
            sess, uart, "S1b-no-pace-64KB-unack", 21, 65536, 240, 0,
            timeout_s=15, response=False)
        await asyncio.sleep(1.0); uart.read_all()
        result["S2_paced_zero_loss"] = await stream_phase(
            sess, uart, "S2-paced-64KB", 12, 65536, 240, 24,
            timeout_s=15, response=True)
        await asyncio.sleep(1.0); uart.read_all()
        result["S3_small_frame"] = await stream_phase(
            sess, uart, "S3-20B-8KB", 13, 8192, 20, 0,
            timeout_s=10, response=False)
        await asyncio.sleep(1.0); uart.read_all()
        result["S4_endurance_60s"] = await stream_phase(
            sess, uart, "S4-endurance", 14,
            int(10240 * args.endurance_s), 240, 24,
            timeout_s=args.endurance_s + 15, response=True)

        checks = {
            "s1_reliable_zero_loss": result["S1_no_pace_reliable"]["loss_bytes"] == 0
                and result["S1_no_pace_reliable"]["exact"] is True,
            "s1b_pressure_not_torn": result["S1b_no_pace_unack"]["integrity"] in
                ("chunk-drops-only", "exact"),
            "s2_zero_loss": result["S2_paced_zero_loss"]["loss_bytes"] == 0
                and result["S2_paced_zero_loss"]["exact"] is True,
            "s3_small_frame_ok": result["S3_small_frame"]["loss_bytes"] == 0
                and result["S3_small_frame"]["exact"] is True,
            "s4_endurance_ok": result["S4_endurance_60s"]["loss_bytes"] == 0
                and result["S4_endurance_60s"]["exact"] is True,
            "s4_throughput_near_baud":
                result["S4_endurance_60s"]["uart_goodput_Bps"] > 9000,
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
    path = RESULTS_DIR / f"t3_uart1_stress_{ts}.json"
    result["finished_at"] = wall()
    result["_path"] = str(path)
    path.write_text(json.dumps(sanitize(result), ensure_ascii=False, indent=2),
                    encoding="utf-8")
    print("\n== UART1 STRESS SUMMARY ==")
    print(json.dumps(sanitize(result), ensure_ascii=False, indent=2)[:7000])
    print(f"[results] {path}")
    return 0 if result.get("pass") else 1


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="UART1 single-path stress")
    parser.add_argument("--address")
    parser.add_argument("--uart-port", default="/dev/ttyUSB1")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--endurance-s", type=int, default=60)
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
