"""C15-2 Telemetry REST + Attributes + BLE policy (pure, host-testable, no I/O).

Evidence sources (firmware 1.3-956afcd):
- telemetry `v1/devices/me/telemetry`: net_mode / uart_baudrate / sta_has_ip,
  published every ~10 s with fresh timestamps.
- attributes `v1/devices/me/attributes`: net_mode / sta_ssid / sta_ip /
  uart_baudrate; unchanged values may keep an old lastUpdateTs (inferred).
- BLE network.status: net_mode / sta_ssid / connected_ssid / sta_has_ip /
  sta_ip / sta_connected.
- BLE device.status: device_id / firmware / build_sha.
- BLE operation.get: state / error; terminal states success/failed/
  cancelled/expired.

This module never talks to MQTT; the device Access Token is not used.
"""

from __future__ import annotations

from typing import Any

NET_MODES = ("ap", "sta", "apsta")
RUNTIME_NET_MODE = "sta"

TELEMETRY_KEYS = ("net_mode", "uart_baudrate", "sta_has_ip")
ATTRIBUTE_KEYS = ("sta_ssid", "sta_ip")

READ_MAX_ATTEMPTS = 3
READ_RETRY_DELAY_S = 2.0
READY_STABLE_S = 10.0
READY_MAX_GAP_S = 3.0
TERMINAL_TIMEOUT_S = 40.0
RECOVERY_TIMEOUT_S = 60.0
TERMINAL_STATES = ("failed", "success", "cancelled", "expired")
TERMINAL_EXPECTED_ERRORS = ("wrong_password", "ap_not_found")

BASELINE_MAX_AGE_S = 30.0
CLOCK_SKEW_S = 5.0
MAX_DATA_DELAY_S = 15.0
ATTR_STALE_WARN_S = 600.0
SERIES_LIMIT = 500

IDENTITY_FIELDS = ("device_id", "firmware", "build_sha")
EXIT_CODES = {"PASS": 0, "FAIL": 1, "NOT_COVERED": 2, "STOP": 3}


def _nonempty_str(value: Any) -> bool:
    return isinstance(value, str) and value.strip() != ""


def _as_bool(value: Any) -> bool | None:
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        low = value.strip().lower()
        if low == "true":
            return True
        if low == "false":
            return False
    if isinstance(value, (int, float)) and value in (0, 1):
        return bool(value)
    return None


def _as_number(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str):
        try:
            return float(value.strip())
        except ValueError:
            return None
    return None


