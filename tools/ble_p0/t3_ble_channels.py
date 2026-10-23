#!/usr/bin/env python3
"""T3: BLE GATT/CMD/DATA channel acceptance at MTU 23 (and MTU 256 baseline).

This is a test harness, not a protocol change. It drives both NUS paths:

  CMD  : CMD RX (0004, write) -> JSON line reassembly -> CMD TX (0005, notify)
  DATA : DATA RX (0002, write) -> data_pipe -> DATA TX (0003, notify)

The DATA path is a byte pipe with no application ACK/sequence/retransmit and no
frame reassembly. The script therefore checks byte-exact echo, ordering, chunk
sizes, duplicate behaviour, subscription gating, and disconnect behaviour, and
records the reliability boundary explicitly instead of claiming delivery from
a successful GATT write.

Run (MTU 23 test build or normal MTU 256):
    .venv-bleak/bin/python -u tools/ble_p0/t3_ble_channels.py --ssid CMCC-502
"""
from __future__ import annotations

import argparse
import asyncio
import json
import secrets
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any

from bleak import BleakClient

sys.path.insert(0, str(Path(__file__).resolve().parent))
from p0_acceptance import (  # noqa: E402
    CMD_TX,
    DATA_RX,
    DATA_TX,
    Session,
    choose_target,
    clear_cached_iot_targets,
    now,
    response_error_code,
    sanitize,
    scan_filtered,
    stamp,
)
from success_path import RESULTS_DIR, data_of, reset_board  # noqa: E402


def wall() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


@dataclass
class DataNote:
    t: float
    data: bytes


class DataCollector:
    """Raw DATA TX notifications; deliberately no JSON/frame interpretation."""

    def __init__(self) -> None:
        self.items: list[DataNote] = []
        self.total = 0

    def on_notify(self, _char: Any, data: bytearray) -> None:
        raw = bytes(data)
        self.items.append(DataNote(now(), raw))
        self.total += len(raw)

    def clear(self) -> None:
        self.items.clear()
        self.total = 0

    def payload(self) -> bytes:
        return b"".join(item.data for item in self.items)

    async def drain(self, quiet_s: float = 0.15) -> None:
        await asyncio.sleep(quiet_s)
        self.clear()

    async def wait_bytes(self, expected: int, timeout: float = 5.0) -> bytes:
        deadline = now() + timeout
        while now() < deadline and self.total < expected:
            await asyncio.sleep(0.01)
        return self.payload()

    def sizes(self) -> list[int]:
        return [len(item.data) for item in self.items]

    def times(self) -> list[float]:
        return [round(item.t, 3) for item in self.items]


def chunked(data: bytes, size: int) -> list[bytes]:
    return [data[i:i + size] for i in range(0, len(data), size)]


def pattern(n: int, seed: int) -> bytes:
    return bytes(((i * 37 + seed * 11 + 0xA5) & 0xFF) for i in range(n))


async def open_session(
    args: argparse.Namespace,
    target: dict[str, Any] | None = None,
):
    if target is None:
        clear_cached_iot_targets()
        rows = await scan_filtered(8)
        target = choose_target(rows, args.address)
        if target is None:
            raise RuntimeError("IotHub NUS device not found")
    cli = BleakClient(target["address"], timeout=25)
    await cli.connect()
    sess = Session(
        cli,
        address=target["address"],
        username=args.user,
        password=args.user_password,
        max_write=args.max_write,
    )
    dc = DataCollector()
    await cli.start_notify(CMD_TX, sess.on_notify)
    await cli.start_notify(DATA_TX, dc.on_notify)
    await asyncio.sleep(0.2)
    await sess.login()
    return cli, sess, target, dc


async def close_session(cli: BleakClient | None, sess: Session | None) -> None:
    if sess is not None:
        try:
            await sess.close()
        except Exception:  # noqa: BLE001
            pass
    if cli is not None:
        try:
            await cli.disconnect()
        except Exception:  # noqa: BLE001
            pass


def service_uuid_str(obj: Any) -> str:
    return str(getattr(obj, "uuid", obj)).lower()


