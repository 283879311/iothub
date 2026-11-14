#!/usr/bin/env python3
"""S6 v11.1 离线自检（不连设备/不连 broker，全部替身对象）。

覆盖评审要求的 7 类用例：
  1 连接失败（connect 抛异常）
  2 订阅失败（subscribe 返回非 0）
  3 SUBACK 拒绝（授权码 128）
  4 R4 监听未就绪（无 CONNACK）
  5 R4 观察期间收到设备消息
  6 R2 缺少 changed 目标键
  7 TCP 不可达
另含：严格 ID 匹配（正常/串台）、非本轮 op-ID 拒发。

用法：.venv-bleak/bin/python tools/s6_selftest.py
"""

import asyncio
import json
import sys
import types
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import s6_execute as S  # noqa: E402

# 自检写入临时目录（不污染正式证据目录）
import tempfile  # noqa: E402
S.OUT = Path(tempfile.mkdtemp(prefix="s6_selftest_"))

PASS = 0
FAIL = 0


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  PASS  {name}")
    else:
        FAIL += 1
        print(f"  FAIL  {name}  {detail}")


# ---------------- 替身 ----------------
class FakeInfo:
    rc = 0

    def wait_for_publish(self, timeout=None):
        return None

    def is_published(self):
        return True


class FakeCli:
    """可编程替身：connect/subscribe 行为与回调由用例设定。"""

    def __init__(self, connect_exc=None, subscribe_rc=0,
                 connack_rc=0, suback=(0,), auto_conn=True):
        self.connect_exc = connect_exc
        self.subscribe_rc = subscribe_rc
        self.connack_rc = connack_rc
        self.suback = suback
        self.auto_conn = auto_conn
        self.connected = False
        self.on_connect = None
        self.on_message = None
        self.on_subscribe = None
        self.on_disconnect = None
        self.last_pub = None

    # paho 风格接口
    def connect(self, *a, **k):
        if self.connect_exc:
            raise self.connect_exc
        return 0

    def subscribe(self, topic, qos=0):
        if self.subscribe_rc != 0:
            return (self.subscribe_rc, None)
        return (0, 1)

    def loop_start(self):
        if self.connect_exc:
            return
        if self.auto_conn and self.on_connect:
            if self.connack_rc == 0:
                self.connected = True
            self.on_connect(self, None, {}, self.connack_rc, None)
            if self.connack_rc == 0 and self.suback is not None \
                    and self.on_subscribe:
                codes = [types.SimpleNamespace(value=c) for c in self.suback]
                self.on_subscribe(self, None, 1, codes, None)

    def loop_stop(self):
        pass

    def disconnect(self):
        self.connected = False

    def is_connected(self):
        return self.connected

    def publish(self, topic, payload, qos=0):
        self.last_pub = (topic, payload)
        return FakeInfo()


async def case_lifecycle():
    print("[1] 连接失败")
    r = S.Rpc("h", 1, cli=FakeCli(connect_exc=OSError("refused")))
    r.start(asyncio.get_running_loop())
    ok, why = await r.wait_ready(2.0)
    check("connect_exc → 未就绪", not ok and "connect_exc" in why, why)

    print("[2] 订阅失败")
    r = S.Rpc("h", 1, cli=FakeCli(subscribe_rc=4))
    r.start(asyncio.get_running_loop())
    ok, why = await r.wait_ready(2.0)
    check("subscribe rc≠0 → 未就绪", not ok and "subscribe_rc" in why, why)

    print("[3] SUBACK 拒绝")
    r = S.Rpc("h", 1, cli=FakeCli(suback=(128,)))
    r.start(asyncio.get_running_loop())
    ok, why = await r.wait_ready(2.0)
    check("suback 128 → 未就绪", not ok and "suback_rejected" in why, why)

    print("[3b] 正常就绪 + ACL 预检")
    r = S.Rpc("h", 1, cli=FakeCli(suback=(1,)))
    r.start(asyncio.get_running_loop())
    ok, why = await r.wait_ready(2.0)
    sub_ok, pub_ok, ev = r.acl_preflight()
    check("就绪且预检通过", ok and sub_ok and pub_ok, f"{ok}/{why}/{sub_ok}/{pub_ok}")


