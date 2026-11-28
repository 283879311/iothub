"""Host tests for the C15-2 Telemetry REST runner (fakes only: no BLE/TB)."""

import asyncio
import inspect
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "review"))
import c152_policy as p  # noqa: E402
import c152_rest_observer as rest  # noqa: E402
import c152_telemetry_rest as r  # noqa: E402

IDENT = {"device_id": "IotHub-65D8", "firmware": "1.3-956afcd",
         "build_sha": "c231c9501"}
READY = {"trial": {"state": "idle"}, "mqtt_connected": True,
         "sta_connected": True, "sta_has_ip": True, "sta_ip": "10.0.0.2",
         "sta_ssid": "ssid", "net_mode": "sta", "connected_ssid": "ssid"}
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

    def __init__(self, clock, shared, handler):
        self.clock = clock
        self.shared = shared
        self.handler = handler
        self.ids = []
        self.cmds = []
        self.writes = 0
        self.pending = {}

    async def send_request(self, req, timeout=15, retries=0):
        self.ids.append(req["id"])
        self.cmds.append(req["cmd"])
        if req["cmd"] == "network.tryconnect":
            self.writes += 1
            self.shared["written"] = True
            self.shared["op_start"] = self.clock.wall
        rid = req["id"]
        if rid in self.pending:
            return FakeResp(self.pending.pop(rid))
        result = self.handler(req, self.clock, self.shared)
        if inspect.isawaitable(result):
            result = await result
        return FakeResp(result)


def make_handler(identity=IDENT, identity_after=None, tryconnect=None,
                 terminal=None, post_status=None, fail_after_write=()):
    state = {"ident": 0}

    def handler(req, clock, shared):
        cmd, rid = req["cmd"], req["id"]
        if cmd in fail_after_write and shared.get("written"):
            raise asyncio.TimeoutError()
        if cmd == "device.status":
            if rid.startswith("ident"):
                state["ident"] += 1
                data = identity if state["ident"] == 1 else (identity_after or identity)
                return {"ok": True, "data": data}
            return {"ok": True,
                    "data": dict(identity, network=READY_NET)}
        if cmd == "network.status":
            if post_status is not None and shared.get("written"):
                return {"ok": True, "data": post_status}
            return {"ok": True, "data": READY}
        if cmd == "network.tryconnect":
            if callable(tryconnect):
                return tryconnect(req, clock, shared)
            return tryconnect or {"ok": True, "data": {"operation_id": "op-1"}}
        if cmd == "operation.get":
            clock.advance(4.0)
            return {"ok": True,
                    "data": terminal or {"state": "failed",
                                         "error": "wrong_password"}}
        return {"ok": True, "data": {}}

    return handler


def ok_baseline(clock, shared):
    ts = int((clock.wall - 10.0) * 1000)
    return {"net_mode": [{"ts": ts, "value": "sta"}],
            "uart_baudrate": [{"ts": ts, "value": "115200"}],
            "sta_has_ip": [{"ts": ts, "value": "true"}]}


def ok_post(clock, shared):
    ts = int((shared["op_start"] + 2.0) * 1000)
    return {"net_mode": [{"ts": ts, "value": "sta"}],
            "uart_baudrate": [{"ts": ts, "value": "115200"}],
            "sta_has_ip": [{"ts": ts, "value": "true"}]}


def ok_attrs(clock, shared):
    ts = int((clock.wall - 3600.0) * 1000)
    return [{"key": "sta_ssid", "value": "ssid", "lastUpdateTs": ts},
            {"key": "sta_ip", "value": "10.0.0.2", "lastUpdateTs": ts}]


class FakeObserver:
    def __init__(self, clock, shared, baseline=ok_baseline, post=ok_post,
                 attrs=ok_attrs, start_error=None, series_error=None,
                 attrs_error=None):
        self.clock = clock
        self.shared = shared
        self.baseline = baseline
        self.post = post
        self.attrs = attrs
        self.start_error = start_error
        self.series_error = series_error
        self.attrs_error = attrs_error
        self.calls = 0
        self.started = False

    async def start(self):
        self.started = True
        if self.start_error is not None:
            raise self.start_error

    async def fetch_series(self, keys, start_ms, end_ms):
        call = self.calls
        self.calls += 1
        if self.series_error is not None and call >= 1:
            raise self.series_error
        factory = self.baseline if call == 0 else self.post
        return factory(self.clock, self.shared)

    async def fetch_attributes(self, keys):
        if self.attrs_error is not None:
            raise self.attrs_error
        return self.attrs(self.clock, self.shared)


