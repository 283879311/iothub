"""Host tests for the C15-2 BLE read helpers (fake session, fake clock)."""

import asyncio
import inspect
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "review"))
import c152_ble as ble  # noqa: E402

READY_NET = {"trial": {"state": "idle"}, "mqtt_connected": True,
             "sta_connected": True, "sta_has_ip": True, "sta_ip": "10.0.0.2",
             "sta_ssid": "ssid", "mode": "sta", "connected_ssid": "ssid"}


class Clock:
    def __init__(self):
        self.wall = 1_790_000_000.0
        self.mono = 0.0

    def time(self):
        return self.wall

    def now(self):
        return self.mono

    def advance(self, seconds):
        self.wall += seconds
        self.mono += seconds

    async def sleep(self, seconds):
        self.advance(seconds)


class FakeResp:
    def __init__(self, obj):
        self.obj = obj


class FakeSess:
    token = "tok"

    def __init__(self, handler):
        self.handler = handler
        self.ids = []
        self.pending = {}

    async def send_request(self, req, timeout=15, retries=0):
        self.ids.append(req["id"])
        rid = req["id"]
        if rid in self.pending:
            return FakeResp(self.pending.pop(rid))
        result = self.handler(req)
        if inspect.isawaitable(result):
            result = await result
        return FakeResp(result)


def make_deps(handler, clock=None):
    clock = clock or Clock()
    sess = FakeSess(handler)
    deps = ble.Deps(sess, None, clock.time, clock.now, clock.sleep)
    return deps, sess, clock


def run(coro):
    return asyncio.run(coro)


class ReadCmdTests(unittest.TestCase):
    def test_retry_uses_unique_ids(self):
        calls = {"n": 0}

        def handler(req):
            calls["n"] += 1
            if calls["n"] == 1:
                raise asyncio.TimeoutError()
            return {"ok": True, "data": {}}

        deps, sess, _ = make_deps(handler)
        out = run(ble.read_cmd(deps, "probe", {}, "r"))
        self.assertEqual(out, {"ok": True, "data": {}})
        self.assertEqual(sess.ids, ["r-1", "r-2"])

    def test_deadline_exhausted_before_send(self):
        deps, sess, clock = make_deps(lambda req: {"ok": True, "data": {}})
        with self.assertRaises(ble.ReadTimeout):
            run(ble.read_cmd(deps, "probe", {}, "p",
                             deadline=clock.now() - 1.0))
        self.assertEqual(sess.ids, [])

    def test_three_timeouts_stop(self):
        def handler(req):
            raise asyncio.TimeoutError()

        deps, sess, _ = make_deps(handler)
        with self.assertRaises(ble.ReadTimeout):
            run(ble.read_cmd(deps, "probe", {}, "r"))
        self.assertEqual(sess.ids, ["r-1", "r-2", "r-3"])

    def test_late_response_cannot_satisfy_next_poll(self):
        calls = {"n": 0}

        def handler(req):
            calls["n"] += 1
            if calls["n"] == 1:
                raise asyncio.TimeoutError()
            return {"ok": True, "data": {"fresh": True}}

        deps, sess, _ = make_deps(handler)
        first = run(ble.read_cmd(deps, "probe", {}, "p"))
        self.assertEqual(first, {"ok": True, "data": {"fresh": True}})
        stale_id = sess.ids[0]
        sess.pending[stale_id] = {"ok": True, "data": {"stale": True}}
        second = run(ble.read_cmd(deps, "probe", {}, "p"))
        self.assertEqual(second, {"ok": True, "data": {"fresh": True}})
        self.assertIn(stale_id, sess.pending)
        self.assertEqual(len(sess.ids), len(set(sess.ids)))

    def test_ids_unique_across_calls(self):
        deps, sess, _ = make_deps(lambda req: {"ok": True, "data": {}})
        run(ble.read_cmd(deps, "probe", {}, "p"))
        run(ble.read_cmd(deps, "probe", {}, "p"))
        self.assertEqual(len(sess.ids), len(set(sess.ids)))

    def test_malformed_data_is_device_error(self):
        deps, _, _ = make_deps(lambda req: {"ok": True})
        with self.assertRaises(ble.DeviceObservationError) as ctx:
            run(ble.read_data(deps, "device.status", {}, "d"))
        self.assertEqual(ctx.exception.code, "malformed_response")

    def test_command_error_is_device_error(self):
        deps, _, _ = make_deps(
            lambda req: {"ok": False, "error": {"code": "not_found"}})
        with self.assertRaises(ble.DeviceObservationError) as ctx:
            run(ble.read_data(deps, "operation.get", {}, "o"))
        self.assertEqual(ctx.exception.code, "command_error")


class ReadinessTests(unittest.TestCase):
    def handler(self, req):
        if req["cmd"] == "device.status":
            return {"ok": True, "data": {"network": READY_NET}}
        return {"ok": True, "data": {}}

    def test_real_time_gate(self):
        deps, _, clock = make_deps(self.handler)
        self.assertTrue(run(ble.wait_ready(deps, 10.0)))
        self.assertGreaterEqual(clock.mono, 10.0)

    def test_gap_resets_streak(self):
        clock = Clock()

        async def slow_sleep(seconds):
            clock.advance(4.0 * seconds)

        sess = FakeSess(self.handler)
        deps = ble.Deps(sess, None, clock.time, clock.now, slow_sleep)
        self.assertFalse(run(ble.wait_ready(deps, 10.0)))


if __name__ == "__main__":
    unittest.main()
