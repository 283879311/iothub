"""Host tests for the C15-2 attributes-consistency policy (no device, no MQTT)."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "review"))
import c152_policy as p  # noqa: E402


def sample(ts, **fields):
    base = {"net_mode": "sta", "sta_ssid": "ssid", "sta_ip": "10.0.0.2",
            "uart_baudrate": 115200}
    base.update(fields)
    return {"ts": ts, "obj": base}


def attrs(**fields):
    return sample(0, **fields)["obj"]


IDENT = {"device_id": "IotHub-65D8", "firmware": "1.3-956afcd",
         "build_sha": "c231c9501"}


class AttrSampleTests(unittest.TestCase):
    def test_sta_valid(self):
        self.assertEqual(p.attr_sample_violations(attrs()), [])

    def test_ap_is_not_runtime_mode(self):
        self.assertEqual(p.attr_sample_violations(attrs(net_mode="ap", sta_ssid="")), ["net_mode_not_sta"])

    def test_apsta_is_not_runtime_mode(self):
        self.assertEqual(p.attr_sample_violations(attrs(net_mode="apsta", sta_ip="1.2.3.4")), ["net_mode_not_sta"])

    def test_sta_without_ssid_is_violation(self):
        self.assertEqual(p.attr_sample_violations(attrs(sta_ssid="")), ["sta_mode_without_ssid"])

    def test_invalid_mode(self):
        self.assertEqual(p.attr_sample_violations(attrs(net_mode="mesh")), ["net_mode_invalid"])

    def test_missing_baudrate(self):
        obj = attrs()
        del obj["uart_baudrate"]
        self.assertEqual(p.attr_sample_violations(obj), ["uart_baudrate_missing"])

    def test_invalid_baudrate(self):
        self.assertIn("uart_baudrate_invalid",
                      p.attr_sample_violations(attrs(uart_baudrate=0)))

    def test_non_object_sample(self):
        self.assertEqual(p.attr_sample_violations(None), ["sample_not_object"])


class WindowTests(unittest.TestCase):
    def test_zero_samples(self):
        win = p.window_eval([], 10.0, 20.0)
        self.assertEqual(win["n_attr"], 0)
        self.assertEqual(p.decide(n_attr=win["n_attr"])[0], "NOT_COVERED")

    def test_samples_outside_window_ignored(self):
        self.assertEqual(p.window_eval([sample(5), sample(25)], 10.0, 20.0)["n_attr"], 0)

    def test_boundaries_inclusive_and_last(self):
        win = p.window_eval([sample(10), sample(15, sta_ip="10.0.0.3")], 10.0, 20.0)
        self.assertEqual(win["n_attr"], 2)
        self.assertEqual(win["last"]["sta_ip"], "10.0.0.3")
        self.assertEqual(win["last_ts"], 15.0)

    def test_violation_makes_fail(self):
        win = p.window_eval([sample(11, net_mode="ap", sta_ssid="")], 10.0, 20.0)
        verdict, reason = p.decide(
            n_attr=win["n_attr"], violations=win["violations"],
            terminal_state="failed", terminal_error="wrong_password",
            sta_restored=True, identity_ok=True)
        self.assertEqual((verdict, reason), ("FAIL", "attribute_inconsistent"))


class CrosscheckTests(unittest.TestCase):
    def test_ok(self):
        last = {"sta_ip": "10.0.0.2"}
        status = {"sta_has_ip": True, "sta_ip": "10.0.0.2"}
        self.assertEqual(p.crosscheck(last, 10.0, status, 12.0), [])

    def test_status_before_last_attr(self):
        self.assertIn("status_not_after_last_attr", p.crosscheck(
            {"sta_ip": "10.0.0.2"}, 12.0,
            {"sta_has_ip": True, "sta_ip": "10.0.0.2"}, 11.0))

    def test_sta_has_ip_false(self):
        self.assertIn("sta_has_ip_not_true", p.crosscheck(
            {"sta_ip": "10.0.0.2"}, 10.0,
            {"sta_has_ip": False, "sta_ip": "10.0.0.2"}, 12.0))

    def test_empty_attr_ip_is_rejected(self):
        self.assertIn("attr_sta_ip_empty", p.crosscheck(
            {"sta_ip": ""}, 10.0,
            {"sta_has_ip": True, "sta_ip": ""}, 12.0))

    def test_empty_status_ip_is_rejected(self):
        self.assertIn("status_sta_ip_empty", p.crosscheck(
            {"sta_ip": "10.0.0.2"}, 10.0,
            {"sta_has_ip": True, "sta_ip": ""}, 12.0))

    def test_both_empty_is_not_a_match(self):
        errors = p.crosscheck({"sta_ip": ""}, 10.0,
                              {"sta_has_ip": True, "sta_ip": ""}, 12.0)
        self.assertIn("attr_sta_ip_empty", errors)
        self.assertIn("status_sta_ip_empty", errors)
        self.assertNotIn("sta_ip_mismatch", errors)

    def test_missing_inputs(self):
        self.assertEqual(p.crosscheck(None, None, {}, 1.0), ["no_attribute_sample"])
        self.assertEqual(p.crosscheck({"sta_ip": "x"}, 1.0, {}, None),
                         ["status_sample_missing"])


class IdentityTests(unittest.TestCase):
    def test_ok(self):
        self.assertEqual(p.identity_errors(IDENT, IDENT), [])

    def test_device_mismatch(self):
        dev = dict(IDENT, device_id="Other")
        self.assertIn("device_id_mismatch", p.identity_errors(dev, IDENT))

    def test_firmware_mismatch(self):
        dev = dict(IDENT, firmware="1.3-0000000")
        self.assertIn("firmware_mismatch", p.identity_errors(dev, IDENT))

    def test_build_sha_missing(self):
        dev = dict(IDENT)
        del dev["build_sha"]
        self.assertIn("build_sha_missing", p.identity_errors(dev, IDENT))

    def test_unreadable(self):
        self.assertEqual(p.identity_errors(None, IDENT), ["identity_unreadable"])


class SubackTests(unittest.TestCase):
    def test_empty_and_none(self):
        self.assertFalse(p.suback_ok(None)[0])
        self.assertFalse(p.suback_ok([])[0])

    def test_ok_and_rejected(self):
        self.assertTrue(p.suback_ok([0])[0])
        self.assertEqual(p.suback_ok([0x80]), (False, "rejected"))

    def test_unparsable(self):
        self.assertEqual(p.suback_ok(["x"]), (False, "unparsable"))


class TryconnectTests(unittest.TestCase):
    def test_classification(self):
        self.assertEqual(p.tryconnect_class(True, "op-1", None), "accepted")
        self.assertEqual(p.tryconnect_class(False, None, "busy"), "busy")
        self.assertEqual(p.tryconnect_class(False, None, "state"), "rejected")

    def test_busy_is_not_covered(self):
        self.assertEqual(p.decide(busy=True), ("NOT_COVERED", "tryconnect_busy"))


class TerminalTests(unittest.TestCase):
    def test_terminal_ok(self):
        self.assertTrue(p.terminal_ok("failed", "wrong_password"))
        self.assertTrue(p.terminal_ok("failed", "ap_not_found"))
        self.assertFalse(p.terminal_ok("failed", "timeout"))
        self.assertFalse(p.terminal_ok("success", None))


class DecisionTests(unittest.TestCase):
    def passed(self, **over):
        kwargs = dict(n_attr=2, violations=[], terminal_state="failed",
                      terminal_error="wrong_password", sta_restored=True,
                      identity_ok=True, cross_errors=[])
        kwargs.update(over)
        return p.decide(**kwargs)

    def test_pass(self):
        self.assertEqual(self.passed(), ("PASS", "ok"))

    def test_read_timeout_wins(self):
        self.assertEqual(p.decide(read_timeout=True), ("STOP", "read_timeout"))

    def test_failure_branches(self):
        self.assertEqual(self.passed(terminal_state="success")[0], "FAIL")
        self.assertEqual(self.passed(sta_restored=False)[0], "FAIL")
        self.assertEqual(self.passed(identity_ok=False)[0], "FAIL")
        self.assertEqual(self.passed(cross_errors=["x"])[0], "FAIL")


class MiscTests(unittest.TestCase):
    def test_retry_boundaries(self):
        self.assertTrue(p.should_retry_read(1))
        self.assertTrue(p.should_retry_read(2))
        self.assertFalse(p.should_retry_read(3))

    def test_constants(self):
        self.assertEqual(p.READ_MAX_ATTEMPTS, 3)
        self.assertEqual(p.READ_RETRY_DELAY_S, 2.0)
        self.assertEqual(p.READY_STABLE_S, 10.0)
        self.assertEqual(p.RUNTIME_NET_MODE, "sta")

    def test_exit_codes(self):
        self.assertEqual(p.exit_code("PASS"), 0)
        self.assertEqual(p.exit_code("FAIL"), 1)
        self.assertEqual(p.exit_code("NOT_COVERED"), 2)
        self.assertEqual(p.exit_code("STOP"), 3)


if __name__ == "__main__":
    unittest.main()