async def case_presence():
    print("[4] R4 监听未就绪")
    p = S.Presence("h", 1, cli=FakeCli(auto_conn=False))
    ok, why = await p.wait_ready(1.5)
    check("无 CONNACK → 未就绪", not ok and why == "ready_timeout", why)
    ok4, det4 = await S.r4_silence_check(p, 0.2)
    check("未就绪不得判静默通过", not ok4 and det4["stage"] == "LISTENER_NOT_READY",
          json.dumps(det4, ensure_ascii=False))

    print("[5] R4 观察期间收到设备消息")
    p = S.Presence("h", 1, cli=FakeCli(suback=(1,)))
    ok, why = await p.wait_ready(2.0)

    async def inject():
        await asyncio.sleep(0.1)
        p.seen.append((__import__("time").monotonic(),
                       "v1/devices/me/telemetry"))

    asyncio.ensure_future(inject())
    ok4, det4 = await S.r4_silence_check(p, 1.0)
    check("窗口内有消息 → 不通过", (not ok4) and det4["stage"] == "STILL_ON_EMQX",
          json.dumps(det4, ensure_ascii=False))

    print("[5b] R4 正常静默")
    p = S.Presence("h", 1, cli=FakeCli(suback=(1,)))
    ok4, det4 = await S.r4_silence_check(p, 0.6)
    check("就绪+静默 → 通过", ok4 and det4["msgs_in_window"] == 0,
          json.dumps(det4, ensure_ascii=False))


async def case_matching():
    print("[6] 严格 ID 匹配")
    cli = FakeCli(suback=(1,))
    r = S.Rpc("h", 1, cli=cli)
    r.start(asyncio.get_running_loop())
    await r.wait_ready(2.0)

    async def ok_case():
        fut = asyncio.ensure_future(r.call("operation.get", {}, timeout=2.0))
        for _ in range(60):
            await asyncio.sleep(0.02)
            rid = next(iter(r._waiters), None)
            if rid:
                msg = types.SimpleNamespace(
                    topic=f"v1/devices/me/rpc/response/{rid}",
                    payload=json.dumps({"id": rid, "ok": True,
                                        "data": {"state": "got_ip"}}).encode())
                r._on_msg(None, None, msg)
                break
        return await fut

    kind, resp = await ok_case()
    check("body.id+topic 一致 → OK", kind == "OK", str(resp)[:80])

    async def stale_case():
        fut = asyncio.ensure_future(r.call("network.status", {}, timeout=1.2))
        for _ in range(40):
            await asyncio.sleep(0.02)
            rid = next(iter(r._waiters), None)
            if rid:
                msg = types.SimpleNamespace(
                    topic="v1/devices/me/rpc/response/9999",
                    payload=json.dumps({"id": rid, "ok": True, "data": {}}).encode())
                r._on_msg(None, None, msg)
                break
        return await fut

    kind, why = await stale_case()
    check("串台 topic → TRANSPORT", kind == "TRANSPORT", str(why))

    print("[6b] 非本轮 op-ID 拒发")
    kind, why = await r.op_state("op-not-mine")
    check("registry 拒发", kind == "TRANSPORT" and "op_not_in_registry" in why, why)


def case_r2():
    print("[7] R2 changed 键校验")
    ok, det = S.r2_write_ok({"ok": True, "data": {"changed": ["mqtt_host"]}})
    check("缺 mqtt_port → 不通过", not ok and det["missing"] == ["mqtt_port"],
          json.dumps(det, ensure_ascii=False))
    ok, det = S.r2_write_ok({"ok": True, "data": {"changed": ["mqtt_host",
                                                             "mqtt_port"]}})
    check("两键齐全 → 通过", ok, json.dumps(det, ensure_ascii=False))
    ok, det = S.r2_write_ok({"ok": False, "data": {"changed": ["mqtt_host"]}})
    check("ok=false → 不通过", not ok, json.dumps(det, ensure_ascii=False))


def case_tcp():
    print("[8] TCP 预检")
    ok, why = S.tcp_precheck("127.0.0.1", 1, timeout=1.5)
    check("不可达端口 → False", not ok, why)


async def main():
    await case_lifecycle()
    await case_presence()
    await case_matching()
    case_r2()
    case_tcp()
    print(f"\nselftest: {PASS} passed, {FAIL} failed")
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