def audit_gatt(cli: BleakClient) -> dict[str, Any]:
    services = cli.services
    services = list(services) if services is not None else []
    rows: list[dict[str, Any]] = []
    for svc in services:
        chars = []
        for ch in svc.characteristics:
            chars.append(
                {
                    "uuid": service_uuid_str(ch),
                    "handle": getattr(ch, "handle", None),
                    "properties": sorted(str(p) for p in (ch.properties or [])),
                    "descriptors": [service_uuid_str(d) for d in (ch.descriptors or [])],
                }
            )
        rows.append({"uuid": service_uuid_str(svc), "characteristics": chars})
    out: dict[str, Any] = {
        "mtu": getattr(cli, "mtu_size", None),
        "services": rows,
    }

    def char(uuid_tail: str) -> dict[str, Any] | None:
        for svc in rows:
            for ch in svc["characteristics"]:
                if ch["uuid"].endswith(uuid_tail):
                    return ch
        return None

    data_rx = char("0002-b5a3-f393-e0a9-e50e24dcca9e")
    data_tx = char("0003-b5a3-f393-e0a9-e50e24dcca9e")
    cmd_rx = char("0004-b5a3-f393-e0a9-e50e24dcca9e")
    cmd_tx = char("0005-b5a3-f393-e0a9-e50e24dcca9e")
    out["data_rx"] = data_rx
    out["data_tx"] = data_tx
    out["cmd_rx"] = cmd_rx
    out["cmd_tx"] = cmd_tx
    out["checks"] = {
        "service_present": any(s["uuid"].startswith("6e400001") for s in rows),
        "data_rx_write": bool(
            data_rx
            and "write" in data_rx["properties"]
            and "write-without-response" in data_rx["properties"]
        ),
        "data_tx_notify": bool(data_tx and "notify" in data_tx["properties"]),
        "cmd_rx_write": bool(
            cmd_rx
            and "write" in cmd_rx["properties"]
            and "write-without-response" in cmd_rx["properties"]
        ),
        "cmd_tx_notify": bool(cmd_tx and "notify" in cmd_tx["properties"]),
        "data_tx_cccd": bool(
            data_tx
            and any(d.startswith("00002902") for d in data_tx["descriptors"])
        ),
        "cmd_tx_cccd": bool(
            cmd_tx and any(d.startswith("00002902") for d in cmd_tx["descriptors"])
        ),
        "data_tx_no_indicate": bool(data_tx and "indicate" not in data_tx["properties"]),
        "cmd_tx_no_indicate": bool(cmd_tx and "indicate" not in cmd_tx["properties"]),
    }
    return out


async def test_data_single(
    cli: BleakClient, dc: DataCollector, args: argparse.Namespace
) -> dict[str, Any]:
    await dc.drain()
    payload = pattern(args.max_write, seed=1)
    t0 = now()
    await cli.write_gatt_char(DATA_RX, payload, response=True)
    echo = await dc.wait_bytes(len(payload), timeout=3)
    return {
        "sent_len": len(payload),
        "echo_len": len(echo),
        "exact": echo == payload,
        "latency_s": round((dc.items[-1].t - t0) if dc.items else -1, 4),
        "notify_sizes": dc.sizes(),
        "sent_hex": payload.hex(),
        "echo_hex": echo.hex()[:160],
    }


async def test_data_long(
    cli: BleakClient, dc: DataCollector, args: argparse.Namespace
) -> dict[str, Any]:
    await dc.drain()
    payload = pattern(args.data_frame_size, seed=2)
    chunks = chunked(payload, args.max_write)
    t0 = now()
    writes = []
    for chunk in chunks:
        started = now()
        await cli.write_gatt_char(DATA_RX, chunk, response=True)
        writes.append({"len": len(chunk), "t0": round(started, 3), "t1": round(now(), 3)})
    echo = await dc.wait_bytes(len(payload), timeout=5)
    return {
        "sent_len": len(payload),
        "write_chunks": len(chunks),
        "write_sizes": [len(c) for c in chunks],
        "echo_len": len(echo),
        "exact": echo == payload,
        "latency_s": round((dc.items[-1].t - t0) if dc.items else -1, 4),
        "notify_count": len(dc.items),
        "notify_sizes": dc.sizes(),
        "write_rows": writes,
    }


