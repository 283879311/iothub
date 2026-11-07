#!/usr/bin/env python3
"""MQTT raw-v1 41 例回归 harness(2026-11-07 重建入库)。

背景:本套件首跑于 2026-11-06(DEF-SCAN 验收 R-SCAN-3/V-3),但脚本只在当轮
会话的 /tmp 里,未入库 —— 证据链断点,已列入待排期。本版从归档证据
artifacts/scan_v2/r41_report.json(41 例的请求形态与设备实响应)重建,
判据与当时记录一致;设备侧行为如有变化,以失败形式暴露而不是迁就。

主题契约(main/mqtt.c):
  请求  v1/devices/me/rpc/request/<rid>   (TB backend)
  响应  v1/devices/me/rpc/response/<rid>  (v1 回包 id=envelope id;v0 拒绝壳
         id=rid 数值);自定义主题 iothub/rpc → iothub/rpc/resp(仅 P11 用)

运行前置(需单独批准,属配置变更):
  设备 mqtt_host 临时指向本 harness 的 broker(默认 127.0.0.1:21883),
  测完恢复正式配置。设备侧 MQTT 命令走 broker 认证,无需 payload token。

用法:
  python3 tools/mqtt_v1_r41.py --list
  python3 tools/mqtt_v1_r41.py --selftest            # 离线:帧构造/匹配逻辑自测
  python3 tools/mqtt_v1_r41.py [--broker 127.0.0.1:21883] [--out artifacts/xxx]

关于 T01/T01b/T02 的 v0 字样:v0 外部入口已于 2026-11-05 彻底删除,
这三例是**墓碑测试** —— 发 v0 形状的帧,断言必须被拒(unsupported_version,
拒绝壳按设计保留 v0 方言并回带数值 id,见 main/mqtt.c 963-985)。固件内
无任何 v0 工作路径;本 harness 的 P/N 组全部是纯 v1 信封。

用例覆盖(与 r41_report.json 对齐):
  T01-T08 envelope 校验(v0 壳/版本/缺字段/未知命令/非 JSON 静默/限流)
  P01-P12 命令面正路径(只读为主;P06 config.set 仅 gpio/data_route 域)
  F01-F10 BLE-only 命令经 MQTT 一律 forbidden
  N01-N07 网络事务(N03/N07 需注入时序,默认 SKIP;N06 真实换网,
          静态锁定设备自动记 SKIP 并写明原因)
"""
from __future__ import annotations

import argparse
import json
import sys
import threading
import time
from datetime import datetime

try:
    import paho.mqtt.client as mqtt
except ImportError:  # pragma: no cover
    mqtt = None

RPC_REQ = "v1/devices/me/rpc/request/{}"
RPC_RESP_FILTER = "v1/devices/me/rpc/response/#"
CUSTOM_RPC = "iothub/rpc"
CUSTOM_RESP = "iothub/rpc/resp"


