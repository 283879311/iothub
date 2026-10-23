#!/usr/bin/env python3
"""P0 BLE acceptance harness for IotHub (BlueZ central, bleak).

Simulates the mini-program's BLE behavior:
  1. scan with the NUS service UUID filter (6E400001-...);
  2. record what the advertisement / scan response exposes as the device name;
  3. login, then send 10 network.scan requests and reassemble every CMD TX
     notification into complete JSON (id-matched, no truncation);
  4. start an async v1 network.tryconnect, then send operation.get while a
     long response is still being fragmented;
  5. exercise duplicate requests, wrong password, apply/cancel, cancel race,
     and finally (only if P0 passes) the v0 config.set {"net_mode":"sta"}.

Credentials/tokens are never written to the result file. Run with:
    .venv-bleak/bin/python tools/ble_p0/p0_acceptance.py --ssid CMCC-502
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import random
import re
import secrets
import string
import subprocess
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

NUS_SVC = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
DATA_RX = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
DATA_TX = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
CMD_RX = "6E400004-B5A3-F393-E0A9-E50E24DCCA9E"
CMD_TX = "6E400005-B5A3-F393-E0A9-E50E24DCCA9E"
NAME_PREFIX = "IotHubBLE-"
SHORT_PREFIX = "IH-"
RATE_LIMIT_S = 0.25          # firmware: 5 commands/s, 200ms minimum
TERMINAL_STATES = {"success", "failed", "cancelled", "expired"}
RESULTS_DIR = Path(__file__).resolve().parent / "results"

_t0 = time.monotonic()


def now() -> float:
    return time.monotonic()


def stamp() -> str:
    return datetime.now().isoformat(timespec="milliseconds")


def mono(ts: float | None) -> str:
    return f"{(ts - _t0):.3f}s" if ts is not None else "-"


def mask_key(key: str, value: Any) -> Any:
    low = key.lower()
    if any(s in low for s in ("token", "password", "passwd", "secret")):
        if isinstance(value, bool):
            return value
        if isinstance(value, str) and value:
            return "<redacted>"
    return value


def sanitize(obj: Any) -> Any:
    if isinstance(obj, dict):
        return {k: mask_key(str(k), sanitize(v)) for k, v in obj.items()}
    if isinstance(obj, list):
        return [sanitize(v) for v in obj]
    return obj


def find_token(obj: Any) -> str | None:
    if isinstance(obj, dict):
        tok = obj.get("token")
        if isinstance(tok, str) and tok:
            return tok
        for value in obj.values():
            found = find_token(value)
            if found:
                return found
    elif isinstance(obj, list):
        for value in obj:
            found = find_token(value)
            if found:
                return found
    return None


def strip_ansi(text: str) -> str:
    return re.sub(r"\x1b\[[0-9;]*m", "", text)


def run_cmd(cmd: list[str], timeout: float = 15) -> tuple[int, str, str]:
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return proc.returncode, proc.stdout, proc.stderr
    except Exception as exc:  # noqa: BLE001
        return 1, "", str(exc)


def adv_record(dev: Any, adv: Any) -> dict[str, Any]:
    service_uuids = [str(u) for u in (adv.service_uuids or [])]
    service_data = {}
    for key, value in (adv.service_data or {}).items():
        service_data[str(key)] = bytes(value).hex()
    mfr = {}
    for key, value in (adv.manufacturer_data or {}).items():
        mfr[str(key)] = bytes(value).hex()
    return {
        "address": dev.address,
        "name": dev.name,
        "local_name": adv.local_name,
        "rssi": adv.rssi,
        "tx_power": adv.tx_power,
        "service_uuids": service_uuids,
        "service_data": service_data,
        "manufacturer_data": mfr,
        "connectable": getattr(adv, "connectable", None),
    }


def is_iot_record(rec: dict[str, Any]) -> bool:
    name = rec.get("name") or rec.get("local_name") or ""
    return name.startswith(NAME_PREFIX) or name.startswith(SHORT_PREFIX)


def choose_target(records: list[dict[str, Any]], address: str | None) -> dict[str, Any] | None:
    if address:
        for rec in records:
            if rec["address"].lower() == address.lower():
                return rec
    for rec in records:
        if (rec.get("name") or "").startswith(NAME_PREFIX):
            return rec
    for rec in records:
        if NUS_SVC.lower() in [u.lower() for u in rec.get("service_uuids", [])]:
            return rec
    return None


def clear_cached_iot_targets(verbose: bool = True) -> list[str]:
    """Drop cached IotHub entries so a scan proves a fresh over-the-air hit."""
    removed: list[str] = []
    rc, out, err = run_cmd(["bluetoothctl", "devices"], timeout=10)
    if rc != 0:
        if verbose:
            print(f"[scan] bluetoothctl devices failed: {err.strip()}")
        return removed
    for line in out.splitlines():
        m = re.match(r"Device ([0-9A-Fa-f:]{17}) (.*)", line.strip())
        if not m:
            continue
        addr, name = m.group(1), m.group(2).strip()
        if name.startswith(NAME_PREFIX) or name.startswith(SHORT_PREFIX):
            rc, out2, err2 = run_cmd(["bluetoothctl", "remove", addr], timeout=10)
            if verbose:
                print(f"[scan] remove cached {addr} {name} -> rc={rc} {strip_ansi(out2 + err2).strip()}")
            if rc == 0:
                removed.append(addr)
    return removed


async def scan_filtered(timeout: float) -> list[dict[str, Any]]:
    """BlueZ discovery filter with the NUS UUID (WeChat `services` equivalent)."""
    found = await BleakScanner.discover(
        timeout=timeout, service_uuids=[NUS_SVC], return_adv=True
    )
    records = [adv_record(dev, adv) for dev, adv in found.values()]
    print(f"[scan-filtered] {len(records)} hit(s) in {timeout:.0f}s")
    for rec in records:
        print(f"  HIT {rec['address']} name={rec['name']!r} local_name={rec['local_name']!r} "
              f"rssi={rec['rssi']} uuids={rec['service_uuids']}")
    return records


async def scan_unfiltered(timeout: float) -> list[dict[str, Any]]:
    latest: dict[str, dict[str, Any]] = {}

    def callback(dev: Any, adv: Any) -> None:
        latest[dev.address] = adv_record(dev, adv)

    scanner = BleakScanner(detection_callback=callback)
    await scanner.start()
    try:
        await asyncio.sleep(timeout)
    finally:
        await scanner.stop()
    records = list(latest.values())
    print(f"[scan-unfiltered] {len(records)} device(s) in {timeout:.0f}s")
    for rec in records:
        if is_iot_record(rec) or NUS_SVC.lower() in [u.lower() for u in rec["service_uuids"]]:
            print(f"  TARGET? {rec['address']} name={rec['name']!r} "
                  f"local_name={rec['local_name']!r} rssi={rec['rssi']} "
                  f"uuids={rec['service_uuids']}")
    return records


def capture_name_transition(address: str, timeout: float = 12) -> dict[str, Any]:
    """Observe ADV short name -> scan-response full name in bluetoothctl output."""
    rc, out, err = run_cmd(
        ["timeout", str(int(timeout + 4)), "bluetoothctl", "--timeout", str(int(timeout)), "scan on"],
        timeout=timeout + 6,
    )
    text = strip_ansi(out + err)
    relevant = []
    for line in text.splitlines():
        if address.lower() in line.lower() or NAME_PREFIX in line or SHORT_PREFIX in line:
            relevant.append(line.strip())
    names: list[str] = []
    for line in relevant:
        for pattern in (r"Device\s+([0-9A-Fa-f:]{17})\s+(.+)$", r"Name:\s*(.+)$"):
            m = re.search(pattern, line)
            if m:
                value = m.group(m.lastindex).strip()
                if value and value not in names:
                    names.append(value)
    print(f"[name-transition] rc={rc} lines={len(relevant)} names={names}")
    for line in relevant:
        print(f"  | {line}")
    return {"rc": rc, "lines": relevant, "names": names}


# ---------------------------------------------------------------------------
# JSON stream reassembly
# ---------------------------------------------------------------------------
@dataclass
class Response:
    obj: dict[str, Any]
    raw: bytes
    frag_sizes: list[int]
    rx_time: float
    prefix: bytes
    tx_time: float | None = None
    latency: float | None = None
    request_id: Any = None

    @property
    def raw_len(self) -> int:
        return len(self.raw)

    @property
    def message(self) -> str:
        return self.raw.decode("utf-8", "replace")


class JsonStream:
    """Accumulate CMD TX notifications, emit one complete JSON object at a time."""

    def __init__(self) -> None:
        self.buf = bytearray()
        self.frag_sizes: list[int] = []
        self.last_prefix = b""
        self.last_prefix_time = 0.0

    def feed(self, data: bytes) -> list[tuple[dict[str, Any], bytes, list[int], bytes, float]]:
        if not self.buf:
            self.last_prefix = bytes(data[:96])
            self.last_prefix_time = now()
        self.buf.extend(data)
        self.frag_sizes.append(len(data))
        emitted: list[tuple[dict[str, Any], bytes, list[int], bytes, float]] = []
        while True:
            item = self._pop()
            if item is None:
                break
            obj, raw = item
            emitted.append((obj, raw, list(self.frag_sizes), self.last_prefix, self.last_prefix_time))
            self.frag_sizes = []
        return emitted

    def _pop(self) -> tuple[dict[str, Any], bytes] | None:
        # Drop whitespace and anything before the first '{'.
        i = 0
        while i < len(self.buf) and self.buf[i] in b" \r\n\t":
            i += 1
        if i >= len(self.buf):
            self.buf.clear()
            return None
        if self.buf[i] != 0x7B:
            j = self.buf.find(b"{", i)
            if j < 0:
                self.buf.clear()
                return None
            del self.buf[:j]
        depth = 0
        in_str = False
        esc = False
        for i, byte in enumerate(self.buf):
            if in_str:
                if esc:
                    esc = False
                elif byte == 0x5C:  # backslash
                    esc = True
                elif byte == 0x22:  # quote
                    in_str = False
                continue
            if byte == 0x22:
                in_str = True
            elif byte == 0x7B:
                depth += 1
            elif byte == 0x7D:
                depth -= 1
                if depth == 0:
                    raw = bytes(self.buf[: i + 1])
                    del self.buf[: i + 1]
                    try:
                        obj = json.loads(raw)
                    except Exception as exc:  # noqa: BLE001
                        obj = {"_parse_error": str(exc), "_raw": raw.decode("utf-8", "replace")}
                    return obj, raw
        return None


class ConnectionLost(RuntimeError):
    pass


class Session:
    def __init__(
        self,
        cli: BleakClient,
        address: str | None = None,
        username: str = "admin",
        password: str = "123456",
        max_write: int = 20,
    ) -> None:
        self.cli = cli
        self.address = address or getattr(cli, "address", None)
        self.username = username
        self.password = password
        self.max_write = max(1, min(int(max_write), 512))
        self.cmd_notify_hook = None
        self.stream = JsonStream()
        self.pending: dict[Any, Response] = {}
        self.events: list[Response] = []
        self.responses: list[Response] = []
        self.nfy_count = 0
        self.nfy_sizes: list[int] = []
        self.tx_lock = asyncio.Lock()
        self.last_tx = 0.0
        self.seq = 0
        self.token: str | None = None
        self.token_time = 0.0
        self.log: list[str] = []
        self.mtu = getattr(cli, "mtu_size", None)
        self.reconnect_count = 0
        self.reconnect_lock = asyncio.Lock()
        self._reconnecting = False

    def is_connected(self) -> bool:
        return self.cli is not None and bool(getattr(self.cli, "is_connected", False))

    def record(self, text: str) -> None:
        self.log.append(text)
        print(text)

    def on_notify(self, _char: Any, data: bytearray) -> None:
        if self.cmd_notify_hook is not None:
            self.cmd_notify_hook(bytes(data), now())
        self.nfy_count += 1
        self.nfy_sizes.append(len(data))
        for obj, raw, frag_sizes, prefix, prefix_time in self.stream.feed(bytes(data)):
            res = Response(
                obj=obj,
                raw=raw,
                frag_sizes=frag_sizes,
                rx_time=now(),
                prefix=prefix,
            )
            self.responses.append(res)
            if isinstance(obj, dict) and "id" in obj:
                self.pending[obj["id"]] = res
            else:
                self.events.append(res)
            body = json.dumps(sanitize(obj), ensure_ascii=False)
            self.record(
                f"<{mono(res.rx_time)}> RX id={obj.get('id') if isinstance(obj, dict) else None} "
                f"{len(raw)}B frags={len(frag_sizes)} sizes={frag_sizes} {body[:1200]}"
            )

    def next_seq(self) -> int:
        self.seq += 1
        return self.seq

    async def _write(self, payload: bytes) -> float:
        # CMD RX accepts short writes and reassembles a JSON line up to 512B.
        # Default to 20B: safe at MTU 23 (ATT payload 20B) and on Android
        # WeChat, which may not negotiate a larger MTU. The 275B config.set
        # must be chunked by the client, exactly as the mini-program does.
        max_write = self.max_write
        chunks = [payload[i:i + max_write] for i in range(0, len(payload), max_write)]
        async with self.tx_lock:
            if not self.is_connected():
                raise ConnectionLost("not connected before write")
            wait = RATE_LIMIT_S - (now() - self.last_tx)
            if wait > 0:
                await asyncio.sleep(wait)
            tx_time = now()
            if len(chunks) > 1:
                self.record(
                    f"[tx] chunked {len(payload)}B into {len(chunks)} GATT writes "
                    f"(<= {max_write}B each)"
                )
            for index, chunk in enumerate(chunks):
                if not self.is_connected():
                    raise ConnectionLost("disconnected between command chunks")
                try:
                    await self.cli.write_gatt_char(CMD_RX, chunk, response=True)
                except Exception as exc:  # noqa: BLE001
                    if not self.is_connected():
                        raise ConnectionLost(f"write failed after disconnect: {exc}") from exc
                    self.record(
                        f"[tx] chunk {index + 1}/{len(chunks)} write-with-response "
                        f"failed ({exc}); retrying without response"
                    )
                    try:
                        await self.cli.write_gatt_char(CMD_RX, chunk, response=False)
                    except Exception as exc2:  # noqa: BLE001
                        if not self.is_connected():
                            raise ConnectionLost(
                                f"write-without-response failed after disconnect: {exc2}"
                            ) from exc2
                        raise
                if index + 1 < len(chunks):
                    await asyncio.sleep(0.02)
            self.last_tx = now()
            return tx_time

    async def wait_rate_gap(self) -> None:
        while True:
            async with self.tx_lock:
                wait = RATE_LIMIT_S - (now() - self.last_tx)
                if wait <= 0:
                    return
            await asyncio.sleep(wait)

    async def _wait_response(self, rid: Any, timeout: float) -> Response | None:
        deadline = now() + timeout
        while now() < deadline:
            res = self.pending.get(rid)
            if res is not None:
                return self.pending.pop(rid)
            if not self.is_connected():
                raise ConnectionLost("disconnected while waiting for response")
            await asyncio.sleep(0.005)
        return None

    async def send_request(
        self,
        req: dict[str, Any],
        timeout: float = 20,
        retries: int = 3,
        allow_relogin: bool = True,
        allow_reconnect: bool = True,
    ) -> Response:
        rid = req["id"]
        for attempt in range(retries + 1):
            payload = (json.dumps(req, ensure_ascii=False, separators=(",", ":")) + "\n").encode()
            self.record(f">{mono(now())} TX id={rid} {len(payload)}B "
                        f"{json.dumps(sanitize(req), ensure_ascii=False)[:800]}")
            try:
                tx_time = await self._write(payload)
                res = await self._wait_response(rid, timeout)
            except (ConnectionLost, BleakError) as exc:
                if allow_reconnect and not self._reconnecting and attempt < retries:
                    self.record(
                        f"[conn] link lost during id={rid} ({type(exc).__name__}: {exc}); "
                        "reconnecting and retrying"
                    )
                    await self.reconnect()
                    req["token"] = self.token
                    continue
                raise
            if res is None:
                if not self.is_connected():
                    if allow_reconnect and not self._reconnecting and attempt < retries:
                        self.record("[conn] response timeout after disconnect; reconnecting")
                        await self.reconnect()
                        req["token"] = self.token
                        continue
                    raise ConnectionLost(f"no response for id={rid} after disconnect")
                raise TimeoutError(f"no complete JSON for id={rid} within {timeout}s")
            res.request_id = rid
            res.tx_time = tx_time
            res.latency = res.rx_time - tx_time
            code = response_error_code(res.obj)
            if code == "rate_limited" and attempt < retries:
                self.record("[tx] rate_limited; retrying after 350ms")
                await asyncio.sleep(0.35)
                continue
            if code == "unauthorized" and allow_relogin and attempt < retries:
                self.record("[auth] token rejected; re-login and retry")
                await self.login()
                req["token"] = self.token
                continue
            return res
        raise RuntimeError(f"request id={rid} failed after retries")

    async def _connect_once(self) -> None:
        if self.cli is not None:
            try:
                await self.cli.disconnect()
            except Exception:  # noqa: BLE001
                pass
        cli = BleakClient(self.address, timeout=25)
        await cli.connect()
        self.cli = cli
        self.stream = JsonStream()
        self.pending.clear()
        self.events.clear()
        self.nfy_sizes = []
        self.last_tx = 0.0
        self.token = None
        self.mtu = getattr(cli, "mtu_size", None)
        await cli.start_notify(CMD_TX, self.on_notify)
        await asyncio.sleep(0.2)
        await self.login(self.username, self.password)

    async def _rescan_address(self) -> None:
        try:
            hits = await BleakScanner.discover(
                timeout=6, service_uuids=[NUS_SVC], return_adv=True
            )
            for addr, (dev, adv) in hits.items():
                if (dev.name or "").startswith(NAME_PREFIX) or NUS_SVC.lower() in [
                    str(u).lower() for u in (adv.service_uuids or [])
                ]:
                    if addr != self.address:
                        self.record(f"[conn] target moved: {self.address} -> {addr}")
                    self.address = addr
                    return
        except Exception as exc:  # noqa: BLE001
            self.record(f"[conn] rescan failed: {exc}")

    async def reconnect(self) -> None:
        async with self.reconnect_lock:
            if self.is_connected():
                return
            self.reconnect_count += 1
            self._reconnecting = True
            self.record(f"[conn] reconnect #{self.reconnect_count} starting")
            try:
                for attempt in range(4):
                    try:
                        await self._connect_once()
                        self.record(f"[conn] reconnect #{self.reconnect_count} ok")
                        return
                    except Exception as exc:  # noqa: BLE001
                        self.record(
                            f"[conn] reconnect attempt {attempt + 1}/4 failed: "
                            f"{type(exc).__name__}: {exc}"
                        )
                        await asyncio.sleep(1.5)
                        await self._rescan_address()
            finally:
                self._reconnecting = False
            raise ConnectionError("reconnect failed after 4 attempts")

    async def close(self) -> None:
        try:
            if self.cli is not None:
                await self.cli.disconnect()
        except Exception:  # noqa: BLE001
            pass

    async def v1(
        self,
        cmd: str,
        args: dict[str, Any] | None = None,
        timeout: float = 20,
        token: bool = True,
        id_prefix: str | None = None,
    ) -> Response:
        rid = f"{id_prefix or cmd}-{self.next_seq()}"
        req: dict[str, Any] = {"id": rid, "version": "1.0", "cmd": cmd, "args": args or {}}
        if token and self.token:
            req["token"] = self.token
        return await self.send_request(req, timeout=timeout)

    async def v0(
        self,
        method: str,
        params: dict[str, Any],
        req_id: int | str = 75,
        timeout: float = 20,
    ) -> Response:
        req: dict[str, Any] = {"id": req_id, "method": method, "params": params}
        if self.token:
            req["token"] = self.token
        return await self.send_request(req, timeout=timeout)

    async def login(
        self, username: str | None = None, password: str | None = None
    ) -> Response:
        self.username = username or self.username
        self.password = password or self.password
        rid = f"login-{self.next_seq()}"
        req = {
            "id": rid,
            "version": "1.0",
            "cmd": "auth.login",
            "args": {"username": self.username, "password": self.password},
        }
        res = await self.send_request(
            req, timeout=10, retries=1, allow_relogin=False, allow_reconnect=False
        )
        token = find_token(res.obj)
        if not token:
            raise RuntimeError(f"login failed: {json.dumps(sanitize(res.obj), ensure_ascii=False)}")
        self.token = token
        self.token_time = now()
        self.record("[auth] login ok (token redacted)")
        return res

    async def wait_for_prefix(self, prefix: bytes, start_count: int, timeout: float = 5) -> bool:
        deadline = now() + timeout
        while now() < deadline:
            if self.nfy_count > start_count and self.stream.last_prefix.startswith(prefix):
                return True
            await asyncio.sleep(0.005)
        return False

    async def poll_operation(
        self,
        operation_id: str,
        timeout: float = 25,
        interval: float = 0.5,
        stop_states: set[str] | None = None,
    ) -> tuple[dict[str, Any], float, dict[str, Any]]:
        started = now()
        last: dict[str, Any] = {}
        first: dict[str, Any] = {}
        stops = stop_states or TERMINAL_STATES
        while now() - started < timeout:
            res = await self.v1(
                "operation.get",
                {"operation_id": operation_id},
                timeout=10,
                id_prefix="opget-poll",
            )
            obj = res.obj
            data = obj.get("data") if isinstance(obj.get("data"), dict) else {}
            if not first:
                first = data
            last = data
            state = data.get("state")
            if state in stops:
                return data, now() - started, first
            await asyncio.sleep(interval)
        return last, now() - started, first


def response_error_code(obj: dict[str, Any]) -> str | None:
    if not isinstance(obj, dict):
        return None
    err = obj.get("error")
    if isinstance(err, dict):
        code = err.get("code")
        return str(code) if code is not None else None
    if isinstance(err, str):
        return err
    return None


def response_error_text(obj: dict[str, Any]) -> str | None:
    if not isinstance(obj, dict):
        return None
    err = obj.get("error")
    if isinstance(err, dict):
        return json.dumps(sanitize(err), ensure_ascii=False)
    if isinstance(err, str):
        return err
    return None


def pass_fail(ok: bool) -> str:
    return "PASS" if ok else "FAIL"


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
async def test_scan_and_name(args: argparse.Namespace) -> dict[str, Any]:
    result: dict[str, Any] = {"name": "1-2_SCAN_FILTER_AND_NAME", "pass": False}
    clear_cached_iot_targets()

    filtered = await scan_filtered(args.scan_timeout)
    result["filtered_hits"] = filtered
    target = choose_target(filtered, args.address)
    result["target"] = target
    if target is None:
        result["reason"] = "NUS service filter returned no IotHub device"
        return result

    unfiltered = await scan_unfiltered(6)
    result["unfiltered"] = [
        rec for rec in unfiltered
        if is_iot_record(rec) or NUS_SVC.lower() in [u.lower() for u in rec["service_uuids"]]
    ]
    target_addr = target["address"]
    # Drop the cache entry the filtered scan just created, so bluetoothctl can
    # show the name transition from ADV short name to scan-response full name.
    run_cmd(["bluetoothctl", "remove", target_addr], timeout=10)
    result["name_transition"] = capture_name_transition(target_addr, timeout=12)

    names = result["name_transition"]["names"]
    full_name = target.get("name") or target.get("local_name") or ""
    result["bluez_name"] = full_name
    result["adv_has_nus_uuid"] = NUS_SVC.lower() in [u.lower() for u in target["service_uuids"]]
    result["short_name_seen"] = any(n.startswith(SHORT_PREFIX) for n in names) or any(
        n.startswith(SHORT_PREFIX) for n in [target.get("name") or ""]
    )
    result["full_name_seen"] = full_name.startswith(NAME_PREFIX) or any(
        n.startswith(NAME_PREFIX) for n in names
    )
    # The service-filter hit + NUS UUID in the advertisement is the P0 requirement.
    # The actual iOS/Android UI cannot be exercised from this BlueZ host; record
    # the over-the-air name evidence and flag the phone part as unverified.
    result["pass"] = bool(result["adv_has_nus_uuid"] and result["full_name_seen"])
    result["phone_ui_verified"] = False
    result["note"] = (
        "BlueZ host: filtered scan hit and full name observed. iOS/Android WeChat "
        "UI was NOT tested on a phone; short name in ADV is recorded above."
    )
    return result


async def test_long_responses(sess: Session, n: int = 10) -> dict[str, Any]:
    result: dict[str, Any] = {
        "name": "3_LONG_RESPONSES_10X",
        "pass": False,
        "runs": [],
        "disconnects": [],
    }
    start_reconnects = sess.reconnect_count
    for i in range(n):
        rid_prefix = f"scan{i + 1}"
        res: Response | None = None
        attempts = 0
        last_error = ""
        while attempts < 3:
            attempts += 1
            try:
                res = await sess.v1("network.scan", {}, timeout=30, id_prefix=rid_prefix)
                break
            except Exception as exc:  # noqa: BLE001
                last_error = f"{type(exc).__name__}: {exc}"
                result["disconnects"].append(
                    {"i": i + 1, "attempt": attempts, "error": last_error}
                )
                sess.record(f"[long {i + 1}/{n}] attempt {attempts} failed: {last_error}")
                try:
                    await sess.reconnect()
                except Exception as rexc:  # noqa: BLE001
                    last_error = f"reconnect: {type(rexc).__name__}: {rexc}"
                    result["disconnects"].append(
                        {"i": i + 1, "attempt": attempts, "error": last_error}
                    )
                    break
        if res is None:
            result["runs"].append(
                {"i": i + 1, "failed": True, "attempts": attempts, "error": last_error}
            )
            continue
        obj = res.obj
        data = obj.get("data") if isinstance(obj.get("data"), dict) else {}
        aps = data.get("aps") if isinstance(data.get("aps"), list) else []
        run = {
            "i": i + 1,
            "requested_id": res.request_id,
            "response_id": obj.get("id"),
            "json_complete": "_parse_error" not in obj,
            "ok": obj.get("ok") is True,
            "error": response_error_text(obj),
            "count": data.get("count"),
            "aps": len(aps),
            "ssids": [ap.get("ssid") for ap in aps if isinstance(ap, dict)][:60],
            "bytes": res.raw_len,
            "frags": len(res.frag_sizes),
            "frag_sizes": res.frag_sizes,
            "latency_ms": round((res.latency or 0) * 1000, 1),
            "attempts": attempts,
        }
        result["runs"].append(run)
        sess.record(
            f"[long {i + 1}/{n}] complete={run['json_complete']} id={run['response_id']} "
            f"ok={run['ok']} bytes={run['bytes']} frags={run['frags']} "
            f"aps={run['aps']} latency={run['latency_ms']}ms attempts={attempts}"
        )
        await asyncio.sleep(0.25)

    runs = result["runs"]
    result["reconnects_total"] = sess.reconnect_count - start_reconnects
    result["count"] = len(runs)
    result["all_complete"] = len(runs) == n and all(
        r.get("json_complete") and r.get("response_id") == r.get("requested_id") for r in runs
    )
    result["all_scan_ok"] = len(runs) == n and all(r.get("ok") for r in runs)
    result["all_ids_match"] = len(runs) == n and all(
        r.get("response_id") == r.get("requested_id") for r in runs
    )
    result["all_multi_fragment"] = bool(runs) and all(r.get("frags", 0) > 1 for r in runs)
    result["pass"] = result["all_complete"]
    if runs:
        result["bytes_min"] = min(r["bytes"] for r in runs)
        result["bytes_max"] = max(r["bytes"] for r in runs)
        result["frags_max"] = max(r["frags"] for r in runs)
    return result


async def test_extra_fragmentation(sess: Session, n: int = 10) -> dict[str, Any]:
    """Same reassembly stress with config.get, which is reliably long."""
    result: dict[str, Any] = {
        "name": "3b_CONFIG_GET_10X",
        "pass": False,
        "runs": [],
        "disconnects": [],
    }
    start_reconnects = sess.reconnect_count
    for i in range(n):
        res: Response | None = None
        attempts = 0
        last_error = ""
        while attempts < 3:
            attempts += 1
            try:
                res = await sess.v1("config.get", {}, timeout=20, id_prefix=f"cfg{i + 1}")
                break
            except Exception as exc:  # noqa: BLE001
                last_error = f"{type(exc).__name__}: {exc}"
                result["disconnects"].append(
                    {"i": i + 1, "attempt": attempts, "error": last_error}
                )
                sess.record(f"[cfg {i + 1}/{n}] attempt {attempts} failed: {last_error}")
                try:
                    await sess.reconnect()
                except Exception as rexc:  # noqa: BLE001
                    last_error = f"reconnect: {type(rexc).__name__}: {rexc}"
                    result["disconnects"].append(
                        {"i": i + 1, "attempt": attempts, "error": last_error}
                    )
                    break
        if res is None:
            result["runs"].append(
                {"i": i + 1, "failed": True, "attempts": attempts, "error": last_error}
            )
            continue
        obj = res.obj
        run = {
            "i": i + 1,
            "requested_id": res.request_id,
            "response_id": obj.get("id"),
            "json_complete": "_parse_error" not in obj,
            "ok": obj.get("ok") is True,
            "error": response_error_text(obj),
            "bytes": res.raw_len,
            "frags": len(res.frag_sizes),
            "frag_sizes": res.frag_sizes,
            "latency_ms": round((res.latency or 0) * 1000, 1),
            "attempts": attempts,
        }
        result["runs"].append(run)
        sess.record(
            f"[cfg {i + 1}/{n}] complete={run['json_complete']} id={run['response_id']} "
            f"bytes={run['bytes']} frags={run['frags']} latency={run['latency_ms']}ms "
            f"attempts={attempts}"
        )
        await asyncio.sleep(0.25)
    runs = result["runs"]
    result["reconnects_total"] = sess.reconnect_count - start_reconnects
    result["all_complete"] = len(runs) == n and all(
        r.get("json_complete") and r.get("response_id") == r.get("requested_id") for r in runs
    )
    result["pass"] = result["all_complete"]
    if runs:
        result["bytes_min"] = min(r["bytes"] for r in runs)
        result["bytes_max"] = max(r["bytes"] for r in runs)
        result["frags_max"] = max(r["frags"] for r in runs)
    return result


async def test_operation_during_fragmentation(
    sess: Session, ssid: str, password: str
) -> dict[str, Any]:
    result: dict[str, Any] = {"name": "4_OPERATION_GET_DURING_FRAGMENTATION", "pass": False}
    key = "p0tc-" + secrets.token_hex(6)
    tc_id = f"tc-{sess.next_seq()}"
    tc_req = {
        "id": tc_id,
        "version": "1.0",
        "cmd": "network.tryconnect",
        "args": {"ssid": ssid, "password": password, "idempotency_key": key},
        "token": sess.token,
    }
    tc = await sess.send_request(tc_req, timeout=15)
    data = tc.obj.get("data") if isinstance(tc.obj.get("data"), dict) else {}
    op_id = data.get("operation_id")
    result["tryconnect"] = {
        "id": tc.obj.get("id"),
        "ok": tc.obj.get("ok") is True,
        "operation_id": op_id,
        "state": data.get("state"),
        "error": response_error_text(tc.obj),
        "latency_ms": round((tc.latency or 0) * 1000, 1),
    }
    sess.record(f"[op] tryconnect accepted op={op_id} state={data.get('state')}")
    if not op_id:
        result["reason"] = "tryconnect did not return operation_id"
        return result

    # Duplicate tryconnect with the same idempotency key: must return the same op.
    await asyncio.sleep(0.25)
    dup_same = await sess.v1(
        "network.tryconnect",
        {"ssid": ssid, "password": password, "idempotency_key": key},
        timeout=10,
        id_prefix="tc-dup-same",
    )
    dup_same_data = dup_same.obj.get("data") if isinstance(dup_same.obj.get("data"), dict) else {}
    result["duplicate_same_key"] = {
        "ok": dup_same.obj.get("ok") is True,
        "operation_id": dup_same_data.get("operation_id"),
        "state": dup_same_data.get("state"),
        "error": response_error_text(dup_same.obj),
        "same_operation": dup_same_data.get("operation_id") == op_id,
    }

    # A different key while the first candidate is active must be busy.
    await asyncio.sleep(0.25)
    dup_diff = await sess.v1(
        "network.tryconnect",
        {"ssid": ssid, "password": password, "idempotency_key": "p0tc-" + secrets.token_hex(6)},
        timeout=10,
        id_prefix="tc-dup-diff",
    )
    diff_data = dup_diff.obj.get("data") if isinstance(dup_diff.obj.get("data"), dict) else {}
    result["duplicate_different_key"] = {
        "ok": dup_diff.obj.get("ok") is True,
        "error_code": response_error_code(dup_diff.obj),
        "state": diff_data.get("state"),
        "operation_id": diff_data.get("operation_id"),
    }
    # If the first candidate had already finished, the different key may have
    # started a second operation; cancel it so it cannot interfere.
    diff_op_id = diff_data.get("operation_id")
    if dup_diff.obj.get("ok") is True and diff_op_id and diff_op_id != op_id:
        sess.record(f"[op] duplicate-different-key started second op {diff_op_id}; cancelling it")
        await asyncio.sleep(0.25)
        await sess.v1(
            "network.cancel",
            {"operation_id": diff_op_id},
            timeout=10,
            id_prefix="tc-dup-diff-cancel",
        )

    # Long response + operation.get while the long response is still arriving.
    long_id = f"long-{sess.next_seq()}"
    long_req = {
        "id": long_id,
        "version": "1.0",
        "cmd": "config.get",
        "args": {},
        "token": sess.token,
    }
    start_nfy = sess.nfy_count
    long_task = asyncio.create_task(sess.send_request(long_req, timeout=30))
    prefix_ok = await sess.wait_for_prefix(
        ('{"id":"' + long_id + '"').encode(), start_nfy, timeout=5
    )
    result["long_response_started"] = prefix_ok
    first_frag_time = sess.stream.last_prefix_time if prefix_ok else None
    await sess.wait_rate_gap()
    opget_id = f"opget-{sess.next_seq()}"
    opget_req = {
        "id": opget_id,
        "version": "1.0",
        "cmd": "operation.get",
        "args": {"operation_id": op_id},
        "token": sess.token,
    }
    opget_task = asyncio.create_task(sess.send_request(opget_req, timeout=30))
    long_res, opget_res = await asyncio.gather(long_task, opget_task)
    long_data = long_res.obj.get("data") if isinstance(long_res.obj.get("data"), dict) else {}
    opget_data = opget_res.obj.get("data") if isinstance(opget_res.obj.get("data"), dict) else {}
    result["fragmentation"] = {
        "long_id": long_res.obj.get("id"),
        "long_ok": long_res.obj.get("ok") is True,
        "long_bytes": long_res.raw_len,
        "long_frags": len(long_res.frag_sizes),
        "long_frag_sizes": long_res.frag_sizes,
        "long_latency_ms": round((long_res.latency or 0) * 1000, 1),
        "first_fragment_at_s": round(first_frag_time - _t0, 3) if first_frag_time else None,
        "opget_sent_at_s": round((opget_res.tx_time or 0) - _t0, 3),
        "opget_rx_at_s": round(opget_res.rx_time - _t0, 3),
        "opget_latency_ms": round((opget_res.latency or 0) * 1000, 1),
        "opget_id": opget_res.obj.get("id"),
        "opget_state": opget_data.get("state"),
        "opget_error": response_error_code(opget_res.obj),
        "opget_data": sanitize(opget_data),
        "sent_before_long_complete": bool(
            opget_res.tx_time is not None and opget_res.tx_time < long_res.rx_time
        ),
        "opget_before_long_complete": opget_res.rx_time < long_res.rx_time,
    }
    result["opget"] = result["fragmentation"]

    # Poll until got_ip/terminal to know the tryconnect outcome and duration.
    terminal, elapsed, first = await sess.poll_operation(
        op_id, timeout=25, stop_states=TERMINAL_STATES | {"got_ip"}
    )
    result["terminal"] = {
        "state": terminal.get("state"),
        "error": terminal.get("error"),
        "data": sanitize(terminal),
        "elapsed_s": round(elapsed, 3),
        "first_poll": sanitize(first),
    }
    result["tryconnect_total_before_opget_s"] = round(
        ((opget_res.rx_time or 0) - tc.rx_time), 3
    )
    opget_state = result["fragmentation"]["opget_data"].get("state")
    result["pass"] = bool(
        result["tryconnect"]["ok"]
        and opget_state in {"accepted", "connecting", "got_ip"}
        and (opget_res.rx_time - tc.rx_time) < 15.0
    )
    result["note"] = (
        "Pass = operation.get returned a state before the 15s tryconnect could finish; "
        "the operation itself may still be connecting at that moment."
    )

    # apply / cancel contract for this operation
    await asyncio.sleep(0.25)
    apply_res = await sess.v1(
        "network.apply", {"operation_id": op_id}, timeout=15, id_prefix="apply"
    )
    result["apply"] = {
        "ok": apply_res.obj.get("ok") is True,
        "state": (apply_res.obj.get("data") or {}).get("state")
        if isinstance(apply_res.obj.get("data"), dict) else None,
        "error_code": response_error_code(apply_res.obj),
        "error": response_error_text(apply_res.obj),
        "data": sanitize(apply_res.obj.get("data")),
    }
    await asyncio.sleep(0.25)
    apply_dup = await sess.v1(
        "network.apply", {"operation_id": op_id}, timeout=15, id_prefix="apply-dup"
    )
    result["apply_duplicate"] = {
        "ok": apply_dup.obj.get("ok") is True,
        "state": (apply_dup.obj.get("data") or {}).get("state")
        if isinstance(apply_dup.obj.get("data"), dict) else None,
        "error_code": response_error_code(apply_dup.obj),
    }
    await asyncio.sleep(0.25)
    cancel_res = await sess.v1(
        "network.cancel", {"operation_id": op_id}, timeout=15, id_prefix="cancel"
    )
    result["cancel"] = {
        "ok": cancel_res.obj.get("ok") is True,
        "state": (cancel_res.obj.get("data") or {}).get("state")
        if isinstance(cancel_res.obj.get("data"), dict) else None,
        "error_code": response_error_code(cancel_res.obj),
        "error": response_error_text(cancel_res.obj),
        "data": sanitize(cancel_res.obj.get("data")),
    }
    await asyncio.sleep(0.25)
    cancel_dup = await sess.v1(
        "network.cancel", {"operation_id": op_id}, timeout=15, id_prefix="cancel-dup"
    )
    result["cancel_duplicate"] = {
        "ok": cancel_dup.obj.get("ok") is True,
        "state": (cancel_dup.obj.get("data") or {}).get("state")
        if isinstance(cancel_dup.obj.get("data"), dict) else None,
        "error_code": response_error_code(cancel_dup.obj),
    }
    return result


async def wait_wifi_ready(sess: Session, timeout: float = 20) -> dict[str, Any]:
    """Wait until STA has an IP after apply/candidate teardown."""
    started = now()
    last: dict[str, Any] = {}
    while now() - started < timeout:
        try:
            res = await sess.v1("network.status", {}, timeout=10, id_prefix="wifi-ready")
            data = res.obj.get("data") if isinstance(res.obj.get("data"), dict) else {}
            last = data
            sess.record(
                f"[wifi] ready-check ssid={data.get('sta_ssid')!r} "
                f"connected={data.get('sta_connected')} ip={data.get('sta_ip')!r}"
            )
            ip = str(data.get("sta_ip") or "")
            if data.get("sta_has_ip") and ip not in ("", "0.0.0.0"):
                return {
                    "ok": True,
                    "elapsed_s": round(now() - started, 3),
                    "status": sanitize(data),
                }
        except Exception as exc:  # noqa: BLE001
            sess.record(f"[wifi] ready-check error: {exc}")
        await asyncio.sleep(1.0)
    return {
        "ok": False,
        "elapsed_s": round(now() - started, 3),
        "status": sanitize(last),
    }


async def test_error_password(sess: Session, ssid: str, wrong_password: str) -> dict[str, Any]:
    result: dict[str, Any] = {"name": "5a_WRONG_PASSWORD", "pass": False}
    key = "p0wrong-" + secrets.token_hex(6)
    tc = await sess.v1(
        "network.tryconnect",
        {"ssid": ssid, "password": wrong_password, "idempotency_key": key},
        timeout=15,
        id_prefix="wrongpw",
    )
    data = tc.obj.get("data") if isinstance(tc.obj.get("data"), dict) else {}
    op_id = data.get("operation_id")
    result["tryconnect"] = {
        "ok": tc.obj.get("ok") is True,
        "operation_id": op_id,
        "state": data.get("state"),
        "error": response_error_text(tc.obj),
    }
    if not op_id:
        result["reason"] = "tryconnect did not return operation_id"
        return result
    terminal, elapsed, first = await sess.poll_operation(
        op_id, timeout=25, stop_states=TERMINAL_STATES | {"got_ip"}
    )
    result["terminal"] = {
        "state": terminal.get("state"),
        "error": terminal.get("error"),
        "data": sanitize(terminal),
        "elapsed_s": round(elapsed, 3),
        "first_poll": sanitize(first),
    }
    # A hidden/absent AP also ends in state=failed, but that is not a
    # wrong-password result. Require an authentication/connect failure code.
    result["pass"] = bool(
        terminal.get("state") == "failed"
        and terminal.get("error") in {"wrong_password", "connect_failed", "timeout"}
    )
    await asyncio.sleep(0.25)
    cancel = await sess.v1(
        "network.cancel", {"operation_id": op_id}, timeout=10, id_prefix="wrongpw-cancel"
    )
    result["cancel_after_failed"] = {
        "ok": cancel.obj.get("ok") is True,
        "state": (cancel.obj.get("data") or {}).get("state")
        if isinstance(cancel.obj.get("data"), dict) else None,
        "error_code": response_error_code(cancel.obj),
    }
    return result


async def test_cancel_race(sess: Session, ssid: str, wrong_password: str, rounds: int = 2) -> dict[str, Any]:
    result: dict[str, Any] = {"name": "5b_CANCEL_RACE", "pass": False, "rounds": []}
    for i in range(rounds):
        key = "p0race-" + secrets.token_hex(6)
        tc = await sess.v1(
            "network.tryconnect",
            {"ssid": ssid, "password": wrong_password, "idempotency_key": key},
            timeout=15,
            id_prefix=f"race{i + 1}",
        )
        data = tc.obj.get("data") if isinstance(tc.obj.get("data"), dict) else {}
        op_id = data.get("operation_id")
        if not op_id:
            result["rounds"].append({"i": i + 1, "tryconnect": sanitize(tc.obj)})
            continue
        await asyncio.sleep(0.25)
        cancel = await sess.v1(
            "network.cancel", {"operation_id": op_id}, timeout=10, id_prefix=f"race{i + 1}-cancel"
        )
        cancel_data = cancel.obj.get("data") if isinstance(cancel.obj.get("data"), dict) else {}
        terminal, elapsed, _ = await sess.poll_operation(
            op_id, timeout=20, interval=0.4, stop_states=TERMINAL_STATES | {"got_ip"}
        )
        await asyncio.sleep(0.25)
        cancel_dup = await sess.v1(
            "network.cancel",
            {"operation_id": op_id},
            timeout=10,
            id_prefix=f"race{i + 1}-cancel-dup",
        )
        row = {
            "i": i + 1,
            "operation_id": op_id,
            "cancel_ok": cancel.obj.get("ok") is True,
            "cancel_state": cancel_data.get("state"),
            "cancel_error": response_error_text(cancel.obj),
            "terminal_state": terminal.get("state"),
            "terminal_error": terminal.get("error"),
            "terminal_elapsed_s": round(elapsed, 3),
            "cancel_duplicate_ok": cancel_dup.obj.get("ok") is True,
            "cancel_duplicate_state": (cancel_dup.obj.get("data") or {}).get("state")
            if isinstance(cancel_dup.obj.get("data"), dict) else None,
            "cancel_duplicate_error": response_error_text(cancel_dup.obj),
        }
        row["pass"] = terminal.get("state") in ("cancelled", "failed") and (
            cancel.obj.get("ok") is True or response_error_code(cancel.obj) == "operation_state"
        )
        result["rounds"].append(row)
        sess.record(
            f"[race {i + 1}] cancel={row['cancel_state'] or row['cancel_error']} "
            f"terminal={row['terminal_state']} elapsed={row['terminal_elapsed_s']}s "
            f"dup={row['cancel_duplicate_state'] or row['cancel_duplicate_error']}"
        )
        await asyncio.sleep(0.25)
    result["pass"] = bool(result["rounds"]) and all(r.get("pass") for r in result["rounds"])
    return result


async def test_not_found(sess: Session) -> dict[str, Any]:
    result: dict[str, Any] = {"name": "5c_NOT_FOUND", "pass": False}
    bogus = "op-does-not-exist-" + secrets.token_hex(3)
    get_res = await sess.v1(
        "operation.get", {"operation_id": bogus}, timeout=10, id_prefix="nf-get"
    )
    apply_res = await sess.v1(
        "network.apply", {"operation_id": bogus}, timeout=10, id_prefix="nf-apply"
    )
    cancel_res = await sess.v1(
        "network.cancel", {"operation_id": bogus}, timeout=10, id_prefix="nf-cancel"
    )
    result["operation_get"] = {
        "ok": get_res.obj.get("ok") is True,
        "error_code": response_error_code(get_res.obj),
        "error": response_error_text(get_res.obj),
    }
    result["apply"] = {
        "ok": apply_res.obj.get("ok") is True,
        "error_code": response_error_code(apply_res.obj),
        "error": response_error_text(apply_res.obj),
    }
    result["cancel"] = {
        "ok": cancel_res.obj.get("ok") is True,
        "error_code": response_error_code(cancel_res.obj),
        "error": response_error_text(cancel_res.obj),
    }
    result["pass"] = all(
        response_error_code(r.obj) == "not_found"
        for r in (get_res, apply_res, cancel_res)
    )
    return result


async def test_final_config_set(sess: Session, args: argparse.Namespace) -> dict[str, Any]:
    result: dict[str, Any] = {"name": "6_FINAL_CONFIG_SET_NET_MODE_STA", "pass": False}
    if now() - sess.token_time > 500:
        await sess.login(args.user, args.password)
    res = await sess.v0("config.set", {"net_mode": "sta"}, req_id=75, timeout=20)
    result["response"] = sanitize(res.obj)
    result["ok"] = res.obj.get("ok") is True
    result["latency_ms"] = round((res.latency or 0) * 1000, 1)
    result["pass"] = result["ok"]
    sess.record(f"[final] config.set net_mode=sta -> {json.dumps(sanitize(res.obj), ensure_ascii=False)}")
    await asyncio.sleep(2.0)
    result["still_connected_after_2s"] = sess.cli.is_connected
    if sess.cli.is_connected:
        try:
            verify = await sess.v1("config.get", {"domains": ["network"]}, timeout=10)
            data = verify.obj.get("data") if isinstance(verify.obj.get("data"), dict) else {}
            network = data.get("network") if isinstance(data.get("network"), dict) else {}
            result["verify_net_mode"] = network.get("net_mode")
        except Exception as exc:  # noqa: BLE001
            result["verify_error"] = str(exc)
    return result


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
async def run(args: argparse.Namespace) -> int:
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    summary: dict[str, Any] = {
        "timestamp": stamp(),
        "target_address_arg": args.address,
        "ssid_arg": args.ssid,
        "tests": {},
    }

    scan_result = await test_scan_and_name(args)
    summary["tests"]["scan"] = scan_result
    print(f"\n== 1/2 scan+name: {pass_fail(scan_result['pass'])} ==\n")
    target = scan_result.get("target")
    if not target:
        summary["fatal"] = "no target discovered"
        _write_results(summary)
        return 2

    address = target["address"]
    print(f"[conn] connecting to {address} name={target.get('name')!r}")
    async with BleakClient(address, timeout=25) as cli:
        print(f"[conn] connected={cli.is_connected} mtu_size={getattr(cli, 'mtu_size', None)}")
        sess = Session(cli, address=address, username=args.user, password=args.password)
        summary["board"] = {
            "address": address,
            "name": target.get("name"),
            "adv_local_name": target.get("local_name"),
            "mtu_size": getattr(cli, "mtu_size", None),
        }
        await cli.start_notify(CMD_TX, sess.on_notify)
        await asyncio.sleep(0.2)
        try:
            info = await sess.v1("device.info", {}, timeout=10, token=False, id_prefix="info")
            summary["device_info"] = sanitize(info.obj)
            sess.record(f"[info] {json.dumps(sanitize(info.obj), ensure_ascii=False)}")
            await sess.login(args.user, args.password)
            await asyncio.sleep(0.25)
            status = await sess.v1("network.status", {}, timeout=15, id_prefix="status")
            status_data = status.obj.get("data") if isinstance(status.obj.get("data"), dict) else {}
            summary["network_status"] = sanitize(status_data)
            sess.record(f"[status] {json.dumps(sanitize(status_data), ensure_ascii=False)}")
            await asyncio.sleep(0.25)
            cfg = await sess.v1("config.get", {}, timeout=20, id_prefix="cfg-initial")
            cfg_data = cfg.obj.get("data") if isinstance(cfg.obj.get("data"), dict) else {}
            summary["config_initial"] = {
                "bytes": cfg.raw_len,
                "frags": len(cfg.frag_sizes),
                "frag_sizes": cfg.frag_sizes,
            }
            await asyncio.sleep(0.25)
            profiles = await sess.v1(
                "network.profile.list", {}, timeout=15, id_prefix="profiles"
            )
            summary["profiles"] = sanitize(profiles.obj.get("data"))

            async def run_or_retry(name: str, factory: Any, attempts: int = 2) -> dict[str, Any]:
                last: dict[str, Any] = {}
                for attempt in range(attempts):
                    before = sess.reconnect_count
                    try:
                        res = await factory()
                        res["reconnects"] = sess.reconnect_count - before
                        return res
                    except Exception as exc:  # noqa: BLE001
                        last = {
                            "name": name,
                            "pass": False,
                            "error": f"{type(exc).__name__}: {exc}",
                            "attempt": attempt + 1,
                        }
                        sess.record(f"[test] {name} attempt {attempt + 1} failed: {last['error']}")
                        if attempt + 1 < attempts:
                            try:
                                await sess.reconnect()
                            except Exception as rexc:  # noqa: BLE001
                                last["reconnect_error"] = f"{type(rexc).__name__}: {rexc}"
                                sess.record(f"[test] reconnect for retry failed: {last['reconnect_error']}")
                return last

            long_result = await run_or_retry(
                "3_LONG_RESPONSES_10X", lambda: test_long_responses(sess, args.n_long)
            )
            summary["tests"]["long_scan"] = long_result
            print(f"\n== 3 long scan x{args.n_long}: {pass_fail(long_result['pass'])} ==\n")

            extra_result = await run_or_retry(
                "3b_CONFIG_GET_10X", lambda: test_extra_fragmentation(sess, args.n_long)
            )
            summary["tests"]["long_config"] = extra_result
            print(f"\n== 3b long config x{args.n_long}: {pass_fail(extra_result['pass'])} ==\n")

            ssid = args.ssid or status_data.get("sta_ssid") or ""
            if not ssid:
                summary["tests"]["operation"] = {
                    "name": "4_OPERATION_GET_DURING_FRAGMENTATION",
                    "pass": False,
                    "reason": "no SSID available (network.status.sta_ssid empty and --ssid not set)",
                }
                summary["tests"]["wrong_password"] = {
                    "name": "5a_WRONG_PASSWORD",
                    "pass": False,
                    "reason": "no SSID",
                }
                summary["tests"]["cancel_race"] = {
                    "name": "5b_CANCEL_RACE",
                    "pass": False,
                    "reason": "no SSID",
                }
            else:
                correct_password = args.wifi_password or args.wrong_password
                if args.wifi_password:
                    print(f"[op] using ssid={ssid!r}; correct Wi-Fi password supplied (not logged)")
                else:
                    print(
                        f"[op] using ssid={ssid!r}; NO correct Wi-Fi password, "
                        "tryconnect/apply is expected to fail"
                    )
                op_result = await run_or_retry(
                    "4_OPERATION_GET_DURING_FRAGMENTATION",
                    lambda: test_operation_during_fragmentation(sess, ssid, correct_password),
                )
                summary["tests"]["operation"] = op_result
                print(f"\n== 4 operation.get during fragmentation: {pass_fail(op_result['pass'])} ==\n")

                if op_result.get("apply", {}).get("ok"):
                    summary["wifi_after_apply"] = await wait_wifi_ready(sess, timeout=20)
                else:
                    summary["wifi_after_apply"] = {
                        "ok": False,
                        "skipped": True,
                        "reason": "apply did not succeed; no network switch to wait for",
                    }

                wrong_result = await run_or_retry(
                    "5a_WRONG_PASSWORD",
                    lambda: test_error_password(sess, ssid, args.wrong_password),
                )
                summary["tests"]["wrong_password"] = wrong_result
                print(f"\n== 5a wrong password: {pass_fail(wrong_result['pass'])} ==\n")

                race_result = await run_or_retry(
                    "5b_CANCEL_RACE",
                    lambda: test_cancel_race(sess, ssid, args.wrong_password),
                )
                summary["tests"]["cancel_race"] = race_result
                print(f"\n== 5b cancel race: {pass_fail(race_result['pass'])} ==\n")

            notfound_result = await run_or_retry(
                "5c_NOT_FOUND", lambda: test_not_found(sess)
            )
            summary["tests"]["not_found"] = notfound_result
            print(f"\n== 5c not found: {pass_fail(notfound_result['pass'])} ==\n")
            summary["reconnect_count"] = sess.reconnect_count

            core_pass = bool(
                scan_result.get("pass")
                and summary["tests"]["long_scan"].get("pass")
                and summary["tests"].get("operation", {}).get("pass")
            )
            summary["p0_core_pass"] = core_pass
            summary["phone_ui_verified"] = False
            if args.final_config_set:
                if core_pass:
                    final_result = await run_or_retry(
                        "6_FINAL_CONFIG_SET_NET_MODE_STA",
                        lambda: test_final_config_set(sess, args),
                    )
                    summary["tests"]["final_config_set"] = final_result
                    print(f"\n== 6 final config.set: {pass_fail(final_result['pass'])} ==\n")
                else:
                    summary["tests"]["final_config_set"] = {
                        "name": "6_FINAL_CONFIG_SET_NET_MODE_STA",
                        "pass": False,
                        "skipped": True,
                        "reason": "P0 core did not pass; not sending config.set",
                    }
                    print("\n== 6 final config.set: SKIPPED (P0 core failed) ==\n")
            else:
                summary["tests"]["final_config_set"] = {
                    "name": "6_FINAL_CONFIG_SET_NET_MODE_STA",
                    "skipped": True,
                    "reason": "--no-final-config-set",
                }
        finally:
            try:
                await sess.close()
            except Exception:  # noqa: BLE001
                pass
            try:
                await cli.stop_notify(CMD_TX)
            except Exception:  # noqa: BLE001
                pass

    summary["log"] = sess.log
    _write_results(summary)
    print("\n== SUMMARY ==")
    for key, value in summary["tests"].items():
        print(f"  {key:20s} {pass_fail(bool(value.get('pass')))}")
    print(f"  p0_core_pass={summary.get('p0_core_pass')} phone_ui_verified=False")
    print(f"[results] {summary.get('_path')}")
    return 0 if summary["p0_core_pass"] else 1


def _write_results(summary: dict[str, Any]) -> None:
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    path = RESULTS_DIR / f"p0_{ts}.json"
    summary["_path"] = str(path)
    path.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"[results] wrote {path}")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="IotHub BLE P0 acceptance harness")
    parser.add_argument("--address", help="BLE address to pick (default: name prefix / NUS UUID)")
    parser.add_argument("--ssid", help="Wi-Fi SSID for tryconnect (default: network.status.sta_ssid)")
    parser.add_argument(
        "--wifi-password",
        default=os.environ.get("IOTHUB_WIFI_PASSWORD"),
        help="correct Wi-Fi password for the success path; prefer env IOTHUB_WIFI_PASSWORD "
             "(the value is never written to the result file)",
    )
    parser.add_argument(
        "--wrong-password",
        default="p0-wrong-" + "".join(random.choices(string.ascii_letters + string.digits, k=12)),
        help="deliberately wrong Wi-Fi password for error/cancel tests",
    )
    parser.add_argument("--user", default="admin", help="BLE admin username")
    parser.add_argument("--password", default="123456", help="BLE admin password")
    parser.add_argument("--n-long", type=int, default=10, help="number of long responses")
    parser.add_argument("--scan-timeout", type=float, default=12, help="filtered scan seconds")
    parser.add_argument(
        "--final-config-set",
        dest="final_config_set",
        action="store_true",
        default=True,
        help="send v0 config.set {net_mode: sta} after P0 core passes (default: on)",
    )
    parser.add_argument(
        "--no-final-config-set",
        dest="final_config_set",
        action="store_false",
        help="do not send the final config.set",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        print("\ninterrupted")
        return 130
    except Exception as exc:  # noqa: BLE001
        print(f"\nFATAL: {type(exc).__name__}: {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