async def test_data_duplicate(
    cli: BleakClient, dc: DataCollector, args: argparse.Namespace
) -> dict[str, Any]:
    await dc.drain()
    frame = pattern(args.max_write, seed=3)
    await cli.write_gatt_char(DATA_RX, frame, response=True)
    await cli.write_gatt_char(DATA_RX, frame, response=True)
    echo = await dc.wait_bytes(2 * len(frame), timeout=3)
    return {
        "frame_len": len(frame),
        "echo_len": len(echo),
        "two_copies": echo == frame + frame,
        "notify_sizes": dc.sizes(),
    }


async def test_data_continuous(
    cli: BleakClient, dc: DataCollector, args: argparse.Namespace
) -> dict[str, Any]:
    await dc.drain()
    frames = [
        pattern(args.max_write * args.continuous_chunks, seed=10 + i)
        for i in range(args.continuous_frames)
    ]
    expected = b"".join(frames)
    t0 = now()
    writes = []
    for frame in frames:
        for chunk in chunked(frame, args.max_write):
            started = now()
            await cli.write_gatt_char(DATA_RX, chunk, response=True)
            writes.append({"len": len(chunk), "t0": round(started, 3), "t1": round(now(), 3)})
    echo = await dc.wait_bytes(len(expected), timeout=10)
    return {
        "frames": len(frames),
        "frame_size": len(frames[0]) if frames else 0,
        "write_chunks": len(writes),
        "sent_len": len(expected),
        "echo_len": len(echo),
        "exact": echo == expected,
        "notify_count": len(dc.items),
        "notify_sizes": dc.sizes(),
        "latency_s": round((dc.items[-1].t - t0) if dc.items else -1, 4),
    }


async def test_data_burst(
    cli: BleakClient, dc: DataCollector, args: argparse.Namespace
) -> dict[str, Any]:
    await dc.drain()
    frames = [pattern(args.max_write, seed=100 + i) for i in range(args.burst_writes)]
    expected = b"".join(frames)
    sent = 0
    t0 = now()
    for frame in frames:
        try:
            await cli.write_gatt_char(DATA_RX, frame, response=False)
            sent += len(frame)
        except Exception as exc:  # noqa: BLE001
            return {
                "requested_writes": args.burst_writes,
                "sent_len": sent,
                "error": f"{type(exc).__name__}: {exc}",
                "echo_len": dc.total,
            }
    echo = await dc.wait_bytes(len(expected), timeout=5)
    return {
        "requested_writes": args.burst_writes,
        "sent_len": sent,
        "echo_len": len(echo),
        "exact": echo == expected,
        "notify_count": len(dc.items),
        "notify_sizes": dc.sizes(),
        "latency_s": round((dc.items[-1].t - t0) if dc.items else -1, 4),
        "note": "write-without-response: delivery is not guaranteed by ATT",
    }


async def test_data_max_boundary(
    cli: BleakClient, dc: DataCollector, args: argparse.Namespace, mtu: int | None
) -> dict[str, Any]:
    await dc.drain()
    if args.data_max_write > 0:
        payload_limit = args.data_max_write
    else:
        # Bleak/BlueZ may report the default 23 even after a larger ATT MTU was
        # negotiated; use an explicit CLI value for the boundary probe.
        payload_limit = 20 if not mtu or mtu <= 23 else min(240, mtu - 3)
    payload = pattern(payload_limit, seed=4)
    t0 = now()
    await cli.write_gatt_char(DATA_RX, payload, response=True)
    echo = await dc.wait_bytes(len(payload), timeout=3)
    return {
        "host_reported_mtu": mtu,
        "payload_limit": len(payload),
        "sent_len": len(payload),
        "echo_len": len(echo),
        "exact": echo == payload,
        "notify_sizes": dc.sizes(),
        "latency_s": round((dc.items[-1].t - t0) if dc.items else -1, 4),
    }