def make_deps(handler=None, observer=None, write_timeout=20.0):
    clock = Clock()
    shared = {}
    sess = FakeSess(clock, shared, handler or make_handler())
    obs = observer or FakeObserver(clock, shared)
    deps = r.Deps(sess, obs, clock.time, clock.now, clock.sleep,
                  write_timeout=write_timeout)
    return deps, sess, obs, clock, shared


def run(coro):
    return asyncio.run(coro)


class PassPathTests(unittest.TestCase):
    def test_pass_records_all_evidence(self):
        deps, sess, obs, _, _ = make_deps()
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]), ("PASS", "ok"))
        self.assertEqual(sess.writes, 1)
        self.assertEqual(obs.calls, 2)
        self.assertEqual(result["operation"]["terminal"],
                         {"state": "failed", "error": "wrong_password"})
        self.assertTrue(result["attrs_staleness"]["sta_ssid"]["stale"])
        self.assertEqual(result["status_errors"], [])
        self.assertEqual(result["attr_errors"], [])

    def test_attrs_sta_ip_difference_is_allowed(self):
        clock = Clock()
        shared = {}

        def attrs(_clock, _shared):
            return [{"key": "sta_ssid", "value": "ssid",
                     "lastUpdateTs": int((clock.wall - 10) * 1000)},
                    {"key": "sta_ip", "value": "192.168.1.9",
                     "lastUpdateTs": int((clock.wall - 10) * 1000)}]

        sess = FakeSess(clock, shared, make_handler())
        obs = FakeObserver(clock, shared, attrs=attrs)
        deps = r.Deps(sess, obs, clock.time, clock.now, clock.sleep)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]), ("PASS", "ok"))

    def test_attributes_missing_is_fail(self):
        def attrs(_clock, _shared):
            return []

        clock = Clock()
        shared = {}
        sess = FakeSess(clock, shared, make_handler())
        obs = FakeObserver(clock, shared, attrs=attrs)
        deps = r.Deps(sess, obs, clock.time, clock.now, clock.sleep)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("FAIL", "attributes_inconsistent"))
        self.assertIn("sta_ssid_missing", result["attr_errors"])

    def test_attributes_ssid_mismatch(self):
        def attrs(_clock, _shared):
            return [{"key": "sta_ssid", "value": "other",
                     "lastUpdateTs": int(_clock.wall * 1000)}]

        clock = Clock()
        shared = {}
        sess = FakeSess(clock, shared, make_handler())
        obs = FakeObserver(clock, shared, attrs=attrs)
        deps = r.Deps(sess, obs, clock.time, clock.now, clock.sleep)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("FAIL", "attributes_inconsistent"))
        self.assertIn("sta_ssid_mismatch", result["attr_errors"])


class BaselineTests(unittest.TestCase):
    def test_baseline_missing_blocks_write(self):
        clock = Clock()
        shared = {}
        sess = FakeSess(clock, shared, make_handler())
        obs = FakeObserver(clock, shared, baseline=lambda c, s: {})
        deps = r.Deps(sess, obs, clock.time, clock.now, clock.sleep)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("NOT_COVERED", "baseline_missing_net_mode"))
        self.assertEqual(sess.writes, 0)

    def test_baseline_stale_blocks_write(self):
        clock = Clock()
        shared = {}

        def stale(c, _s):
            ts = int((c.wall - 60.0) * 1000)
            return {"net_mode": [{"ts": ts, "value": "sta"}],
                    "uart_baudrate": [{"ts": ts, "value": "115200"}],
                    "sta_has_ip": [{"ts": ts, "value": "true"}]}

        sess = FakeSess(clock, shared, make_handler())
        obs = FakeObserver(clock, shared, baseline=stale)
        deps = r.Deps(sess, obs, clock.time, clock.now, clock.sleep)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("NOT_COVERED", "baseline_stale_net_mode"))
        self.assertEqual(sess.writes, 0)