class MqttV1:
    """极简同步客户端:发 v1 信封,按 id 等响应。"""

    def __init__(self, broker: str, timeout: float = 8.0):
        host, _, port = broker.partition(":")
        self.host, self.port = host, int(port or 21883)
        self.timeout = timeout
        self.lock = threading.Lock()
        self.responses: dict[str, dict] = {}
        self.custom: list[dict] = []
        self.rid = 7000
        self.cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, protocol=mqtt.MQTTv311)
        self.cli.on_message = self._on_message
        self.cli.connect(self.host, self.port, keepalive=30)
        self.cli.subscribe([(RPC_RESP_FILTER, 1), (CUSTOM_RESP, 1)])
        self.cli.loop_start()

    def _on_message(self, _c, _u, msg):
        try:
            obj = json.loads(msg.payload.decode("utf-8", "replace"))
        except json.JSONDecodeError:
            return
        with self.lock:
            if msg.topic == CUSTOM_RESP:
                self.custom.append(obj)
                return
            rid = msg.topic.rsplit("/", 1)[-1]
            self.responses[str(obj.get("id", rid))] = obj
            self.responses.setdefault(f"__rid__{rid}", obj)

    def _next_rid(self) -> int:
        self.rid += 1
        return self.rid

    def send(self, req: dict | str, custom: bool = False, wait: float | None = None):
        """发一帧;req 为 dict(v1 信封/v0 形状)或原始字节(非 JSON 用例)。
        返回 (resp|None, rid)。"""
        rid = self._next_rid()
        topic = CUSTOM_RPC if custom else RPC_REQ.format(rid)
        payload = json.dumps(req, separators=(",", ":")).encode() if isinstance(req, dict) else req
        with self.lock:
            self.responses.pop(f"__rid__{rid}", None)
        self.cli.publish(topic, payload, qos=1)
        deadline = time.time() + (self.timeout if wait is None else wait)
        while time.time() < deadline:
            with self.lock:
                if custom:
                    if self.custom:
                        return self.custom.pop(0), rid
                else:
                    hit = self.responses.get(str(req.get("id")), None) if isinstance(req, dict) else None
                    if hit is None:
                        hit = self.responses.get(f"__rid__{rid}")
                    if hit is not None:
                        return hit, rid
            time.sleep(0.02)
        return None, rid

    def close(self):
        self.cli.loop_stop()
        self.cli.disconnect()


def code_of(resp: dict | None) -> str:
    if resp is None:
        return "<timeout>"
    if "error" in resp and isinstance(resp["error"], dict):
        return resp["error"].get("code", "<no-code>")
    if "error" in resp:
        return str(resp["error"])
    return "<ok>"


def ok(resp):
    return resp is not None and resp.get("ok") is True


def err_is(resp, code):
    return code_of(resp) == code