async def test_data_unsubscribed(
    cli: BleakClient, dc: DataCollector, args: argparse.Namespace
) -> dict[str, Any]:
    await cli.stop_notify(DATA_TX)
    await dc.drain()
    payload = pattern(args.max_write, seed=5)
    write_ok = True
    try:
        await cli.write_gatt_char(DATA_RX, payload, response=True)
    except Exception:  # noqa: BLE001
        write_ok = False
    await asyncio.sleep(0.4)
    while_unsub = dc.payload()
    await cli.start_notify(DATA_TX, dc.on_notify)
    await dc.drain()
    marker = pattern(args.max_write, seed=6)
    await cli.write_gatt_char(DATA_RX, marker, response=True)
    after = await dc.wait_bytes(len(marker), timeout=3)
    await asyncio.sleep(0.3)
    return {
        "gatt_write_ok_while_unsubscribed": write_ok,
        "echo_while_unsubscribed": len(while_unsub),
        "resubscribe_marker_exact": after == marker,
        "stale_after_resubscribe": dc.payload() != marker,
        "note": "DATA TX subscription gates delivery; GATT write success is not delivery",
    }


async def test_long_command(
    sess: Session, args: argparse.Namespace
) -> dict[str, Any]:
    # A synthetic v0 command of >=275B with an unknown method: exercises CMD RX
    # line reassembly and 20B host chunking without changing any configuration.
    target_len = max(275, args.long_command_size)
    req: dict[str, Any] = {
        "id": 9001,
        "method": "__t3_unknown__",
        "params": {"padding": ""},
    }
    if sess.token:
        req["token"] = sess.token
    payload = json.dumps(req, ensure_ascii=False, separators=(",", ":")).encode()
    pad = max(0, target_len - len(payload))
    req["params"] = {"padding": "A" * pad}
    payload = json.dumps(req, ensure_ascii=False, separators=(",", ":")).encode()
    t0 = now()
    res = await sess.send_request(req, timeout=15)
    return {
        "target_len": target_len,
        "actual_len": len(payload),
        "write_chunks": (len(payload) + args.max_write - 1) // args.max_write,
        "latency_s": round(now() - t0, 4),
        "ok": res.obj.get("ok") is True,
        "error_code": response_error_code(res.obj),
        "note": "unknown method; no config mutation; verifies 20B line reassembly",
    }


async def test_cmd_long_and_concurrent(
    sess: Session,
    cli: BleakClient,
    dc: DataCollector,
    args: argparse.Namespace,
) -> dict[str, Any]:
    # Start a wrong-password candidate so operation.get has a non-terminal state.
    tc = await sess.v1(
        "network.tryconnect",
        {
            "ssid": args.ssid,
            "password": args.wrong_password,
            "idempotency_key": "t3-" + secrets.token_hex(6),
        },
        timeout=20,
        id_prefix="t3-tc",
    )
    op_id = data_of(tc.obj).get("operation_id")
    if not isinstance(op_id, str):
        return {"ok": False, "reason": "tryconnect did not return operation_id",
                "tryconnect": sanitize(tc.obj)}

    fragments: list[dict[str, Any]] = []
    first = asyncio.Event()

    def hook(data: bytes, t: float) -> None:
        fragments.append({"t": round(t, 4), "len": len(data)})
        if len(fragments) == 1:
            first.set()

    sess.cmd_notify_hook = hook
    t0 = now()
    cfg_task = asyncio.create_task(
        sess.v1("config.get", {}, timeout=30, id_prefix="t3-cfg")
    )
    try:
        await asyncio.wait_for(first.wait(), timeout=3)
    except asyncio.TimeoutError:
        pass
    first_seen = bool(fragments)
    data_payload = pattern(args.max_write * args.concurrent_data_chunks, seed=7)
    await dc.drain()
    for chunk in chunked(data_payload, args.max_write):
        await cli.write_gatt_char(DATA_RX, chunk, response=True)

    opget_sent = now()
    opget = await sess.v1(
        "operation.get", {"operation_id": op_id}, timeout=20, id_prefix="t3-opget"
    )
    opget_done = getattr(opget, "rx_time", None) or now()

    try:
        cfg_res = await cfg_task
        # rx_time is when the complete JSON arrived, not when this coroutine
        # later got scheduled after awaiting operation.get.
        cfg_done = getattr(cfg_res, "rx_time", None) or now()
        cfg_ok = cfg_res.obj.get("ok") is True
        cfg_len = len(cfg_res.raw)
    except Exception as exc:  # noqa: BLE001
        cfg_done = now()
        cfg_ok = False
        cfg_len = 0
        cfg_error = f"{type(exc).__name__}: {exc}"
    else:
        cfg_error = None
    data_echo = await dc.wait_bytes(len(data_payload), timeout=5)
    sess.cmd_notify_hook = None

    # Cancel the candidate; no apply/config mutation.
    cancel = await sess.v1(
        "network.cancel", {"operation_id": op_id}, timeout=15, id_prefix="t3-cancel"
    )
    return {
        "operation_id": op_id,
        "tryconnect": sanitize(tc.obj),
        "cmd_first_fragment": first_seen,
        "cmd_fragments": fragments,
        "cmd_fragment_count": len(fragments),
        "cmd_fragment_sizes": [f["len"] for f in fragments],
        "long_response_len": cfg_len,
        "long_response_ok": cfg_ok,
        "long_response_error": cfg_error,
        "opget_state": data_of(opget.obj).get("state"),
        "opget_error": data_of(opget.obj).get("error"),
        "opget_sent_during_long": opget_sent < cfg_done,
        "opget_done_before_long": opget_done < cfg_done,
        "opget_latency_s": round(opget_done - opget_sent, 4),
        "data_sent_len": len(data_payload),
        "data_echo_len": len(data_echo),
        "data_exact": data_echo == data_payload,
        "data_notify_sizes": dc.sizes(),
        "cancel": sanitize(cancel.obj),
        "long_started": round(t0, 3),
        "long_finished": round(cfg_done, 3),
    }


