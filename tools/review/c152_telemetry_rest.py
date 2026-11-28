#!/usr/bin/env python3
"""C15-2 Telemetry REST + Attributes + BLE mixed-observation harness.

Observation channel: ThingsBoard REST GETs (telemetry timeseries + attributes).
Command/state channel: BLE device.status / network.status / operation.get.
No MQTT, no device Access Token, no second MQTT session.

Decision logic lives in `c152_policy.py`; REST mapping lives in
`c152_rest_observer.py`. This file wires I/O and exposes `run_case()` with
injectable dependencies (fake BLE session / fake observer / fake clocks).

Usage (device run requires separate authorization):
  IOTHUB_BLE_USER=... IOTHUB_BLE_PASSWORD=... \
  IOTHUB_TB_USER=... IOTHUB_TB_PASSWORD=... \
  IOTHUB_TB_REST_BASE=http://<host>:<port> IOTHUB_TB_DEVICE_ID=<uuid> \
  IOTHUB_TEST_SSID=... python3 tools/review/c152_telemetry_rest.py --device IotHub-65D8

Exit codes: 0 PASS, 1 FAIL, 2 NOT_COVERED, 3 STOP.
"""

from __future__ import annotations

import argparse
import asyncio
import importlib.util
import json
import os
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
P0 = ROOT / "tools" / "ble_p0" / "p0_acceptance.py"
sys.path.insert(0, str(Path(__file__).resolve().parent))
import c152_policy as policy  # noqa: E402
import c152_rest_observer as rest  # noqa: E402
from c152_ble import (Deps, DeviceObservationError, ReadTimeout,  # noqa: E402
                      StopRun, identity_gate, read_data, req, select_target,
                      wait_ready, wait_sta, wait_terminal)

WRONG_PASSWORD = "race-wrong-pass-0"
EXPECTED = {"device_id": "IotHub-65D8", "firmware": "1.3-956afcd",
            "build_sha": "c231c9501"}
DEFAULT_ADDR = "10:97:BD:D3:65:DA"
OBSERVER_PREFLIGHT_WINDOW_S = 120.0


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


def _samples_view(samples: dict) -> dict:
    return {key: [{"ts": s["ts"], "value": s["value"]}
                  for s in samples.get(key, [])]
            for key in policy.TELEMETRY_KEYS}


async def _observer_preflight(deps: Deps, result: dict) -> tuple[str | None, dict]:
    """Login + pre-operation series fetch; returns (error_code, series)."""
    now = deps.wall()
    try:
        await deps.observer.start()
        raw = await deps.observer.fetch_series(
            policy.TELEMETRY_KEYS,
            int((now - OBSERVER_PREFLIGHT_WINDOW_S) * 1000),
            int((now + policy.CLOCK_SKEW_S) * 1000))
        series, errors = policy.parse_series(raw)
        if errors:
            raise rest.ObserverError("protocol", errors[0])
        series, _dropped = policy.drop_future(series, now)
    except rest.ObserverError as exc:
        result["observer"] = {"state": "error",
                              "error": {"code": exc.code,
                                        "detail": exc.detail}}
        return exc.code, {}
    result["observer"] = {"state": "ok", "error": None}
    return None, series