def _ts_seconds(value: Any) -> float | None:
    """TB timestamps are epoch milliseconds; accept seconds defensively."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    number = float(value)
    if number <= 0:
        return None
    return number / 1000.0 if number > 1e11 else number


def parse_series(raw: Any, keys=TELEMETRY_KEYS) -> tuple[dict, list[str]]:
    """Normalize a TB timeseries response.

    Returns ({key: [{"ts": seconds, "value": raw}]}, protocol_errors).
    Missing keys are not protocol errors (they become "no sample" later);
    malformed points are.
    """
    if not isinstance(raw, dict):
        return {}, ["series_not_object"]
    errors: list[str] = []
    series: dict[str, list[dict]] = {}
    for key in keys:
        points = raw.get(key, [])
        if not isinstance(points, list):
            errors.append("series_not_list:" + key)
            continue
        samples: list[dict] = []
        for point in points:
            if not isinstance(point, dict):
                errors.append("point_not_object:" + key)
                continue
            ts = _ts_seconds(point.get("ts"))
            if ts is None:
                errors.append("ts_invalid:" + key)
                continue
            if "value" not in point:
                errors.append("value_missing:" + key)
                continue
            samples.append({"ts": ts, "value": point.get("value")})
        samples.sort(key=lambda item: item["ts"])
        series[key] = samples
    return series, errors


def parse_attributes(raw: Any, keys=ATTRIBUTE_KEYS) -> tuple[dict, list[str]]:
    """Normalize a TB attributes response.

    Returns ({key: {"value": ..., "last_update_ts": seconds|None}},
    protocol_errors). Missing keys are left absent for the criterion layer.
    """
    if not isinstance(raw, list):
        return {}, ["attributes_not_list"]
    errors: list[str] = []
    out: dict[str, dict] = {}
    for item in raw:
        if not isinstance(item, dict):
            errors.append("attribute_not_object")
            continue
        key = item.get("key")
        if key not in keys:
            continue
        out[key] = {
            "value": item.get("value"),
            "last_update_ts": _ts_seconds(item.get("lastUpdateTs")),
        }
    return out, errors


def drop_future(series: dict, now: float, skew: float = CLOCK_SKEW_S
                ) -> tuple[dict, int]:
    """Remove samples timestamped after now+skew; returns (clean, dropped)."""
    clean: dict[str, list[dict]] = {}
    dropped = 0
    limit = now + skew
    for key, samples in series.items():
        kept = []
        for sample in samples:
            if sample["ts"] > limit:
                dropped += 1
            else:
                kept.append(sample)
        clean[key] = kept
    return clean, dropped


def latest_before(series: dict, key: str, moment: float) -> dict | None:
    candidates = [s for s in series.get(key, []) if s["ts"] <= moment]
    return candidates[-1] if candidates else None


def baseline_check(series: dict, op_start: float,
                   max_age: float = BASELINE_MAX_AGE_S
                   ) -> tuple[dict | None, str | None]:
    """Require one pre-operation sample per telemetry key with bounded age."""
    baseline: dict[str, dict] = {}
    for key in TELEMETRY_KEYS:
        sample = latest_before(series, key, op_start)
        if sample is None:
            return None, "baseline_missing_" + key
        if op_start - sample["ts"] > max_age:
            return None, "baseline_stale_" + key
        baseline[key] = sample
    return baseline, None


def window_samples(series: dict, w0: float, w1: float) -> dict:
    """Samples inside [w0, w1] inclusive, already time-sorted."""
    out: dict[str, list[dict]] = {}
    for key in TELEMETRY_KEYS:
        out[key] = [s for s in series.get(key, []) if w0 <= s["ts"] <= w1]
    return out


def fresh_window_samples(series: dict, baseline: dict | None, elapsed: float,
                         max_delay: float = MAX_DATA_DELAY_S,
                         skew: float = CLOCK_SKEW_S) -> dict:
    """Samples strictly newer than the per-key baseline.

    The upper bound is derived from the *measured* monotonic elapsed time
    between the baseline snapshot and the operation end, not from host wall
    time: `baseline_ts + elapsed + max_delay + skew`. This keeps freshness
    independent of any constant host/TB clock offset.

    Freshness itself is `ts > baseline[key].ts`; the baseline sample can never
    be reused as an in-window sample.
    """
    out: dict[str, list[dict]] = {}
    for key in TELEMETRY_KEYS:
        base = (baseline or {}).get(key)
        if base is None:
            out[key] = []
            continue
        w_end = base["ts"] + elapsed + max_delay + skew
        out[key] = [s for s in series.get(key, [])
                    if s["ts"] > base["ts"] and s["ts"] <= w_end]
    return out


def classify_freshness(series: dict, fresh: dict) -> tuple[list[str], list[str]]:
    """Split keys lacking a fresh sample into (no_sample, no_fresh).

    - no_sample: the key has no samples at all in the response.
    - no_fresh: samples exist but none is newer than the baseline (old or
      later than the allowed data-delay bound).
    """
    missing: list[str] = []
    stale: list[str] = []
    for key in TELEMETRY_KEYS:
        if fresh.get(key):
            continue
        if series.get(key):
            stale.append(key)
        else:
            missing.append(key)
    return missing, stale


def missing_window_keys(samples: dict) -> list[str]:
    return [key for key in TELEMETRY_KEYS if not samples.get(key)]


def telemetry_violations(samples: dict) -> list[dict]:
    """Per-sample self-consistency for the three telemetry fields."""
    errors: list[dict] = []
    for key in TELEMETRY_KEYS:
        for sample in samples.get(key, []):
            value = sample.get("value")
            error = None
            if key == "net_mode":
                if not isinstance(value, str) or value not in NET_MODES:
                    error = "net_mode_invalid"
                elif value != RUNTIME_NET_MODE:
                    error = "net_mode_not_sta"
            elif key == "uart_baudrate":
                number = _as_number(value)
                if number is None or number <= 0:
                    error = "uart_baudrate_invalid"
            elif key == "sta_has_ip":
                flag = _as_bool(value)
                if flag is None:
                    error = "sta_has_ip_invalid"
                elif not flag:
                    error = "sta_has_ip_not_true"
            if error is not None:
                errors.append({"key": key, "ts": sample["ts"], "error": error})
    return errors


def status_errors(status: Any) -> list[str]:
    """BLE network.status must show a restored STA link."""
    if not isinstance(status, dict):
        return ["status_not_object"]
    errors: list[str] = []
    if status.get("net_mode") != RUNTIME_NET_MODE:
        errors.append("status_net_mode_not_sta")
    if status.get("sta_connected") is not True:
        errors.append("status_sta_not_connected")
    if status.get("sta_has_ip") is not True:
        errors.append("status_sta_has_ip_not_true")
    if not _nonempty_str(status.get("sta_ip")):
        errors.append("status_sta_ip_empty")
    if not _nonempty_str(status.get("sta_ssid")):
        errors.append("status_sta_ssid_empty")
    return errors


def attributes_errors(attrs: dict, status: dict) -> list[str]:
    """sta_ssid must exist and match the BLE formal-config SSID.

    sta_ip is recorded but never compared: attributes may hold a stale IP.
    """
    errors: list[str] = []
    item = attrs.get("sta_ssid")
    if item is None:
        errors.append("sta_ssid_missing")
        return errors
    value = item.get("value")
    if not _nonempty_str(value):
        errors.append("sta_ssid_empty")
        return errors
    ble_ssid = status.get("sta_ssid") if isinstance(status, dict) else None
    if not _nonempty_str(ble_ssid):
        errors.append("status_sta_ssid_empty")
    elif value != ble_ssid:
        errors.append("sta_ssid_mismatch")
    return errors


def attributes_staleness(attrs: dict, now: float) -> dict:
    """Record per-attribute age/stale flag; never a verdict by itself."""
    out: dict[str, dict] = {}
    for key, item in attrs.items():
        ts = item.get("last_update_ts")
        if ts is None:
            out[key] = {"age_s": None, "stale": None}
            continue
        age = now - ts
        out[key] = {"age_s": round(age, 3),
                    "stale": age > ATTR_STALE_WARN_S}
    return out


def identity_errors(dev: Any, expected: dict) -> list[str]:
    """Fail-closed identity check for device_id/firmware/build_sha."""
    if not isinstance(dev, dict):
        return ["identity_unreadable"]
    errors: list[str] = []
    for field in IDENTITY_FIELDS:
        value = dev.get(field)
        if not _nonempty_str(value):
            errors.append(field + "_missing")
        elif value != expected.get(field):
            errors.append(field + "_mismatch")
    return errors


def rest_identity_errors(device: Any, expected_uuid: str,
                         expected_name: str) -> list[str]:
    """Verify the REST device UUID/name before any write.

    This binds the REST observation target to the operator-declared TB device;
    the BLE identity gate separately binds the BLE target. The name/device_id
    link is declared by the operator (same name or explicit alias).
    """
    if not isinstance(device, dict):
        return ["rest_device_unreadable"]
    errors: list[str] = []
    dev_id = (device.get("id") or {}).get("id")
    if not _nonempty_str(dev_id) or dev_id != expected_uuid:
        errors.append("rest_device_id_mismatch")
    name = device.get("name")
    if not _nonempty_str(name):
        errors.append("rest_device_name_missing")
    elif name != expected_name:
        errors.append("rest_device_name_mismatch")
    return errors


def binding_kind(tb_name: str, ble_device_id: str,
                 allow_alias: bool) -> str | None:
    """Classify the TB-name <-> BLE-device_id binding.

    Returns "same_name" when identical, "operator_alias" when explicitly
    allowed, or None when an alias was not authorized.
    """
    if tb_name == ble_device_id:
        return "same_name"
    return "operator_alias" if allow_alias else None


def tryconnect_class(ok: bool, op_id: str | None, error: str | None) -> str:
    if ok is True and op_id:
        return "accepted"
    if error == "busy":
        return "busy"
    return "rejected"


def terminal_ok(state: str | None, error: str | None) -> bool:
    return state == "failed" and error in TERMINAL_EXPECTED_ERRORS


def should_retry_read(attempt: int) -> bool:
    return attempt < READ_MAX_ATTEMPTS


def exit_code(verdict: str) -> int:
    return EXIT_CODES.get(verdict, EXIT_CODES["STOP"])


def decide(observer_error: str | None = None,
           device_error: str | None = None,
           write_deadline: bool = False,
           write_unknown: bool = False,
           busy: bool = False,
           tryconnect_ok: bool = True,
           terminal_state: str | None = None,
           terminal_error: str | None = None,
           sta_restored: bool = True,
           identity_after_ok: bool = True,
           missing_keys: list | None = None,
           stale_keys: list | None = None,
           telemetry_errors: list | None = None,
           status_errors_list: list | None = None,
           attr_errors: list | None = None) -> tuple[str, str]:
    """Post-write decision table; observer/device failures never become FAIL."""
    if observer_error:
        return ("STOP", "observer_" + observer_error)
    if device_error:
        return ("STOP", "device_observation_" + device_error)
    if write_deadline:
        return ("STOP", "write_deadline_exhausted_before_send")
    if write_unknown:
        return ("STOP", "write_outcome_unknown")
    if busy:
        return ("NOT_COVERED", "tryconnect_busy")
    if not tryconnect_ok:
        return ("FAIL", "tryconnect_rejected")
    if not terminal_ok(terminal_state, terminal_error):
        return ("FAIL", "terminal_state")
    if not sta_restored:
        return ("FAIL", "sta_not_restored")
    if not identity_after_ok:
        return ("FAIL", "identity_mismatch")
    if missing_keys:
        return ("NOT_COVERED", "telemetry_no_sample_" + missing_keys[0])
    if stale_keys:
        return ("NOT_COVERED", "telemetry_no_fresh_sample_" + stale_keys[0])
    if telemetry_errors:
        return ("FAIL", "telemetry_inconsistent")
    if status_errors_list:
        return ("FAIL", "status_inconsistent")
    if attr_errors:
        return ("FAIL", "attributes_inconsistent")
    return ("PASS", "ok")
