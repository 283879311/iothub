"""Host tests for the C15-2 runner control flow (fakes only: no BLE, no MQTT)."""

import asyncio
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "review"))
import c152_policy as p  # noqa: E402
import c152_mqtt_attrs as r  # noqa: E402

IDENT = {"device_id": "IotHub-65D8", "firmware": "1.3-956afcd",
         "build_sha": "c231c9501"}
READY = {"trial": {"state": "idle"}, "mqtt_connected": True,
         "sta_connected": True, "sta_has_ip": True, "sta_ip": "10.0.0.2"}
ATTR_OK = {"net_mode": "sta", "sta_ssid": "ssid", "sta_ip": "10.0.0.2",
           "uart_baudrate": 115200}


class FakeResp:
    def __init__(self, obj):
        self.obj = obj


class Clock:
    def __init__(self):
        self.wall = 1000.0
        self.mono = 0.0

    def time(self):
        return self.wall

    def now(self):
        return self.mono

    async def sleep(self, seconds):
        self.wall += seconds
        self.mono += seconds


class FakeSess:
    token = "tok"

    def __init__(self, handler):
        self.handler = handler
        self.ids = []
        self.cmds = []
        self.writes = 0
        self.pending = {}  # late responses keyed by request id (Session-like)

    async def send_request(self, req, timeout=15, retries=0):
        self.ids.append(req["id"])
        self.cmds.append(req["cmd"])
        if req["cmd"] == "network.tryconnect":
            self.writes += 1
        rid = req["id"]
        if rid in self.pending:  # simulate Session consuming a stored response
            return FakeResp(self.pending.pop(rid))
        return FakeResp(self.handler(req))


class FakeTap:
    def __init__(self, clock, attrs=None):
        self.clock = clock
        self.attrs = attrs
        self.stopped = False

    def snapshot(self):
        if self.attrs is None:
            return []
        return [{"ts": self.clock.wall, "obj": self.attrs}]

    def stop(self):
        self.stopped = True


def make_handler(identity=IDENT, identity_after=None, tryconnect=None,
                 terminal=None):
    state = {"ident": 0}

    def handler(req):
        rid, cmd = req["id"], req["cmd"]
        if cmd == "device.status":
            if rid.startswith("ident"):
                state["ident"] += 1
                if state["ident"] == 1:
                    return {"ok": True, "data": identity}
                return {"ok": True, "data": identity_after or identity}
            return {"ok": True, "data": {"network": {"sta_connected": True}}}
        if cmd == "network.status":
            return {"ok": True, "data": READY}
        if cmd == "network.tryconnect":
            if callable(tryconnect):
                return tryconnect(req)
            return tryconnect or {"ok": True, "data": {"operation_id": "op-1"}}
        if cmd == "operation.get":
            return {"ok": True,
                    "data": terminal or {"state": "failed",
                                         "error": "wrong_password"}}
        return {"ok": True, "data": {}}

    return handler


def make_deps(handler, clock, attrs=ATTR_OK, tap_error=None, sleep=None):
    sess = FakeSess(handler)

    def factory():
        if tap_error is not None:
            raise tap_error
        return FakeTap(clock, attrs)

    deps = r.Deps(sess, factory, clock.time, clock.now, sleep or clock.sleep)
    return deps, sess


def run(coro):
    return asyncio.run(coro)


class IdentityGateTests(unittest.TestCase):
    def test_device_mismatch_blocks_write(self):
        clock = Clock()
        deps, sess = make_deps(make_handler(identity=dict(IDENT, device_id="Other")),
                               clock)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]), ("STOP", "identity"))
        self.assertEqual(sess.writes, 0)
        self.assertNotIn("network.tryconnect", sess.cmds)

    def test_firmware_mismatch_blocks_write(self):
        clock = Clock()
        deps, sess = make_deps(make_handler(identity=dict(IDENT, firmware="1.3-0")),
                               clock)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual(result["reason"], "identity")
        self.assertEqual(sess.writes, 0)

    def test_build_sha_missing_blocks_write(self):
        clock = Clock()
        bad = dict(IDENT)
        del bad["build_sha"]
        deps, sess = make_deps(make_handler(identity=bad), clock)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual(result["reason"], "identity")
        self.assertEqual(sess.writes, 0)


class ReadinessTests(unittest.TestCase):
    def test_real_time_gate(self):
        clock = Clock()
        deps, _ = make_deps(make_handler(), clock)
        self.assertTrue(run(r.wait_ready(deps, 10.0)))
        self.assertGreaterEqual(clock.mono, 10.0)

    def test_gap_resets_streak(self):
        clock = Clock()

        async def slow_sleep(seconds):
            clock.wall += 4.0 * seconds
            clock.mono += 4.0 * seconds

        deps, _ = make_deps(make_handler(), clock, sleep=slow_sleep)
        self.assertFalse(run(r.wait_ready(deps, 10.0)))