async def test_disconnect(
    args: argparse.Namespace,
    target: dict[str, Any],
    cli: BleakClient,
    sess: Session,
    dc: DataCollector,
    result: dict[str, Any],
) -> tuple[BleakClient, Session, DataCollector]:
    # CMD: start a long response, disconnect during its fragments, reconnect,
    # login, and issue a fresh command. The old partial JSON must not leak.
    fragments: list[dict[str, Any]] = []
    first = asyncio.Event()

    def hook(data: bytes, t: float) -> None:
        fragments.append({"t": round(t, 4), "len": len(data)})
        if len(fragments) == 1:
            first.set()

    sess.cmd_notify_hook = hook
    cfg_task = asyncio.create_task(
        sess.v1("config.get", {}, timeout=30, id_prefix="t3-dc-cfg")
    )
    try:
        await asyncio.wait_for(first.wait(), timeout=3)
    except asyncio.TimeoutError:
        pass
    await cli.disconnect()
    cfg_task.cancel()
    try:
        await cfg_task
    except (Exception, asyncio.CancelledError):  # noqa: BLE001
        pass
    await close_session(cli, sess)
    await asyncio.sleep(1.0)

    cli, sess, target, dc = await open_session(args, target=target)
    responses_before_info = len(sess.responses)
    info = await sess.v1("device.info", {}, timeout=10, id_prefix="t3-dc-info")
    info_id = getattr(info, "request_id", None)
    new_responses = sess.responses[responses_before_info:]
    leaked = (
        any(r.request_id != info_id for r in new_responses)
        or len(sess.events) > 0
        or bool(sess.stream.buf)
    )
    result["disconnect_cmd"] = {
        "fragments_before_disconnect": len(fragments),
        "reconnect_device_info_ok": info.obj.get("ok") is True,
        "reconnect_device_info_error": response_error_code(info.obj),
        "new_session_response_ids": [r.request_id for r in new_responses],
        "old_partial_json_leaked": leaked,
    }

    # DATA: create a burst, disconnect immediately, reconnect and subscribe.
    # The firmware currently has no data_pipe session flush; this probes whether
    # any queued bytes are delivered to the new subscription.
    await dc.drain(0.05)
    burst = [pattern(args.max_write, seed=200 + i) for i in range(args.disconnect_burst)]
    sent = 0
    for frame in burst:
        try:
            await cli.write_gatt_char(DATA_RX, frame, response=False)
            sent += len(frame)
        except Exception:  # noqa: BLE001
            break
    await cli.disconnect()
    await close_session(cli, sess)
    await asyncio.sleep(1.0)

    cli, sess, target, dc = await open_session(args, target=target)
    # Do not clear here: bytes queued from the old session (if any) must be
    # visible before the marker. The firmware's data_pipe has no session id.
    await asyncio.sleep(0.6)
    pre_marker = dc.payload()
    marker = pattern(args.max_write, seed=250)
    await cli.write_gatt_char(DATA_RX, marker, response=True)
    await dc.wait_bytes(len(pre_marker) + len(marker), timeout=3)
    await asyncio.sleep(0.4)
    all_data = dc.payload()
    result["disconnect_data"] = {
        "burst_sent_len": sent,
        "pre_marker_bytes": len(pre_marker),
        "marker_len": len(marker),
        "echo_after_reconnect": len(all_data),
        "marker_exact": all_data[len(pre_marker):] == marker,
        "stale_bytes_after_reconnect": len(pre_marker) > 0,
        "note": "source audit: data_pipe has no disconnect flush or session id",
    }
    return cli, sess, dc