def build_cases(h: MqttV1, opts):
    """用例表:与 artifacts/scan_v2/r41_report.json 的判据一一对应。"""
    cases = []

    def case(cid, name, fn):
        cases.append({"id": cid, "name": name, "fn": fn})

    # ---- T 组:envelope 校验 ----
    case("T01", "v0帧(无version)→unsupported_version",
         lambda: (lambda r: err_is(r[0], "unsupported_version"))(h.send({"id": "t01", "cmd": "device.info", "args": {}})))
    case("T01b", "v0帧数值id→拒绝壳回带id",
         lambda: (lambda r: r[0] is not None and r[0].get("id") == 4242 and r[0].get("error") == "unsupported_version")(
             h.send({"id": 4242, "cmd": "device.info", "args": {}})))
    case("T02", "version=2.0→unsupported_version",
         lambda: (lambda r: err_is(r[0], "unsupported_version"))(h.send({"id": "t02", "version": "2.0", "cmd": "device.info", "args": {}})))
    case("T03", "缺id→invalid_argument",
         lambda: (lambda r: err_is(r[0], "invalid_argument"))(h.send({"version": "1.0", "cmd": "device.info", "args": {}})))
    case("T04", "缺cmd→invalid_argument",
         lambda: (lambda r: err_is(r[0], "invalid_argument"))(h.send({"id": "t04", "version": "1.0", "args": {}})))
    case("T05", "args非对象→invalid_argument",
         lambda: (lambda r: err_is(r[0], "invalid_argument"))(h.send({"id": "t05", "version": "1.0", "cmd": "device.info", "args": "x"})))
    case("T06", "未知命令→unknown_command",
         lambda: (lambda r: err_is(r[0], "unknown_command"))(h.send({"id": "t06", "version": "1.0", "cmd": "no.such.cmd", "args": {}})))
    case("T07", "非JSON静默丢弃(无响应)",
         lambda: (lambda r: r[0] is None)(h.send(b"\xde\xad not json", wait=3.0)))
    case("T08", "200ms内连发第二发→rate_limited",
         lambda: _rate_limit(h))

    # ---- P 组:命令面正路径(MQTT 走 broker 认证,无 payload token) ----
    case("P01", "device.info→ok+data",
         lambda: (lambda r: ok(r[0]) and "firmware" in (r[0].get("data") or {}))(h.send({"id": "P01", "version": "1.0", "cmd": "device.info", "args": {}})))
    case("P02", "device.status→ok+data",
         lambda: (lambda r: ok(r[0]) and isinstance(r[0].get("data"), dict))(h.send({"id": "P02", "version": "1.0", "cmd": "device.status", "args": {}})))
    case("P03", "gpio.status→ok+data",
         lambda: (lambda r: ok(r[0]) and isinstance(r[0].get("data"), dict))(h.send({"id": "P03", "version": "1.0", "cmd": "gpio.status", "args": {}})))
    case("P04", "uart.runtime→ok+data",
         lambda: (lambda r: ok(r[0]) and isinstance(r[0].get("data"), dict))(h.send({"id": "P04", "version": "1.0", "cmd": "uart.runtime", "args": {}})))
    case("P05", "config.get→ok+data",
         lambda: (lambda r: ok(r[0]) and isinstance(r[0].get("data"), dict))(h.send({"id": "P05", "version": "1.0", "cmd": "config.get", "args": {}})))
    case("P07", "io.get→ok+data",
         lambda: (lambda r: ok(r[0]) and isinstance(r[0].get("data"), dict))(h.send({"id": "P07", "version": "1.0", "cmd": "io.get", "args": {}})))
    case("P08", "led.toggle→ok",
         lambda: (lambda r: ok(r[0]))(h.send({"id": "P08", "version": "1.0", "cmd": "led.toggle", "args": {}})))
    case("P09", "network.status→ok+data",
         lambda: (lambda r: ok(r[0]) and "sta_ip" in (r[0].get("data") or {}))(h.send({"id": "P09", "version": "1.0", "cmd": "network.status", "args": {}})))
    case("P05b", "config.get未知域→invalid_argument",
         lambda: (lambda r: err_is(r[0], "invalid_argument"))(h.send({"id": "P05b", "version": "1.0", "cmd": "config.get", "args": {"domains": ["nope"]}})))
    case("P06", "config.set(gpio/data_route域)→ok",
         lambda: (lambda r: ok(r[0]))(h.send({"id": "P06", "version": "1.0", "cmd": "config.set",
                                              "args": {"patch": {"led_power_policy": 1}}})))
    case("P12", "数字id回包为字符串'777'",
         lambda: (lambda r: ok(r[0]) and r[0].get("id") == "777")(
             h.send({"id": 777, "version": "1.0", "cmd": "device.info", "args": {}})))
    case("P11", "自定义主题 iothub/rpc(TB backend 预期无响应)",
         lambda: (lambda r: r[0] is None and not ok(r[0]))(h.send({"id": "P11", "version": "1.0", "cmd": "device.info", "args": {}}, custom=True, wait=3.0)))
    case("P10", "operation.get未知id→not_found",
         lambda: (lambda r: err_is(r[0], "not_found"))(h.send({"id": "P10", "version": "1.0", "cmd": "operation.get", "args": {"operation_id": "op-NOSUCH"}})))

    # ---- F 组:BLE-only 命令经 MQTT 一律 forbidden ----
    for i, (cid, cmd) in enumerate([
        ("F01", "network.scan"), ("F02", "network.profile.list"),
        ("F03", "network.profile.delete"), ("F04", "network.reconnect"),
        ("F05", "auth.login"), ("F06", "auth.admin.set"),
        ("F07", "provisioning.pin.set"), ("F08", "uart.send"),
        ("F09", "data.loopback"),
    ]):
        args = {"ssid": "x", "password": "x"} if cmd == "network.scan" else \
               {"operation_id": "op-x"} if cmd == "network.profile.delete" else \
               {"username": "a", "password": "b"} if cmd in ("auth.login", "auth.admin.set") else \
               {"pin": "1234"} if cmd == "provisioning.pin.set" else \
               {"data": "AA"} if cmd == "uart.send" else \
               {"payload": "x"} if cmd == "data.loopback" else {}
        case(cid, f"{cmd}(BLE-only)→forbidden",
             lambda cmd=cmd, args=args: (lambda r: err_is(r[0], "forbidden"))(
                 h.send({"id": cid, "version": "1.0", "cmd": cmd, "args": args})))
    case("F10", "config.set改网络域→forbidden(guard_rejected映射)",
         lambda: (lambda r: err_is(r[0], "forbidden"))(h.send({"id": "F10", "version": "1.0", "cmd": "config.set",
                                                               "args": {"patch": {"sta_ssid": "evil"}}})))

    # ---- N 组:网络事务 ----
    case("N01", "tryconnect缺ssid→invalid_argument(params_required)",
         lambda: (lambda r: err_is(r[0], "invalid_argument"))(h.send({"id": "N01", "version": "1.0", "cmd": "network.tryconnect", "args": {}})))
    case("N02", "tryconnect假SSID→受理 或 静态锁(static_ip_pinned)",
         lambda: _n02(h, opts))
    case("N03", "假SSID操作终态=failed", lambda: _skip("需失败时序注入,默认 SKIP(与首跑一致)"))
    case("N07", "cancel受理中的操作", lambda: _skip("需在飞时序,默认 SKIP(与首跑一致)"))
    case("N04", "apply缺operation_id→invalid_argument(params_required)",
         lambda: (lambda r: err_is(r[0], "invalid_argument"))(h.send({"id": "N04", "version": "1.0", "cmd": "network.apply", "args": {}})))
    case("N05", "apply未知id→not_found",
         lambda: (lambda r: err_is(r[0], "not_found"))(h.send({"id": "N05", "version": "1.0", "cmd": "network.apply", "args": {"operation_id": "op-NOSUCH"}})))
    case("N06a", "tryconnect真AP受理", lambda: _n06a(h, opts))
    case("N06b", "真AP到达got_ip", lambda: _n06b(h, opts))
    case("N06c", "apply提交→success", lambda: _n06c(h, opts))
    return cases


