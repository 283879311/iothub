"""C15-2 BLE side: session dependencies and bounded read-only command helpers.

Pure orchestration of the BLE command channel (`device.status`,
`network.status`, `operation.get`); no REST/MQTT. All reads use globally
unique request ids and hard deadlines.
"""

from __future__ import annotations

import asyncio
import time

import c152_policy as policy


class ReadTimeout(Exception):
    pass


class DeadlineExceeded(ReadTimeout):
    """The harness deadline ran out; not a device read failure."""


class StopRun(Exception):
    pass


class DeviceObservationError(Exception):
    def __init__(self, code: str, detail: str = "") -> None:
        super().__init__(code + (":" + detail if detail else ""))
        self.code = code
        self.detail = detail


class RequestIds:
    """Monotonic, collision-free request ids across all polls and retries."""

    def __init__(self) -> None:
        self._seq = 0

    def next(self, tag: str) -> str:
        self._seq += 1
        return "%s-%d" % (tag, self._seq)


class Deps:
    """Injectable dependencies for run_case()."""

    def __init__(self, sess, observer, wall=time.time,
                 mono=time.monotonic, sleep=asyncio.sleep, rids=None,
                 write_timeout=20.0):
        self.sess = sess
        self.observer = observer
        self.wall = wall
        self.mono = mono
        self.sleep = sleep
        self.rids = rids or RequestIds()
        self.write_timeout = write_timeout


def select_target(recs, addr: str):
    """Bind the scan result to the explicit BLE address; never "first hit"."""
    for rec in recs:
        if rec.get("address", "").lower() == addr.lower():
            return rec
    return None


def req(rid: str, cmd: str, args: dict, token: str | None) -> dict:
    return {"id": rid, "cmd": cmd, "args": args, "version": "1.0", "token": token}


async def read_cmd(deps: Deps, cmd: str, args: dict, base_rid: str,
                   timeout: float = 15.0, deadline: float | None = None) -> dict:
    """Read-only command; unique id per attempt; hard deadline respected."""
    attempt = 1
    while True:
        now = deps.mono()
        if deadline is not None:
            remaining = deadline - now
            if remaining <= 0:
                raise DeadlineExceeded("%s deadline exhausted before send" % cmd)
            effective = min(timeout, remaining)
        else:
            effective = timeout
        rid = deps.rids.next(base_rid)
        try:
            resp = await asyncio.wait_for(
                deps.sess.send_request(
                    req(rid, cmd, args, deps.sess.token),
                    timeout=effective, retries=0),
                timeout=effective)
            if deadline is not None and deps.mono() > deadline:
                raise DeadlineExceeded("%s returned after deadline" % cmd)
            return resp.obj or {}
        except asyncio.TimeoutError:
            if not policy.should_retry_read(attempt):
                raise ReadTimeout("%s timed out after %d attempts" % (cmd, attempt))
            if deadline is not None:
                remaining = deadline - deps.mono()
                if remaining <= 0:
                    raise DeadlineExceeded(
                        "%s deadline exhausted after timeout" % cmd)
                nap = min(policy.READ_RETRY_DELAY_S, remaining)
            else:
                nap = policy.READ_RETRY_DELAY_S
            attempt += 1
            await deps.sleep(nap)


async def read_data(deps: Deps, cmd: str, args: dict, base_rid: str,
                    timeout: float = 15.0,
                    deadline: float | None = None) -> dict:
    """Read-only command returning the v1 `data` object; malformed -> device error."""
    obj = await read_cmd(deps, cmd, args, base_rid, timeout, deadline)
    if obj.get("ok") is False:
        code = (obj.get("error") or {}).get("code") or "error"
        raise DeviceObservationError("command_error", "%s:%s" % (cmd, code))
    data = obj.get("data")
    if not isinstance(data, dict):
        raise DeviceObservationError("malformed_response", cmd)
    return data


async def identity_gate(deps: Deps, expected: dict,
                        deadline: float | None = None) -> tuple[dict, list[str]]:
    dev = await read_data(deps, "device.status", {}, "ident", 15, deadline)
    return dev, policy.identity_errors(dev, expected)


async def wait_ready(deps: Deps, stable_s: float) -> bool:
    """Real-time stability gate; every read/delay is capped by the deadline."""
    start = None
    last_ok = None
    deadline = deps.mono() + 60.0
    while True:
        if deadline - deps.mono() <= 0:
            return False
        try:
            dev = await read_data(deps, "device.status", {}, "ready", 30,
                                  deadline)
        except DeadlineExceeded:
            return False
        net = dev.get("network") or {}
        ok = (net.get("trial", {}).get("state") == "idle"
              and net.get("mqtt_connected") is True
              and net.get("sta_connected") is True)
        now = deps.mono()
        if ok:
            if start is None or (last_ok is not None
                                 and now - last_ok > policy.READY_MAX_GAP_S):
                start = now
            last_ok = now
            if now - start >= stable_s:
                return True
        else:
            start = None
            last_ok = None
        remaining = deadline - deps.mono()
        if remaining <= 0:
            return False
        await deps.sleep(min(1.0, remaining))


async def wait_terminal(deps: Deps, op_id: str, timeout_s: float) -> dict:
    deadline = deps.mono() + timeout_s
    last: dict = {}
    while True:
        if deadline - deps.mono() <= 0:
            return last
        try:
            last = await read_data(deps, "operation.get",
                                   {"operation_id": op_id}, "term", 10,
                                   deadline)
        except DeadlineExceeded:
            return last
        if last.get("state") in policy.TERMINAL_STATES:
            return last
        remaining = deadline - deps.mono()
        if remaining <= 0:
            return last
        await deps.sleep(min(0.3, remaining))


async def wait_sta(deps: Deps, timeout_s: float) -> bool:
    deadline = deps.mono() + timeout_s
    while True:
        if deadline - deps.mono() <= 0:
            return False
        try:
            dev = await read_data(deps, "device.status", {}, "sta", 30,
                                  deadline)
        except DeadlineExceeded:
            return False
        net = dev.get("network") or {}
        if net.get("sta_connected") is True:
            return True
        remaining = deadline - deps.mono()
        if remaining <= 0:
            return False
        await deps.sleep(min(1.5, remaining))