class WindowTests(unittest.TestCase):
    def test_no_post_sample_not_covered(self):
        clock = Clock()
        shared = {}
        sess = FakeSess(clock, shared, make_handler())
        obs = FakeObserver(clock, shared, post=lambda c, s: {})
        deps = r.Deps(sess, obs, clock.time, clock.now, clock.sleep)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("NOT_COVERED", "telemetry_no_sample_net_mode"))
        self.assertEqual(sess.writes, 1)

    def test_future_sample_excluded(self):
        clock = Clock()
        shared = {}

        def future(c, s):
            ts = int((c.wall + 60.0) * 1000)
            return {"net_mode": [{"ts": ts, "value": "sta"}],
                    "uart_baudrate": [{"ts": ts, "value": "115200"}],
                    "sta_has_ip": [{"ts": ts, "value": "true"}]}

        sess = FakeSess(clock, shared, make_handler())
        obs = FakeObserver(clock, shared, post=future)
        deps = r.Deps(sess, obs, clock.time, clock.now, clock.sleep)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual(result["verdict"], "NOT_COVERED")
        self.assertGreaterEqual(result["window"]["future_dropped"], 1)

    def test_each_telemetry_key_missing_is_not_covered(self):
        for missing_key in p.TELEMETRY_KEYS:
            with self.subTest(missing_key=missing_key):
                clock = Clock()
                shared = {}

                def post(c, s, missing=missing_key):
                    ts = int((s["op_start"] + 2.0) * 1000)
                    values = {"net_mode": "sta", "uart_baudrate": "115200",
                              "sta_has_ip": "true"}
                    return {k: [{"ts": ts, "value": values[k]}]
                            for k in values if k != missing}

                sess = FakeSess(clock, shared, make_handler())
                obs = FakeObserver(clock, shared, post=post)
                deps = r.Deps(sess, obs, clock.time, clock.now, clock.sleep)
                result = run(r.run_case(deps, "ssid"))
                self.assertEqual(
                    (result["verdict"], result["reason"]),
                    ("NOT_COVERED", "telemetry_no_sample_" + missing_key))


class TelemetryTests(unittest.TestCase):
    def _run_with(self, **values):
        clock = Clock()
        shared = {}

        def post(c, s):
            ts = int((s["op_start"] + 2.0) * 1000)
            base = {"net_mode": "sta", "uart_baudrate": "115200",
                    "sta_has_ip": "true"}
            base.update(values)
            return {k: [{"ts": ts, "value": values[k]}] if k in values
                    else [{"ts": ts, "value": base[k]}]
                    for k in ("net_mode", "uart_baudrate", "sta_has_ip")}

        sess = FakeSess(clock, shared, make_handler())
        obs = FakeObserver(clock, shared, post=post)
        deps = r.Deps(sess, obs, clock.time, clock.now, clock.sleep)
        return run(r.run_case(deps, "ssid"))

    def test_net_mode_ap(self):
        result = self._run_with(net_mode="ap")
        self.assertEqual((result["verdict"], result["reason"]),
                         ("FAIL", "telemetry_inconsistent"))

    def test_baudrate_zero(self):
        result = self._run_with(uart_baudrate=0)
        self.assertEqual(result["reason"], "telemetry_inconsistent")

    def test_baudrate_text(self):
        result = self._run_with(uart_baudrate="abc")
        self.assertEqual(result["reason"], "telemetry_inconsistent")

    def test_sta_has_ip_false(self):
        result = self._run_with(sta_has_ip="false")
        self.assertEqual(result["reason"], "telemetry_inconsistent")