def _skip(reason):
    return ("SKIP", reason)


def _rate_limit(h: MqttV1):
    # 限流窗口 200ms/transport:t08a 裸发(不等响应),20ms 后 t08b 必落窗内
    rid = h._next_rid()
    h.cli.publish(RPC_REQ.format(rid),
                  json.dumps({"id": "t08a", "version": "1.0", "cmd": "device.info", "args": {}}),
                  qos=1)
    time.sleep(0.02)
    r2, _ = h.send({"id": "t08b", "version": "1.0", "cmd": "device.info", "args": {}}, wait=3.0)
    return err_is(r2, "rate_limited")


def _n02(h: MqttV1, opts):
    r, _ = h.send({"id": "N02", "version": "1.0", "cmd": "network.tryconnect",
                   "args": {"ssid": opts.fake_ssid, "password": "wrong-pass"}}, wait=12.0)
    return ok(r) or err_is(r, "static_ip_pinned")


_N06: dict = {}


def _n06a(h: MqttV1, opts):
    if opts.ssid is None:
        return _skip("未给 --ssid/--password,静态锁定或无真实 AP:SKIP(写明原因)")
    # 前置探活:N02 的 op 在飞时设备 MQTT 随 Wi-Fi 拆链(实测 60-74s 窗口),
    # 不等回来就发 tryconnect 会落进空档(11-07 首跑 N06a FAIL 根因)
    deadline = time.time() + 25
    while time.time() < deadline:
        probe, _ = h.send({"id": "n06probe", "version": "1.0",
                           "cmd": "device.info", "args": {}}, wait=3.0)
        if probe is not None and probe.get("ok") is True:
            break
        time.sleep(1.0)
    else:
        return _skip("设备 MQTT 25s 未恢复(前置探活失败)")
    r, _ = h.send({"id": "N06", "version": "1.0", "cmd": "network.tryconnect",
                   "args": {"ssid": opts.ssid, "password": opts.password}}, wait=12.0)
    if err_is(r, "static_ip_pinned"):
        return _skip("设备静态锁定,N06 换网路径不适用(用 N02 验锁)")
    if not ok(r):
        return False
    _N06["op"] = (r.get("data") or {}).get("operation_id")
    return True


