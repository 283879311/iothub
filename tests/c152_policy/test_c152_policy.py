"""Host tests for the C15-2 Telemetry REST + Attributes + BLE policy."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "review"))
import c152_policy as p  # noqa: E402

T0 = 1_790_000_000.0  # epoch seconds (2026-09-22); ms form exceeds 1e11


def point(ts_seconds, value):
    return {"ts": int(ts_seconds * 1000), "value": value}


def series(**over):
    base = {
        "net_mode": [point(T0, "sta")],
        "uart_baudrate": [point(T0, "115200")],
        "sta_has_ip": [point(T0, "true")],
    }
    base.update(over)
    return base


def attr(key, value, ts=T0):
    return {"key": key, "value": value, "lastUpdateTs": int(ts * 1000)}


IDENT = {"device_id": "IotHub-65D8", "firmware": "1.3-956afcd",
         "build_sha": "c231c9501"}


class ParseSeriesTests(unittest.TestCase):
    def test_valid_and_sorted(self):
        raw = {"net_mode": [point(T0 + 3, "sta"), point(T0 + 1, "sta")],
               "uart_baudrate": [point(T0 + 1, "115200")],
               "sta_has_ip": [point(T0 + 1, "true")]}
        out, errors = p.parse_series(raw)
        self.assertEqual(errors, [])
        self.assertEqual([s["ts"] for s in out["net_mode"]], [T0 + 1, T0 + 3])

    def test_missing_key_is_not_protocol_error(self):
        out, errors = p.parse_series({"net_mode": [point(T0, "sta")]})
        self.assertEqual(errors, [])
        self.assertEqual(out.get("uart_baudrate"), [])

    def test_not_object(self):
        self.assertEqual(p.parse_series([]), ({}, ["series_not_object"]))

    def test_malformed_points(self):
        raw = {"net_mode": ["x", {"ts": 0, "value": "sta"}, {"ts": 1_790_000_000_000}]}
        _, errors = p.parse_series(raw)
        self.assertIn("point_not_object:net_mode", errors)
        self.assertIn("ts_invalid:net_mode", errors)
        self.assertIn("value_missing:net_mode", errors)

    def test_seconds_timestamp_accepted(self):
        out, _ = p.parse_series({"net_mode": [{"ts": T0, "value": "sta"}]})
        self.assertEqual(out["net_mode"][0]["ts"], T0)


class ParseAttributesTests(unittest.TestCase):
    def test_valid(self):
        out, errors = p.parse_attributes([attr("sta_ssid", "ssid"),
                                          attr("sta_ip", "10.0.0.2")])
        self.assertEqual(errors, [])
        self.assertEqual(out["sta_ssid"]["value"], "ssid")
        self.assertEqual(out["sta_ssid"]["last_update_ts"], T0)

    def test_not_list(self):
        self.assertEqual(p.parse_attributes({}), ({}, ["attributes_not_list"]))

    def test_bad_item_and_ts_missing(self):
        out, errors = p.parse_attributes(["x", {"key": "sta_ssid", "value": "s"}])
        self.assertIn("attribute_not_object", errors)
        self.assertIsNone(out["sta_ssid"]["last_update_ts"])


class BaselineTests(unittest.TestCase):
    def test_ok(self):
        s, _ = p.parse_series(series())
        baseline, reason = p.baseline_check(s, T0 + 20)
        self.assertIsNone(reason)
        self.assertEqual(set(baseline), set(p.TELEMETRY_KEYS))

    def test_missing_key(self):
        s, _ = p.parse_series({"net_mode": [point(T0, "sta")],
                               "uart_baudrate": [point(T0, "115200")]})
        baseline, reason = p.baseline_check(s, T0 + 20)
        self.assertIsNone(baseline)
        self.assertEqual(reason, "baseline_missing_sta_has_ip")

    def test_stale(self):
        old = T0 - 100
        s, _ = p.parse_series(series(
            net_mode=[point(old, "sta")],
            uart_baudrate=[point(old, "115200")],
            sta_has_ip=[point(old, "true")]))
        _, reason = p.baseline_check(s, T0 + 20)
        self.assertEqual(reason, "baseline_stale_net_mode")

    def test_latest_before_ignores_future(self):
        s, _ = p.parse_series(series(
            net_mode=[point(T0, "sta"), point(T0 + 100, "ap")]))
        sample = p.latest_before(s, "net_mode", T0 + 20)
        self.assertEqual(sample["ts"], T0)


class FreshnessTests(unittest.TestCase):
    def baseline(self, ts=T0):
        return {k: {"ts": ts, "value": "x"} for k in p.TELEMETRY_KEYS}

    def test_only_samples_after_baseline_are_fresh(self):
        s, _ = p.parse_series(series(
            net_mode=[point(T0, "sta"), point(T0 + 1, "sta"),
                      point(T0 + 100, "sta")]))
        fresh = p.fresh_window_samples(s, self.baseline(T0), elapsed=50)
        self.assertEqual([x["ts"] for x in fresh["net_mode"]], [T0 + 1])

    def test_baseline_sample_excluded(self):
        s, _ = p.parse_series(series(net_mode=[point(T0, "sta")]))
        fresh = p.fresh_window_samples(s, self.baseline(T0), elapsed=50)
        self.assertEqual(fresh["net_mode"], [])

    def test_upper_bound_from_elapsed_and_delay(self):
        s, _ = p.parse_series(series(net_mode=[point(T0 + 10, "sta"),
                                               point(T0 + 10.001, "sta")]))
        fresh = p.fresh_window_samples(s, self.baseline(T0), elapsed=0,
                                       max_delay=10, skew=0)
        self.assertEqual([x["ts"] for x in fresh["net_mode"]], [T0 + 10])

    def test_classify_missing_vs_stale(self):
        fresh = {"net_mode": [], "uart_baudrate": [], "sta_has_ip": []}
        missing, stale = p.classify_freshness(
            {"net_mode": [point(T0, "sta")],
             "uart_baudrate": [point(T0, "115200")],
             "sta_has_ip": []}, fresh)
        self.assertEqual(missing, ["sta_has_ip"])
        self.assertEqual(stale, ["net_mode", "uart_baudrate"])


class RestIdentityTests(unittest.TestCase):
    DEVICE = {"id": {"id": "uuid-1"}, "name": "Test Device C1"}

    def test_ok(self):
        self.assertEqual(
            p.rest_identity_errors(self.DEVICE, "uuid-1", "Test Device C1"), [])

    def test_uuid_mismatch(self):
        self.assertEqual(
            p.rest_identity_errors(self.DEVICE, "uuid-2", "Test Device C1"),
            ["rest_device_id_mismatch"])

    def test_name_missing_and_mismatch(self):
        self.assertIn("rest_device_name_missing",
                      p.rest_identity_errors({"id": {"id": "uuid-1"}},
                                             "uuid-1", "X"))
        self.assertEqual(
            p.rest_identity_errors(self.DEVICE, "uuid-1", "Other"),
            ["rest_device_name_mismatch"])

    def test_unreadable(self):
        self.assertEqual(p.rest_identity_errors(None, "uuid-1", "X"),
                         ["rest_device_unreadable"])

    def test_binding_kind(self):
        self.assertEqual(p.binding_kind("IotHub-65D8", "IotHub-65D8", False),
                         "same_name")
        self.assertIsNone(p.binding_kind("Test Device C1", "IotHub-65D8", False))
        self.assertEqual(
            p.binding_kind("Test Device C1", "IotHub-65D8", True),
            "operator_alias")


class WindowTests(unittest.TestCase):
    def test_boundaries_inclusive(self):
        s, _ = p.parse_series(series(net_mode=[point(T0, "sta"),
                                               point(T0 + 1, "sta"),
                                               point(T0 + 2, "ap")]))
        win = p.window_samples(s, T0, T0 + 1)
        self.assertEqual([x["ts"] for x in win["net_mode"]], [T0, T0 + 1])

    def test_missing_keys(self):
        win = p.window_samples({"net_mode": []}, T0, T0 + 10)
        self.assertEqual(p.missing_window_keys(win),
                         ["net_mode", "uart_baudrate", "sta_has_ip"])

    def test_drop_future(self):
        s, _ = p.parse_series(series(net_mode=[point(T0, "sta"),
                                               point(T0 + 100, "sta")]))
        clean, dropped = p.drop_future(s, T0 + 0.5)
        self.assertEqual(dropped, 1)
        self.assertEqual([x["ts"] for x in clean["net_mode"]], [T0])


class TelemetryViolationTests(unittest.TestCase):
    def test_valid(self):
        win = p.window_samples(p.parse_series(series())[0], T0 - 1, T0 + 1)
        self.assertEqual(p.telemetry_violations(win), [])

    def test_net_mode_ap_and_invalid(self):
        win = p.window_samples(p.parse_series(series(
            net_mode=[point(T0, "ap"), point(T0 + 1, "mesh")]))[0],
            T0 - 1, T0 + 2)
        errors = [e["error"] for e in p.telemetry_violations(win)]
        self.assertEqual(errors, ["net_mode_not_sta", "net_mode_invalid"])

    def test_baudrate_zero_and_text(self):
        win = p.window_samples(p.parse_series(series(
            uart_baudrate=[point(T0, 0), point(T0 + 1, "abc")]))[0],
            T0 - 1, T0 + 2)
        errors = [e["error"] for e in p.telemetry_violations(win)]
        self.assertEqual(errors, ["uart_baudrate_invalid",
                                  "uart_baudrate_invalid"])

    def test_baudrate_bool_rejected(self):
        win = p.window_samples(p.parse_series(series(
            uart_baudrate=[point(T0, True)]))[0], T0 - 1, T0 + 1)
        self.assertEqual(p.telemetry_violations(win)[0]["error"],
                         "uart_baudrate_invalid")

    def test_sta_has_ip_false_and_invalid(self):
        win = p.window_samples(p.parse_series(series(
            sta_has_ip=[point(T0, "false"), point(T0 + 1, "yes")]))[0],
            T0 - 1, T0 + 2)
        errors = [e["error"] for e in p.telemetry_violations(win)]
        self.assertEqual(errors, ["sta_has_ip_not_true", "sta_has_ip_invalid"])


class StatusTests(unittest.TestCase):
    STATUS = {"net_mode": "sta", "sta_connected": True, "sta_has_ip": True,
              "sta_ip": "10.0.0.2", "sta_ssid": "ssid"}

    def test_ok(self):
        self.assertEqual(p.status_errors(self.STATUS), [])

    def test_fields(self):
        self.assertIn("status_net_mode_not_sta",
                      p.status_errors(dict(self.STATUS, net_mode="ap")))
        self.assertIn("status_sta_not_connected",
                      p.status_errors(dict(self.STATUS, sta_connected=False)))
        self.assertIn("status_sta_has_ip_not_true",
                      p.status_errors(dict(self.STATUS, sta_has_ip=False)))
        self.assertIn("status_sta_ip_empty",
                      p.status_errors(dict(self.STATUS, sta_ip="")))
        self.assertIn("status_sta_ssid_empty",
                      p.status_errors(dict(self.STATUS, sta_ssid="")))

    def test_not_object(self):
        self.assertEqual(p.status_errors(None), ["status_not_object"])


class AttributeTests(unittest.TestCase):
    STATUS = {"sta_ssid": "ssid"}

    def test_ok(self):
        attrs = {"sta_ssid": {"value": "ssid", "last_update_ts": T0}}
        self.assertEqual(p.attributes_errors(attrs, self.STATUS), [])

    def test_missing_empty_mismatch(self):
        self.assertEqual(p.attributes_errors({}, self.STATUS),
                         ["sta_ssid_missing"])
        self.assertEqual(
            p.attributes_errors({"sta_ssid": {"value": ""}}, self.STATUS),
            ["sta_ssid_empty"])
        self.assertEqual(
            p.attributes_errors({"sta_ssid": {"value": "other"}},
                                self.STATUS),
            ["sta_ssid_mismatch"])

    def test_sta_ip_difference_is_not_an_error(self):
        attrs = {"sta_ssid": {"value": "ssid"},
                 "sta_ip": {"value": "192.168.1.9"}}
        self.assertEqual(p.attributes_errors(attrs, self.STATUS), [])

    def test_staleness_records_age(self):
        attrs = {"sta_ssid": {"value": "ssid", "last_update_ts": T0}}
        out = p.attributes_staleness(attrs, T0 + 900)
        self.assertTrue(out["sta_ssid"]["stale"])
        self.assertEqual(out["sta_ssid"]["age_s"], 900.0)

    def test_staleness_missing_ts(self):
        out = p.attributes_staleness(
            {"sta_ssid": {"value": "ssid", "last_update_ts": None}}, T0)
        self.assertIsNone(out["sta_ssid"]["stale"])


class IdentityTests(unittest.TestCase):
    def test_ok(self):
        self.assertEqual(p.identity_errors(IDENT, IDENT), [])

    def test_mismatch_and_missing(self):
        self.assertIn("device_id_mismatch",
                      p.identity_errors(dict(IDENT, device_id="Other"), IDENT))
        bad = dict(IDENT)
        del bad["build_sha"]
        self.assertIn("build_sha_missing", p.identity_errors(bad, IDENT))

    def test_unreadable(self):
        self.assertEqual(p.identity_errors(None, IDENT), ["identity_unreadable"])


class DecisionTests(unittest.TestCase):
    def pass_kwargs(self, **over):
        kwargs = dict(terminal_state="failed", terminal_error="wrong_password",
                      sta_restored=True, identity_after_ok=True,
                      missing_keys=[], stale_keys=[], telemetry_errors=[],
                      status_errors_list=[], attr_errors=[])
        kwargs.update(over)
        return kwargs

    def test_pass(self):
        self.assertEqual(p.decide(**self.pass_kwargs()), ("PASS", "ok"))

    def test_observer_error_is_stop(self):
        verdict, reason = p.decide(**self.pass_kwargs(observer_error="auth"))
        self.assertEqual((verdict, reason), ("STOP", "observer_auth"))

    def test_device_error_is_stop(self):
        verdict, reason = p.decide(**self.pass_kwargs(
            device_error="read_timeout", observer_error=None))
        self.assertEqual((verdict, reason),
                         ("STOP", "device_observation_read_timeout"))

    def test_observer_beats_operation_failure(self):
        verdict, _ = p.decide(**self.pass_kwargs(
            observer_error="server", terminal_state="success",
            terminal_error=None))
        self.assertEqual(verdict, "STOP")

    def test_busy_not_covered(self):
        self.assertEqual(p.decide(**self.pass_kwargs(busy=True)),
                         ("NOT_COVERED", "tryconnect_busy"))

    def test_terminal_and_restore(self):
        self.assertEqual(
            p.decide(**self.pass_kwargs(terminal_state="success",
                                        terminal_error=None)),
            ("FAIL", "terminal_state"))
        self.assertEqual(
            p.decide(**self.pass_kwargs(sta_restored=False)),
            ("FAIL", "sta_not_restored"))
        self.assertEqual(
            p.decide(**self.pass_kwargs(identity_after_ok=False)),
            ("FAIL", "identity_mismatch"))

    def test_missing_sample_is_not_covered(self):
        self.assertEqual(
            p.decide(**self.pass_kwargs(missing_keys=["uart_baudrate"])),
            ("NOT_COVERED", "telemetry_no_sample_uart_baudrate"))

    def test_stale_sample_is_not_covered(self):
        self.assertEqual(
            p.decide(**self.pass_kwargs(stale_keys=["net_mode"])),
            ("NOT_COVERED", "telemetry_no_fresh_sample_net_mode"))

    def test_evidence_failures(self):
        self.assertEqual(
            p.decide(**self.pass_kwargs(telemetry_errors=[{"error": "x"}])),
            ("FAIL", "telemetry_inconsistent"))
        self.assertEqual(
            p.decide(**self.pass_kwargs(status_errors_list=["x"])),
            ("FAIL", "status_inconsistent"))
        self.assertEqual(
            p.decide(**self.pass_kwargs(attr_errors=["x"])),
            ("FAIL", "attributes_inconsistent"))

    def test_write_gates(self):
        self.assertEqual(
            p.decide(**self.pass_kwargs(write_deadline=True)),
            ("STOP", "write_deadline_exhausted_before_send"))
        self.assertEqual(
            p.decide(**self.pass_kwargs(write_unknown=True)),
            ("STOP", "write_outcome_unknown"))


class MiscTests(unittest.TestCase):
    def test_retry_boundaries(self):
        self.assertTrue(p.should_retry_read(1))
        self.assertTrue(p.should_retry_read(2))
        self.assertFalse(p.should_retry_read(3))

    def test_constants(self):
        self.assertEqual(p.READY_STABLE_S, 10.0)
        self.assertEqual(p.BASELINE_MAX_AGE_S, 30.0)
        self.assertEqual(p.MAX_DATA_DELAY_S, 15.0)
        self.assertEqual(p.TELEMETRY_KEYS,
                         ("net_mode", "uart_baudrate", "sta_has_ip"))

    def test_exit_codes(self):
        self.assertEqual(p.exit_code("PASS"), 0)
        self.assertEqual(p.exit_code("FAIL"), 1)
        self.assertEqual(p.exit_code("NOT_COVERED"), 2)
        self.assertEqual(p.exit_code("STOP"), 3)
        self.assertEqual(p.exit_code("unknown"), 3)


if __name__ == "__main__":
    unittest.main()
