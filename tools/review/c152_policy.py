"""C15-2 MQTT attributes consistency policy (pure, host-testable, no I/O).

Firmware facts this module relies on:
- attributes publishes net_mode/sta_ssid/sta_ip/uart_baudrate/... but NOT
  sta_has_ip (telemetry only; 21883 rejects telemetry subscription).
- wifi.c: AP/APSTA strings exist only to read legacy NVS values; runtime forces
  STA and cfg_validate_net_mode() rejects non-STA writes. So the expected
  runtime value is "sta"; "ap"/"apsta" are legacy/abnormal observations.
"""

from __future__ import annotations

from typing import Any, Iterable

NET_MODES = ("ap", "sta", "apsta")
RUNTIME_NET_MODE = "sta"

READ_MAX_ATTEMPTS = 3
READ_RETRY_DELAY_S = 2.0
READY_STABLE_S = 10.0
READY_MAX_GAP_S = 3.0
TERMINAL_TIMEOUT_S = 40.0
RECOVERY_TIMEOUT_S = 60.0
TERMINAL_STATES = ("failed", "success", "cancelled", "expired")
TERMINAL_EXPECTED_ERRORS = ("wrong_password", "ap_not_found")

IDENTITY_FIELDS = ("device_id", "firmware", "build_sha")
EXIT_CODES = {"PASS": 0, "FAIL": 1, "NOT_COVERED": 2, "STOP": 3}


def _nonempty_str(value: Any) -> bool:
    return isinstance(value, str) and value.strip() != ""


def attr_sample_violations(attrs: Any) -> list[str]:
    """Violations for one attributes sample; only fields actually published."""
    if not isinstance(attrs, dict):
        return ["sample_not_object"]

    errors: list[str] = []
    mode = attrs.get("net_mode")
    if mode not in NET_MODES:
        errors.append("net_mode_invalid")
    elif mode != RUNTIME_NET_MODE:
        errors.append("net_mode_not_sta")

    if mode == RUNTIME_NET_MODE and not _nonempty_str(attrs.get("sta_ssid")):
        errors.append("sta_mode_without_ssid")

    if "uart_baudrate" not in attrs:
        errors.append("uart_baudrate_missing")
    else:
        baud = attrs.get("uart_baudrate")
        if isinstance(baud, bool) or not isinstance(baud, (int, float)) or baud <= 0:
            errors.append("uart_baudrate_invalid")

    return errors


def window_eval(samples: Iterable[dict], w0: float, w1: float) -> dict:
    """Evaluate attributes samples inside [w0, w1] (attributes topic only)."""
    in_window: list[dict] = []
    for sample in samples:
        try:
            ts = float(sample.get("ts"))
        except (TypeError, ValueError):
            continue
        if w0 <= ts <= w1:
            in_window.append({"ts": ts, "obj": sample.get("obj")})

    violations: list[dict] = []
    for sample in in_window:
        for error in attr_sample_violations(sample["obj"]):
            violations.append({"ts": sample["ts"], "error": error})

    last = in_window[-1] if in_window else None
    return {
        "n_attr": len(in_window),
        "violations": violations,
        "last": last["obj"] if last else None,
        "last_ts": last["ts"] if last else None,
    }


def crosscheck(last_attrs: dict | None, last_ts: float | None,
               status: dict, status_ts: float | None) -> list[str]:
    """Cross-check the last attributes sample against a later network.status.

    Both `sta_ip` values must be non-empty; two missing values are not a match.
    """
    if last_attrs is None:
        return ["no_attribute_sample"]
    if status_ts is None:
        return ["status_sample_missing"]

    errors: list[str] = []
    if last_ts is None or float(status_ts) < float(last_ts):
        errors.append("status_not_after_last_attr")
    if status.get("sta_has_ip") is not True:
        errors.append("sta_has_ip_not_true")

    attr_ip = last_attrs.get("sta_ip")
    status_ip = status.get("sta_ip")
    if not _nonempty_str(attr_ip):
        errors.append("attr_sta_ip_empty")
    if not _nonempty_str(status_ip):
        errors.append("status_sta_ip_empty")
    if _nonempty_str(attr_ip) and _nonempty_str(status_ip) and attr_ip != status_ip:
        errors.append("sta_ip_mismatch")
    return errors


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


def suback_ok(codes) -> tuple[bool, str]:
    """SUBACK validation: empty/unparsable/rejected all fail closed."""
    if not codes:
        return (False, "empty")
    for code in codes:
        value = getattr(code, "value", code)
        try:
            numeric = int(value)
        except (TypeError, ValueError):
            return (False, "unparsable")
        if numeric >= 0x80:
            return (False, "rejected")
    return (True, "ok")


def decide(read_timeout: bool = False,
           busy: bool = False,
           n_attr: int = 0,
           violations: list | None = None,
           terminal_state: str | None = None,
           terminal_error: str | None = None,
           sta_restored: bool = False,
           identity_ok: bool = False,
           cross_errors: list | None = None) -> tuple[str, str]:
    """Single decision table; verdict in PASS / FAIL / NOT_COVERED / STOP."""
    if read_timeout:
        return ("STOP", "read_timeout")
    if busy:
        return ("NOT_COVERED", "tryconnect_busy")
    if n_attr == 0:
        return ("NOT_COVERED", "no_attributes_sample")
    if violations:
        return ("FAIL", "attribute_inconsistent")
    if not terminal_ok(terminal_state, terminal_error):
        return ("FAIL", "terminal_state")
    if not sta_restored:
        return ("FAIL", "sta_not_restored")
    if not identity_ok:
        return ("FAIL", "identity_mismatch")
    if cross_errors:
        return ("FAIL", "crosscheck_failed")
    return ("PASS", "ok")