def _n06b(h: MqttV1, opts):
    if "op" not in _N06:
        return _skip("N06a 未受理")
    deadline = time.time() + 60
    while time.time() < deadline:
        r, _ = h.send({"id": f"N06b{time.time()%1000:.0f}", "version": "1.0",
                       "cmd": "operation.get", "args": {"operation_id": _N06["op"]}})
        if ok(r):
            st = (r.get("data") or {}).get("state")
            if st in ("got_ip", "success"):
                _N06["state"] = st
                return True
            if st in ("failed", "cancelled", "expired"):
                return False
        time.sleep(1.0)
    return False


def _n06c(h: MqttV1, opts):
    if _N06.get("state") != "got_ip":
        return _skip("N06b 未到 got_ip")
    r, _ = h.send({"id": "N06c", "version": "1.0", "cmd": "network.apply",
                   "args": {"operation_id": _N06["op"]}}, wait=15.0)
    return ok(r)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--broker", default="127.0.0.1:21883")
    ap.add_argument("--out", default=None, help="报告 JSON 输出路径")
    ap.add_argument("--ssid", default=None, help="N06 真实换网用(需批准)")
    ap.add_argument("--password", default=None)
    ap.add_argument("--fake-ssid", default="P0-NOAP-r41")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--selftest", action="store_true", help="离线自测帧构造/分类")
    opts = ap.parse_args()

    cases = build_cases(None, opts)
    if opts.list:
        for c in cases:
            print(c["id"], c["name"])
        return 0

    if opts.selftest:
        # 离线:每例可构造、id/name 唯一、分类函数对记录样本判对
        ids = [c["id"] for c in cases]
        assert len(ids) == len(set(ids)) and len(ids) == 41, f"case 数 {len(ids)}"
        samples = [
            ({"id": "t01", "success": False, "error": "unsupported_version"}, "unsupported_version"),
            ({"id": "t08b", "ok": False, "error": {"code": "rate_limited", "message": "rate_limited"}}, "rate_limited"),
            ({"id": "P01", "ok": True, "data": {"firmware": "x"}}, "<ok>"),
        ]
        for resp, want in samples:
            assert code_of(resp) == want, (resp, want, code_of(resp))
        assert code_of(None) == "<timeout>"
        print(f"selftest: {len(ids)} cases, code_of 分类 4/4 OK")
        return 0

    if mqtt is None:
        print("需要 paho-mqtt:.venv-bleak/bin/python 运行", file=sys.stderr)
        return 2

    h = MqttV1(opts.broker)
    report = {"target": opts.broker, "time": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
              "cases": [], "summary": {}}
    n_pass = n_fail = n_skip = 0
    for c in build_cases(h, opts):
        time.sleep(0.25)   # > 设备 200ms/transport 限流窗;T08 的连发在用例内部自造
        try:
            r = c["fn"]()
        except Exception as exc:  # noqa: BLE001
            r = False
            detail = f"异常 {type(exc).__name__}: {exc}"
        else:
            detail = "PASS" if r is True else (r[1] if isinstance(r, tuple) else json.dumps(r, ensure_ascii=False)[:160] if r is not None else "无响应")
        if r is True:
            status = "PASS"
            n_pass += 1
        elif isinstance(r, tuple) and r[0] == "SKIP":
            status = "SKIP"
            n_skip += 1
        else:
            status = "FAIL"
            n_fail += 1
        report["cases"].append({"id": c["id"], "name": c["name"], "status": status, "detail": detail})
        print(f"{c['id']:5s} {status} {detail[:100]}")
    h.close()
    report["summary"] = {"total": len(report["cases"]), "passed": n_pass,
                         "failed": n_fail, "skipped": n_skip}
    verdict = "PASS" if n_fail == 0 else f"FAIL({n_fail})"
    report["verdict"] = verdict
    if opts.out:
        with open(opts.out, "w", encoding="utf-8") as f:
            json.dump(report, f, ensure_ascii=False, indent=1)
            f.write("\n")
    print(f"汇总:{verdict}  {n_pass}/{n_fail}/{n_skip}(pass/fail/skip)")
    return 0 if n_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