async def run(args: argparse.Namespace) -> int:
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    result: dict[str, Any] = {
        "timestamp": stamp(),
        "started_at": wall(),
        "name": "T3_BLE_CMD_DATA_CHANNELS",
        "ssid": args.ssid,
        "max_write": args.max_write,
        "data_frame_size": args.data_frame_size,
    }
    if not args.no_reset:
        result["reset"] = reset_board(args.serial_port)
        await asyncio.sleep(9)

    cli = sess = None
    target = None
    dc = None
    try:
        cli, sess, target, dc = await open_session(args)
        result["board"] = {"address": target["address"], "name": target.get("name")}
        result["gatt"] = audit_gatt(cli)
        result["mtu"] = getattr(cli, "mtu_size", None)
        result["host_reported_mtu"] = result["mtu"]
        result["mtu_note"] = (
            "Bleak/BlueZ may report the default 23; the observed CMD notify "
            "chunk sizes are the authoritative negotiated payload."
        )
        result["device_info"] = sanitize(
            (await sess.v1("device.info", {}, timeout=10, id_prefix="t3-info")).obj
        )

        loop = await sess.v1(
            "data.loopback", {"enabled": True}, timeout=10, id_prefix="t3-loop"
        )
        result["loopback_enable"] = {
            "ok": loop.obj.get("ok") is True,
            "error_code": response_error_code(loop.obj),
            "data": sanitize(data_of(loop.obj)),
        }
        if loop.obj.get("ok") is not True:
            result["pass"] = False
            result["reason"] = "data.loopback enable failed"
            return _finish(result)

        result["data_single"] = await test_data_single(cli, dc, args)
        result["data_long"] = await test_data_long(cli, dc, args)
        result["data_duplicate"] = await test_data_duplicate(cli, dc, args)
        result["data_continuous"] = await test_data_continuous(cli, dc, args)
        result["data_max"] = await test_data_max_boundary(cli, dc, args, result["mtu"])
        result["data_unsubscribed"] = await test_data_unsubscribed(cli, dc, args)
        result["data_burst"] = await test_data_burst(cli, dc, args)
        result["cmd_long_command"] = await test_long_command(sess, args)
        result["cmd_concurrent"] = await test_cmd_long_and_concurrent(
            sess, cli, dc, args
        )
        result["observed_cmd_notify_max"] = max(
            result["cmd_concurrent"].get("cmd_fragment_sizes") or [0]
        )
        result["observed_data_notify_max"] = max(
            result["data_continuous"].get("notify_sizes") or [0]
        )

        cli, sess, dc = await test_disconnect(args, target, cli, sess, dc, result)

        # Leave the runtime loopback off; it is not persisted.
        try:
            off = await sess.v1(
                "data.loopback", {"enabled": False}, timeout=10, id_prefix="t3-loop-off"
            )
            result["loopback_disable"] = {
                "ok": off.obj.get("ok") is True,
                "data": sanitize(data_of(off.obj)),
            }
        except Exception as exc:  # noqa: BLE001
            result["loopback_disable"] = {"error": f"{type(exc).__name__}: {exc}"}

        checks = {
            "gatt_all": all(result["gatt"]["checks"].values()),
            "data_single_exact": result["data_single"].get("exact") is True,
            "data_long_exact": result["data_long"].get("exact") is True,
            "data_duplicate_two_copies": result["data_duplicate"].get("two_copies") is True,
            "data_continuous_exact": result["data_continuous"].get("exact") is True,
            "data_max_exact": result["data_max"].get("exact") is True,
            "data_unsubscribed_dropped": (
                result["data_unsubscribed"].get("echo_while_unsubscribed") == 0
                and result["data_unsubscribed"].get("gatt_write_ok_while_unsubscribed") is True
            ),
            "data_resubscribe_no_stale": (
                result["data_unsubscribed"].get("resubscribe_marker_exact") is True
                and result["data_unsubscribed"].get("stale_after_resubscribe") is False
            ),
            "cmd_long_command_received": (
                result["cmd_long_command"].get("actual_len", 0) >= 275
                and result["cmd_long_command"].get("error_code") is not None
            ),
            "cmd_long_response_ok": result["cmd_concurrent"].get("long_response_ok") is True,
            "cmd_opget_during_long": (
                (not args.require_opget_during)
                or result["cmd_concurrent"].get("opget_sent_during_long") is True
            ),
            "cmd_opget_state": result["cmd_concurrent"].get("opget_state") in
                ("accepted", "connecting", "failed", "cancelled", "got_ip"),
            "cmd_data_concurrent": result["cmd_concurrent"].get("data_exact") is True,
            "cmd_disconnect_clean": (
                result["disconnect_cmd"].get("reconnect_device_info_ok") is True
                and result["disconnect_cmd"].get("old_partial_json_leaked") is False
            ),
            "data_disconnect_clean": (
                result["disconnect_data"].get("marker_exact") is True
                and result["disconnect_data"].get("stale_bytes_after_reconnect") is False
            ),
        }
        result["checks"] = checks
        result["reliability_boundary"] = {
            "data_layer": "raw byte pipe (data_pipe), no app ACK/sequence/retransmit/framing",
            "gatt_write_success_is_delivery": False,
            "data_pipe_queue_depth": 16,
            "data_pipe_chunk_max": 256,
            "data_notify_chunking": "MTU-3, cap 512, no inter-chunk delay",
            "data_disconnect_flush": "not implemented in data_pipe (source audit)",
            "cmd_layer": "JSON line reassembly 512B; TX queue 8; 20ms inter-fragment; FIFO",
            "cmd_disconnect_flush": "line buffer + TX queue cleared",
        }
        result["pass"] = all(checks.values())
        return _finish(result)
    except Exception as exc:  # noqa: BLE001
        result["pass"] = False
        result["fatal"] = f"{type(exc).__name__}: {exc}"
        return _finish(result)
    finally:
        if cli is not None:
            await close_session(cli, sess)
        if not args.no_final_reset:
            reset_board(args.serial_port)