async def run_case(deps: Deps, ssid: str,
                   expected: dict | None = None) -> dict:
    """Run C15-2; returns the full result dict with verdict/reason."""
    expected = expected or EXPECTED
    result: dict = {"verdict": "STOP", "reason": "unfinished",
                    "observer": {"state": "unknown", "error": None},
                    "device": {"errors": []},
                    "operation": {}}
    try:
        dev, errs = await identity_gate(deps, expected, deps.mono() + 45.0)
        result["device"]["identity"] = dev
        if errs:
            result.update(verdict="STOP", reason="identity", identity_errors=errs)
            return result

        if not await wait_ready(deps, policy.READY_STABLE_S):
            result.update(verdict="STOP", reason="readiness")
            return result

        obs_error, pre_series = await _observer_preflight(deps, result)
        if obs_error is not None:
            result.update(verdict="STOP", reason="observer_" + obs_error)
            return result

        op_start = deps.wall()
        baseline, baseline_reason = policy.baseline_check(pre_series, op_start)
        result["observer"]["baseline"] = baseline
        result["observer"]["baseline_reason"] = baseline_reason
        if baseline_reason is not None:
            result.update(verdict="NOT_COVERED", reason=baseline_reason)
            return result

        write_deadline = deps.mono() + deps.write_timeout
        remaining = write_deadline - deps.mono()
        if remaining <= 0:
            result.update(verdict="STOP",
                          reason="write_deadline_exhausted_before_send")
            return result
        try:
            effective = min(deps.write_timeout, remaining)
            tc = await asyncio.wait_for(
                deps.sess.send_request(
                    req(deps.rids.next("tc1"), "network.tryconnect",
                        {"ssid": ssid, "password": WRONG_PASSWORD},
                        deps.sess.token),
                    timeout=effective, retries=0),
                timeout=effective)
            if deps.mono() > write_deadline:
                raise asyncio.TimeoutError("tryconnect returned after deadline")
        except Exception as exc:  # noqa: BLE001
            result.update(verdict="STOP", reason="write_outcome_unknown",
                          detail=type(exc).__name__)
            return result

        obj = tc.obj or {}
        op_id = (obj.get("data") or {}).get("operation_id")
        cls = policy.tryconnect_class(obj.get("ok"), op_id,
                                      (obj.get("error") or {}).get("code"))
        result["operation"] = {"class": cls, "operation_id": op_id}
        if cls == "busy":
            result.update(verdict="NOT_COVERED", reason="tryconnect_busy",
                          w0=op_start)
            return result
        if cls != "accepted":
            result.update(verdict="FAIL", reason="tryconnect_rejected",
                          w0=op_start)
            return result

        term = await wait_terminal(deps, op_id, policy.TERMINAL_TIMEOUT_S)
        op_end = deps.wall()
        result["operation"]["terminal"] = {"state": term.get("state"),
                                           "error": term.get("error")}
        restored = await wait_sta(deps, policy.RECOVERY_TIMEOUT_S)
        result["sta_restored"] = restored

        dev2, errs2 = await identity_gate(deps, expected, deps.mono() + 45.0)
        result["device"]["identity_after"] = dev2
        identity_ok = not errs2

        observer_error = None
        device_error = None
        status: dict = {}
        attrs: dict = {}
        window: dict = {}
        missing: list = []
        dropped = 0
        try:
            now = deps.wall()
            raw = await deps.observer.fetch_series(
                policy.TELEMETRY_KEYS,
                int((op_start - policy.CLOCK_SKEW_S - 1.0) * 1000),
                int((now + policy.CLOCK_SKEW_S) * 1000))
            series, errors = policy.parse_series(raw)
            if errors:
                raise rest.ObserverError("protocol", errors[0])
            series, dropped = policy.drop_future(series, now)
            window = policy.window_samples(
                series, op_start - policy.CLOCK_SKEW_S,
                op_end + policy.MAX_DATA_DELAY_S)
            missing = policy.missing_window_keys(window)
        except rest.ObserverError as exc:
            observer_error = exc.code
            result["observer"] = {"state": "error",
                                  "error": {"code": exc.code,
                                            "detail": exc.detail}}
        else:
            result["observer"]["state"] = "ok"

        try:
            status = await read_data(deps, "network.status", {}, "post", 30,
                                     deps.mono() + 45.0)
        except ReadTimeout as exc:
            device_error = "read_timeout"
            result["device"]["errors"].append(str(exc))
        except DeviceObservationError as exc:
            device_error = exc.code
            result["device"]["errors"].append(str(exc))

        try:
            raw_attrs = await deps.observer.fetch_attributes(policy.ATTRIBUTE_KEYS)
            attrs, attr_protocol = policy.parse_attributes(raw_attrs)
            if attr_protocol:
                raise rest.ObserverError("protocol", attr_protocol[0])
        except rest.ObserverError as exc:
            if observer_error is None:
                observer_error = exc.code
                result["observer"] = {"state": "error",
                                      "error": {"code": exc.code,
                                                "detail": exc.detail}}

        if status:
            status_view = {key: status.get(key) for key in
                           ("net_mode", "sta_connected", "sta_has_ip",
                            "sta_ip", "sta_ssid", "connected_ssid")}
        else:
            status_view = {}
        status_errs = policy.status_errors(status) if status else []
        attr_errs = (policy.attributes_errors(attrs, status)
                     if status else [])
        telemetry_errs = ([] if missing
                          else policy.telemetry_violations(window))
        staleness = policy.attributes_staleness(attrs, deps.wall())

        verdict, reason = policy.decide(
            observer_error=observer_error, device_error=device_error,
            terminal_state=term.get("state"), terminal_error=term.get("error"),
            sta_restored=restored, identity_after_ok=identity_ok,
            missing_keys=missing, telemetry_errors=telemetry_errs,
            status_errors_list=status_errs, attr_errors=attr_errs)
        result.update(
            verdict=verdict, reason=reason, w0=op_start, op_end=op_end,
            w1=deps.wall(),
            window={"samples": _samples_view(window),
                    "n": {k: len(v) for k, v in window.items()},
                    "missing": missing, "future_dropped": dropped},
            telemetry_errors=telemetry_errs, status=status_view,
            status_errors=status_errs, attrs=attrs,
            attrs_staleness=staleness, attr_errors=attr_errs)
        return result
    except ReadTimeout as exc:
        result.update(verdict="STOP", reason="device_observation_read_timeout",
                      detail=str(exc))
        result["device"]["errors"].append(str(exc))
        return result
    except DeviceObservationError as exc:
        result.update(verdict="STOP", reason="device_observation_" + exc.code,
                      detail=exc.detail)
        result["device"]["errors"].append(str(exc))
        return result
    except StopRun as exc:
        result.update(verdict="STOP", reason="stop", detail=str(exc))
        return result
    except rest.ObserverError as exc:
        result["observer"] = {"state": "error",
                              "error": {"code": exc.code,
                                        "detail": exc.detail}}
        result.update(verdict="STOP", reason="observer_" + exc.code)
        return result
    except Exception as exc:  # noqa: BLE001
        result.update(verdict="STOP", reason="exception",
                      detail="%s: %s" % (type(exc).__name__, exc))
        return result