class ReadCmdTests(unittest.TestCase):
    def test_retry_uses_unique_ids(self):
        clock = Clock()
        calls = {"n": 0}

        def handler(req):
            calls["n"] += 1
            if calls["n"] == 1:
                raise asyncio.TimeoutError()
            return {"ok": True, "data": {}}

        deps, sess = make_deps(handler, clock)
        out = run(r.read_cmd(deps, "probe", {}, "r"))
        self.assertEqual(out, {"ok": True, "data": {}})
        self.assertEqual(sess.ids, ["r-1", "r-2"])

    def test_ids_unique_across_calls(self):
        clock = Clock()
        deps, sess = make_deps(make_handler(), clock)
        run(r.read_cmd(deps, "probe", {}, "p"))
        run(r.read_cmd(deps, "probe", {}, "p"))
        self.assertEqual(len(sess.ids), len(set(sess.ids)))

    def test_late_response_cannot_satisfy_next_poll(self):
        clock = Clock()
        calls = {"n": 0}

        def handler(req):
            calls["n"] += 1
            if calls["n"] == 1:
                raise asyncio.TimeoutError()
            return {"ok": True, "data": {"fresh": True}}

        deps, sess = make_deps(handler, clock)
        first = run(r.read_cmd(deps, "probe", {}, "p"))
        self.assertEqual(first, {"ok": True, "data": {"fresh": True}})
        stale_id = sess.ids[0]
        sess.pending[stale_id] = {"ok": True, "data": {"stale": True}}
        second = run(r.read_cmd(deps, "probe", {}, "p"))
        self.assertEqual(second, {"ok": True, "data": {"fresh": True}})
        self.assertIn(stale_id, sess.pending)  # stale entry not consumed
        self.assertEqual(len(sess.ids), len(set(sess.ids)))

    def test_three_timeouts_stop(self):
        clock = Clock()

        def handler(req):
            raise asyncio.TimeoutError()

        deps, sess = make_deps(handler, clock)
        with self.assertRaises(r.ReadTimeout):
            run(r.read_cmd(deps, "probe", {}, "r"))
        self.assertEqual(sess.ids, ["r-1", "r-2", "r-3"])


class WriteGateTests(unittest.TestCase):
    def test_transport_exception_not_resent(self):
        clock = Clock()

        def tryconnect(req):
            raise RuntimeError("transport down")

        deps, sess = make_deps(make_handler(tryconnect=tryconnect), clock)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("STOP", "write_outcome_unknown"))
        self.assertEqual(sess.writes, 1)
        self.assertEqual(sess.cmds[-1], "network.tryconnect")

    def test_busy_is_not_covered(self):
        clock = Clock()
        busy = {"ok": False, "error": {"code": "busy"}}
        deps, sess = make_deps(make_handler(tryconnect=busy), clock)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("NOT_COVERED", "tryconnect_busy"))
        self.assertEqual(sess.writes, 1)


class OutcomeTests(unittest.TestCase):
    def test_zero_samples_not_covered(self):
        clock = Clock()
        deps, _ = make_deps(make_handler(), clock, attrs=None)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("NOT_COVERED", "no_attributes_sample"))

    def test_empty_ip_fails(self):
        clock = Clock()
        bad = dict(ATTR_OK, sta_ip="")
        deps, _ = make_deps(make_handler(), clock, attrs=bad)
        # network.status is READY with sta_ip "10.0.0.2" -> mismatch/empty both fail
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual(result["verdict"], "FAIL")
        self.assertIn("attr_sta_ip_empty", result["cross_errors"])

    def test_second_identity_mismatch_fails(self):
        clock = Clock()
        deps, _ = make_deps(make_handler(
            identity_after=dict(IDENT, device_id="Other")), clock)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("FAIL", "identity_mismatch"))

    def test_pass_path(self):
        clock = Clock()
        deps, sess = make_deps(make_handler(), clock)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]), ("PASS", "ok"))
        self.assertEqual(result["window"]["n_attr"], 1)
        self.assertEqual(result["cross_errors"], [])
        self.assertEqual(sess.writes, 1)

    def test_run_case_timeout_stop(self):
        clock = Clock()

        def handler(req):
            if req["cmd"] == "network.status":
                raise asyncio.TimeoutError()
            return make_handler()(req)

        deps, sess = make_deps(handler, clock)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("STOP", "read_timeout"))
        self.assertEqual(sess.writes, 0)

    def test_run_case_ids_unique(self):
        clock = Clock()
        deps, sess = make_deps(make_handler(), clock)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual(result["verdict"], "PASS")
        self.assertEqual(len(sess.ids), len(set(sess.ids)))
        for prefix in ("ident", "ready", "term", "sta", "post"):
            self.assertTrue(any(i.startswith(prefix + "-") for i in sess.ids),
                            prefix)
        self.assertTrue(any(i.startswith("tc1-") for i in sess.ids))

    def test_tap_factory_stop_blocks_write(self):
        clock = Clock()
        deps, sess = make_deps(make_handler(), clock,
                               tap_error=r.StopRun("mqtt SUBACK empty"))
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]), ("STOP", "stop"))
        self.assertEqual(sess.writes, 0)

    def test_exit_codes(self):
        self.assertEqual(p.exit_code("PASS"), 0)
        self.assertEqual(p.exit_code("FAIL"), 1)
        self.assertEqual(p.exit_code("NOT_COVERED"), 2)
        self.assertEqual(p.exit_code("STOP"), 3)


if __name__ == "__main__":
    unittest.main()
