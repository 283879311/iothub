"""Host tests for C15-2 evidence freshness and REST/BLE identity binding."""

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / "tools" / "review"))
import c152_rest_observer as rest  # noqa: E402
import c152_telemetry_rest as r  # noqa: E402
from test_c152_runner import (Clock, FakeObserver, FakeSess,  # noqa: E402
                              make_handler, run)


class FreshnessTests(unittest.TestCase):
    VALUES = {"net_mode": "sta", "uart_baudrate": "115200",
              "sta_has_ip": "true"}

    def _run(self, baseline_factory, post_factory):
        clock = Clock()
        shared = {}
        sess = FakeSess(clock, shared, make_handler())
        obs = FakeObserver(clock, shared, baseline=baseline_factory,
                           post=post_factory)
        deps = r.Deps(sess, obs, clock.time, clock.now, clock.sleep)
        return run(r.run_case(deps, "ssid")), sess

    @staticmethod
    def _series(ts):
        return {k: [{"ts": ts, "value": v}]
                for k, v in FreshnessTests.VALUES.items()}

    def test_pre_op_sample_reused_is_not_fresh(self):
        def baseline(clock, shared):
            shared["base_ts"] = int((clock.wall - 3.0) * 1000)
            return self._series(shared["base_ts"])

        def post(clock, shared):
            return self._series(shared["base_ts"])

        result, sess = self._run(baseline, post)
        self.assertEqual((result["verdict"], result["reason"]),
                         ("NOT_COVERED",
                          "telemetry_no_fresh_sample_net_mode"))
        self.assertEqual(sess.writes, 1)
        self.assertEqual(result["window"]["missing"], [])

    def test_sample_one_ms_after_baseline_is_fresh(self):
        def baseline(clock, shared):
            shared["base_ts"] = int((clock.wall - 3.0) * 1000)
            return self._series(shared["base_ts"])

        def post(clock, shared):
            return self._series(shared["base_ts"] + 1)

        result, _ = self._run(baseline, post)
        self.assertEqual((result["verdict"], result["reason"]), ("PASS", "ok"))

    def test_server_clock_behind_host_still_fresh(self):
        def baseline(clock, shared):
            shared["base_ts"] = int((clock.wall - 20.0) * 1000)
            return self._series(shared["base_ts"])

        def post(clock, shared):
            # server timestamp is still 8 s "before" host op_start
            return self._series(shared["base_ts"] + 12_000)

        result, _ = self._run(baseline, post)
        self.assertEqual((result["verdict"], result["reason"]), ("PASS", "ok"))


class RestIdentityTests(unittest.TestCase):
    def _run(self, observer):
        clock = Clock()
        shared = {}
        sess = FakeSess(clock, shared, make_handler())
        deps = r.Deps(sess, observer(clock, shared), clock.time, clock.now,
                      clock.sleep)
        return run(r.run_case(deps, "ssid")), sess

    def test_name_mismatch_blocks_write(self):
        result, sess = self._run(lambda c, s: FakeObserver(
            c, s, device={"id": {"id": "dev-uuid"}, "name": "Other"}))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("STOP", "observer_identity_mismatch"))
        self.assertEqual(sess.writes, 0)
        self.assertIn("rest_device_name_mismatch",
                      result["observer"]["identity_errors"])

    def test_uuid_mismatch_blocks_write(self):
        result, sess = self._run(lambda c, s: FakeObserver(
            c, s, device={"id": {"id": "other-uuid"},
                          "name": "IotHub-65D8"}))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("STOP", "observer_identity_mismatch"))
        self.assertEqual(sess.writes, 0)
        self.assertIn("rest_device_id_mismatch",
                      result["observer"]["identity_errors"])

    def test_device_fetch_error_blocks_write(self):
        result, sess = self._run(lambda c, s: FakeObserver(
            c, s, device_error=rest.ObserverError("forbidden")))
        self.assertEqual((result["verdict"], result["reason"]),
                         ("STOP", "observer_forbidden"))
        self.assertEqual(sess.writes, 0)


if __name__ == "__main__":
    unittest.main()