class StatusTests(unittest.TestCase):
    def test_ble_status_conflict_is_fail(self):
        clock = Clock()
        shared = {}
        bad = dict(READY, sta_has_ip=False)
        sess = FakeSess(clock, shared, make_handler(post_status=bad))
        obs = FakeObserver(clock, shared)
        deps = r.Deps(sess, obs, clock.time, clock.now, clock.sleep)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("FAIL", "status_inconsistent"))
        self.assertIn("status_sta_has_ip_not_true", result["status_errors"])

    def test_ble_net_mode_conflict_is_fail(self):
        clock = Clock()
        shared = {}
        bad = dict(READY, net_mode="ap")
        sess = FakeSess(clock, shared, make_handler(post_status=bad))
        obs = FakeObserver(clock, shared)
        deps = r.Deps(sess, obs, clock.time, clock.now, clock.sleep)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual(result["reason"], "status_inconsistent")


class OperationTests(unittest.TestCase):
    def test_terminal_success_is_fail(self):
        deps, _, _, _, _ = make_deps(make_handler(
            terminal={"state": "success", "error": "none"}))
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("FAIL", "terminal_state"))

    def test_terminal_never_reached_is_fail(self):
        deps, sess, _, _, _ = make_deps(make_handler(
            terminal={"state": "accepted"}))
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("FAIL", "terminal_state"))
        self.assertEqual(sess.writes, 1)

    def test_busy_not_covered(self):
        busy = {"ok": False, "error": {"code": "busy"}}
        deps, sess, _, _, _ = make_deps(make_handler(tryconnect=busy))
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("NOT_COVERED", "tryconnect_busy"))
        self.assertEqual(sess.writes, 1)

    def test_identity_after_mismatch(self):
        deps, _, _, _, _ = make_deps(make_handler(
            identity_after=dict(IDENT, device_id="Other")))
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("FAIL", "identity_mismatch"))

    def test_identity_mismatch_blocks_write_and_observer(self):
        deps, sess, obs, _, _ = make_deps(make_handler(
            identity=dict(IDENT, firmware="other")))
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("STOP", "identity"))
        self.assertEqual(sess.writes, 0)
        self.assertFalse(obs.started)


class ObserverFailureTests(unittest.TestCase):
    def test_start_auth_error_blocks_write(self):
        clock = Clock()
        shared = {}
        sess = FakeSess(clock, shared, make_handler())
        obs = FakeObserver(clock, shared,
                           start_error=rest.ObserverError("auth"))
        deps = r.Deps(sess, obs, clock.time, clock.now, clock.sleep)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("STOP", "observer_auth"))
        self.assertEqual(sess.writes, 0)

    def test_post_fetch_errors_stop_without_fail(self):
        cases = {"auth": 401, "forbidden": 403, "not_found": 404,
                 "rate_limited": 429, "server": 500, "timeout": None}
        for code in cases:
            with self.subTest(code=code):
                clock = Clock()
                shared = {}
                sess = FakeSess(clock, shared, make_handler())
                obs = FakeObserver(clock, shared,
                                   series_error=rest.ObserverError(code))
                deps = r.Deps(sess, obs, clock.time, clock.now, clock.sleep)
                result = run(r.run_case(deps, "ssid"))
                self.assertEqual((result["verdict"], result["reason"]),
                                 ("STOP", "observer_" + code))
                self.assertEqual(sess.writes, 1)
                self.assertEqual(result["operation"]["terminal"]["state"],
                                 "failed")

    def test_attributes_error_is_observer_stop(self):
        clock = Clock()
        shared = {}
        sess = FakeSess(clock, shared, make_handler())
        obs = FakeObserver(clock, shared,
                           attrs_error=rest.ObserverError("forbidden"))
        deps = r.Deps(sess, obs, clock.time, clock.now, clock.sleep)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("STOP", "observer_forbidden"))

    def test_malformed_post_series_is_protocol_stop(self):
        clock = Clock()
        shared = {}
        sess = FakeSess(clock, shared, make_handler())
        obs = FakeObserver(clock, shared, post=lambda c, s: [])
        deps = r.Deps(sess, obs, clock.time, clock.now, clock.sleep)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("STOP", "observer_protocol"))