def _finish(result: dict[str, Any]) -> int:
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    path = RESULTS_DIR / f"t3_ble_channels_{ts}.json"
    result["finished_at"] = wall()
    result["_path"] = str(path)
    path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print("\n== T3 SUMMARY ==")
    print(json.dumps(sanitize(result), ensure_ascii=False, indent=2)[:10000])
    print(f"[results] {path}")
    return 0 if result.get("pass") else 1


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="T3 BLE CMD/DATA channel test")
    parser.add_argument("--ssid", required=True, help="visible AP for a wrong-password candidate")
    parser.add_argument("--wrong-password", default="t3-wrong-" + secrets.token_hex(6))
    parser.add_argument("--address")
    parser.add_argument("--user", default="admin")
    parser.add_argument("--user-password", default="123456")
    parser.add_argument("--serial-port", default="/dev/ttyUSB0")
    parser.add_argument("--max-write", type=int, default=20)
    parser.add_argument("--data-max-write", type=int, default=0,
                        help="single DATA write boundary probe; 0=host-reported MTU")
    parser.add_argument("--data-frame-size", type=int, default=200)
    parser.add_argument("--continuous-frames", type=int, default=10)
    parser.add_argument("--continuous-chunks", type=int, default=3)
    parser.add_argument("--burst-writes", type=int, default=32)
    parser.add_argument("--disconnect-burst", type=int, default=16)
    parser.add_argument("--concurrent-data-chunks", type=int, default=5)
    parser.add_argument("--long-command-size", type=int, default=275)
    parser.add_argument("--require-opget-during", action=argparse.BooleanOptionalAction,
                        default=True,
                        help="MTU23 acceptance requires operation.get during long response; "
                             "expected false at MTU256 because the long response is ~100ms")
    parser.add_argument("--no-reset", action="store_true")
    parser.add_argument("--no-final-reset", action="store_true")
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