async def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ssid", default=os.environ.get("IOTHUB_TEST_SSID"))
    parser.add_argument("--addr", default=DEFAULT_ADDR)
    parser.add_argument("--device", default="IotHub-65D8")
    parser.add_argument("--expect-fw", default="1.3-956afcd")
    parser.add_argument("--expect-sha", default="c231c9501")
    parser.add_argument("--rest-base",
                        default=os.environ.get("IOTHUB_TB_REST_BASE"))
    parser.add_argument("--device-id",
                        default=os.environ.get("IOTHUB_TB_DEVICE_ID"))
    parser.add_argument("--out", default="/tmp/c152_telemetry_rest.json")
    args = parser.parse_args()

    ble_user = os.environ.get("IOTHUB_BLE_USER")
    ble_password = os.environ.get("IOTHUB_BLE_PASSWORD")
    tb_user = os.environ.get("IOTHUB_TB_USER")
    tb_password = os.environ.get("IOTHUB_TB_PASSWORD")
    if not ble_user or not ble_password:
        print("REFUSE: IOTHUB_BLE_USER/IOTHUB_BLE_PASSWORD missing")
        return 3
    if not tb_user or not tb_password or not args.rest_base or not args.device_id:
        print("REFUSE: TB REST env/args missing "
              "(IOTHUB_TB_USER/IOTHUB_TB_PASSWORD/REST_BASE/DEVICE_ID)")
        return 3
    if not args.ssid:
        print("REFUSE: --ssid or IOTHUB_TEST_SSID missing")
        return 3

    p0 = load_module("p0_acceptance", P0)
    result: dict = {"verdict": "STOP", "reason": "unfinished"}
    cli = None
    try:
        recs = await p0.scan_filtered(10)
        target = select_target(recs, args.addr)
        if not target:
            raise StopRun("target %s not found in scan" % args.addr)
        cli = p0.BleakClient(args.addr, timeout=25)
        await cli.connect()
        sess = p0.Session(cli, address=args.addr,
                          username=ble_user, password=ble_password)
        await cli.start_notify(p0.CMD_TX, sess.on_notify)
        await asyncio.sleep(0.3)
        await sess.login(ble_user, ble_password, retries=0)

        transport = rest.UrllibTransport(args.rest_base, timeout=10.0)
        observer = rest.TbRestObserver(transport, args.device_id,
                                       tb_user, tb_password)
        deps = Deps(sess, observer)
        expected = {"device_id": args.device, "firmware": args.expect_fw,
                    "build_sha": args.expect_sha}
        result = await run_case(deps, args.ssid, expected)
        return policy.exit_code(result.get("verdict", "STOP"))
    except StopRun as exc:
        result.update(verdict="STOP", reason="stop", detail=str(exc))
        return 3
    except Exception as exc:  # noqa: BLE001
        result.update(verdict="STOP", reason="exception",
                      detail="%s: %s" % (type(exc).__name__, exc))
        return 3
    finally:
        if cli is not None:
            try:
                await cli.disconnect()
            except Exception:  # noqa: BLE001
                pass
        Path(args.out).write_text(
            json.dumps(result, ensure_ascii=False, indent=1) + "\n",
            encoding="utf-8")
        print(json.dumps({"verdict": result.get("verdict"),
                          "reason": result.get("reason")}, ensure_ascii=False))


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