class DeviceFailureTests(unittest.TestCase):
    def test_post_status_timeout_is_device_observation(self):
        deps, sess, _, _, _ = make_deps(
            make_handler(fail_after_write=("network.status",)))
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("STOP", "device_observation_read_timeout"))
        self.assertEqual(sess.writes, 1)
        self.assertEqual(result["operation"]["terminal"]["state"], "failed")

    def test_operation_get_timeout_is_device_observation(self):
        deps, sess, _, _, _ = make_deps(
            make_handler(fail_after_write=("operation.get",)))
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual(result["reason"], "device_observation_read_timeout")
        self.assertEqual(sess.writes, 1)

    def test_malformed_identity_response(self):
        def handler(req, clock, shared):
            if req["cmd"] == "device.status":
                return {"ok": True}
            return make_handler()(req, clock, shared)

        deps, sess, _, _, _ = make_deps(handler)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual(result["reason"],
                         "device_observation_malformed_response")
        self.assertEqual(sess.writes, 0)


class WriteGateTests(unittest.TestCase):
    def test_transport_exception_not_resent(self):
        def tryconnect(req, clock, shared):
            raise RuntimeError("transport down")

        deps, sess, _, _, _ = make_deps(make_handler(tryconnect=tryconnect))
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("STOP", "write_outcome_unknown"))
        self.assertEqual(sess.writes, 1)

    def test_write_deadline_exhausted_before_send(self):
        deps, sess, _, _, _ = make_deps(write_timeout=0.0)
        result = run(r.run_case(deps, "ssid"))
        self.assertEqual(
            (result["verdict"], result["reason"]),
            ("STOP", "write_deadline_exhausted_before_send"))
        self.assertEqual(sess.writes, 0)


class ObserverMappingTests(unittest.TestCase):
    class Transport:
        def __init__(self, status, body):
            self.status = status
            self.body = body
            self.paths = []
            self.tokens = []

        async def request(self, method, path, token=None, body=None):
            self.paths.append((method, path))
            self.tokens.append(token)
            return self.status, self.body

    def test_login_uses_token(self):
        transport = self.Transport(200, {"token": "jwt"})
        obs = rest.TbRestObserver(transport, "dev", "u", "p")
        run(obs.start())
        run(obs.fetch_series(("net_mode",), 0, 1))
        self.assertEqual(transport.paths[0][0], "POST")
        self.assertEqual(transport.tokens[-1], "jwt")

    def test_status_mapping(self):
        cases = {401: "auth", 403: "forbidden", 404: "not_found",
                 429: "rate_limited", 500: "server", 502: "server",
                 418: "http_418"}
        for status, code in cases.items():
            with self.subTest(status=status):
                obs = rest.TbRestObserver(self.Transport(status, {}),
                                          "dev", "u", "p")
                with self.assertRaises(rest.ObserverError) as ctx:
                    run(obs.start())
                self.assertEqual(ctx.exception.code, code)

    def test_login_without_token_is_protocol(self):
        obs = rest.TbRestObserver(self.Transport(200, {}), "dev", "u", "p")
        with self.assertRaises(rest.ObserverError) as ctx:
            run(obs.start())
        self.assertEqual(ctx.exception.code, "protocol")

    def test_series_not_object_is_protocol(self):
        class T:
            async def request(self, method, path, token=None, body=None):
                return 200, {"token": "jwt"} if path.endswith("/login") else []

        obs = rest.TbRestObserver(T(), "dev", "u", "p")
        run(obs.start())
        with self.assertRaises(rest.ObserverError) as ctx:
            run(obs.fetch_series(("net_mode",), 0, 1))
        self.assertEqual(ctx.exception.code, "protocol")


class SourceIsolationTests(unittest.TestCase):
    def test_no_mqtt_or_device_token_logic(self):
        for name in ("c152_telemetry_rest.py", "c152_rest_observer.py"):
            text = (ROOT / "tools" / "review" / name).read_text(encoding="utf-8")
            low = text.lower()
            self.assertNotIn("paho", low, name)
            self.assertNotIn("iothub_mqtt_token", low, name)
            self.assertNotIn("v1/devices/me", low, name)

    def test_exit_codes(self):
        self.assertEqual(p.exit_code("PASS"), 0)
        self.assertEqual(p.exit_code("FAIL"), 1)
        self.assertEqual(p.exit_code("NOT_COVERED"), 2)
        self.assertEqual(p.exit_code("STOP"), 3)


if __name__ == "__main__":
    unittest.main()
