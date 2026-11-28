"""C15-2 MQTT attributes consistency policy (pure, host-testable, no I/O).

Background: the retired v9 harness read `sta_has_ip` from the attributes topic,
but the firmware never publishes it there (attributes: net_mode/sta_ssid/
sta_ip/uart_baudrate/...; telemetry: sta_has_ip/...). On the production broker
(21883) telemetry subscription is rejected (SUBACK 0x80), so `sta_has_ip` must
be cross-checked with the BLE `network.status` command instead.

This module only decides; it performs no network or device I/O.
"""

from __future__ import annotations

from typing import Any, Iterable

NET_MODES = ("ap", "sta", "apsta")

READ_MAX_ATTEMPTS = 3
READ_RETRY_DELAY_S = 2.0
READY_STABLE_S = 10.0
TERMINAL_TIMEOUT_S = 40.0
RECOVERY_TIMEOUT_S = 60.0
TERMINAL_STATES = ("failed", "success", "cancelled", "expired")
TERMINAL_EXPECTED_ERRORS = ("wrong_password", "ap_not_found")


def attr_sample_violations(attrs: Any) -> list[str]:
    """Return violations for one attributes sample.

    Rules only assert what the firmware actually publishes; `sta_has_ip` is
    intentionally not looked up here.
    """
    if not isinstance(attrs, dict):
        return ["sample_not_object"]

    errors: list[str] = []
    mode = attrs.get("net_mode")
    if mode not in NET_MODES:
        errors.append("net_mode_invalid")
    elif mode == "sta" and not (attrs.get("sta_ssid") or ""):
        # STA mode requires an SSID; empty SSID there is a config/runtime tear.
        errors.append("sta_mode_without_ssid")

    # Only AP (no STA) is asserted to have no STA IP. APSTA may legitimately
    # carry a STA IP, so it is not asserted either way.
    if mode == "ap" and attrs.get("sta_ip"):
        errors.append("ap_mode_with_ip")

    baud = attrs.get("uart_baudrate")
    if baud is not None:
        if isinstance(baud, bool) or not isinstance(baud, (int, float)) or baud <= 0:
            errors.append("uart_baudrate_invalid")

    return errors


def window_eval(samples: Iterable[dict], w0: float, w1: float) -> dict:
    """Evaluate attributes samples inside [w0, w1].

    samples: iterable of {"ts": float, "obj": dict}; callers must only pass
    attributes-topic samples.
    """
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

    The status sample must be taken after the last attributes sample; otherwise
    the two values may describe different points in time.
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
    if last_attrs.get("sta_ip") != status.get("sta_ip"):
        errors.append("sta_ip_mismatch")
    return errors


def tryconnect_class(ok: bool, op_id: str | None, error: str | None) -> str:
    """Classify a tryconnect response: accepted / busy / rejected."""
    if ok is True and op_id:
        return "accepted"
    if error == "busy":
        return "busy"
    return "rejected"


def terminal_ok(state: str | None, error: str | None) -> bool:
    return state == "failed" and error in TERMINAL_EXPECTED_ERRORS


def should_retry_read(attempt: int) -> bool:
    """attempt is 1-based; retry only while under READ_MAX_ATTEMPTS."""
    return attempt < READ_MAX_ATTEMPTS


def decide(read_timeout: bool = False,
           busy: bool = False,
           n_attr: int = 0,
           violations: list | None = None,
           terminal_state: str | None = None,
           terminal_error: str | None = None,
           sta_restored: bool = False,
           identity_ok: bool = False,
           cross_errors: list | None = None) -> tuple[str, str]:
    """Single decision table for C15-2.

    Returns (verdict, reason) with verdict in PASS / FAIL / NOT_COVERED / STOP.
    """
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
