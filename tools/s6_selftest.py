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
                 connack_rc=0, suback=(0,), auto_conn=True,
                 auto_suback=True):
        self.connect_exc = connect_exc
        self.subscribe_rc = subscribe_rc
        self.connack_rc = connack_rc
        self.suback = suback
        self.auto_conn = auto_conn
        self.auto_suback = auto_suback     # False = 新代 SUBACK 尚未到达
        self.connected = False
        self.on_connect = None
        self.on_message = None
        self.on_subscribe = None
        self.on_disconnect = None
        self.last_pub = None
        self._mid = 0

    def _fire_conn(self):
        if self.connack_rc == 0:
            self.connected = True
        if self.on_connect:
            self.on_connect(self, None, {}, self.connack_rc, None)

    def _fire_suback(self, mid):
        if self.auto_suback and self.suback is not None and self.on_subscribe:
            codes = [types.SimpleNamespace(value=c) for c in self.suback]
            self.on_subscribe(self, None, mid, codes, None)

    def simulate_disconnect(self):
        self.connected = False
        if self.on_disconnect:
            self.on_disconnect(self, None, {}, 7, None)

    def simulate_reconnect(self):
        self._fire_conn()
        if self.auto_suback:
            self._fire_suback(self._mid)

    # paho 风格接口
    def connect(self, *a, **k):
        if self.connect_exc:
            raise self.connect_exc
        return 0

    def subscribe(self, topic, qos=0):
        if self.subscribe_rc != 0:
            return (self.subscribe_rc, None)
        self._mid += 1
        return (0, self._mid)

    def loop_start(self):
        if self.connect_exc:
            return
        if self.auto_conn:
            self._fire_conn()
            if self.auto_suback:
                self._fire_suback(self._mid)

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


async def case_v112():
    print("[9] 重连后旧 SUBACK 不得代表新连接就绪（Presence）")
    cli = FakeCli(suback=(1,))
    p = S.Presence("h", 1, cli=cli)
    ok, why = await p.wait_ready(2.0)
    check("首次就绪", ok, why)
    cli.auto_suback = False                 # 新代 SUBACK 未到达
    cli.simulate_disconnect()
    cli.simulate_reconnect()
    ok2, why2 = await p.wait_ready(1.2)
    check("旧 SUBACK 不复用 → 未就绪", (not ok2) and why2 == "ready_timeout", why2)
    cli.auto_suback = True
    cli._fire_suback(cli._mid)               # 新代 SUBACK 到达
    ok3, why3 = await p.wait_ready(1.2)
    check("本代 SUBACK → 恢复就绪", ok3, why3)

    print("[10] 非本代 mid 的 SUBACK 被忽略")
    cli2 = FakeCli(suback=(1,))
    p2 = S.Presence("h", 1, cli=cli2)
    await p2.wait_ready(2.0)
    p2._on_suback(None, None, 999, [types.SimpleNamespace(value=0)])  # 错 mid
    check("错 mid 不覆盖", p2._suback == [types.SimpleNamespace(value=1)]
          or getattr(p2._suback[0], "value", p2._suback[0]) == 1,
          str(p2._suback))

    print("[11] R4：初始化期间断链 → LISTENER_RESET_DURING_INIT")

    class ReconnectDuringReady(S.Presence):
        async def wait_ready(self, timeout=12.0):
            self.disconnect_count += 1
            return (True, "ready")

    rp = ReconnectDuringReady("h", 1, cli=FakeCli(suback=(1,)))
    ok, det = await S.r4_silence_check(rp, 0.2)
    check("初始化断链 → 不通过", (not ok) and det["stage"] == "LISTENER_RESET_DURING_INIT",
          json.dumps(det, ensure_ascii=False))

    print("[12] R4：观察期间断链 → LISTENER_DISCONNECTED")
    p3 = S.Presence("h", 1, cli=FakeCli(suback=(1,)))
    await p3.wait_ready(2.0)

    async def dc_mid():
        await asyncio.sleep(0.2)
        p3.cli.simulate_disconnect()

    asyncio.ensure_future(dc_mid())
    ok, det = await S.r4_silence_check(p3, 1.0)
    check("窗口中断链 → 不通过", (not ok) and det["stage"] == "LISTENER_DISCONNECTED",
          json.dumps(det, ensure_ascii=False))

    print("[13] R4：结束时已失去连接 → LISTENER_LOST_AT_END")
    p4 = S.Presence("h", 1, cli=FakeCli(suback=(1,)))
    await p4.wait_ready(2.0)

    async def dc_end():
        await asyncio.sleep(0.5)
        p4._connected = False              # 连接丢失但不触发 on_disconnect

    asyncio.ensure_future(dc_end())
    ok, det = await S.r4_silence_check(p4, 0.8)
    check("结束失连 → 不通过", (not ok) and det["stage"] == "LISTENER_LOST_AT_END",
          json.dumps(det, ensure_ascii=False))

    print("[14] Rpc 断链后重连可恢复（策略：完整重新就绪）")
    cli3 = FakeCli(suback=(1,))
    r = S.Rpc("h", 1, cli=cli3)
    r.start(asyncio.get_running_loop())
    ok, why = await r.wait_ready(2.0)
    check("首次就绪", ok, why)
    cli3.simulate_disconnect()
    kind, _ = await r.call("network.status", {}, timeout=1.0)
    check("断链时 call → TRANSPORT", kind == "TRANSPORT", kind)
    cli3.simulate_reconnect()
    ok2, why2 = await r.wait_ready(2.0)
    check("重连后就绪且不沿用旧 SUBACK", ok2, why2)

    print("[15] R2 精确分类")
    st, det = S.r2_classify({"data": {"changed": []}})
    check("缺 ok → UNCONFIRMED", st == "UNCONFIRMED", st)
    st, _ = S.r2_classify({"ok": False, "data": {"changed": []}})
    check("明确 ok:false → REJECTED", st == "REJECTED", st)
    st, _ = S.r2_classify({"ok": True, "data": {"changed": "mqtt_host"}})
    check("changed 非数组 → UNCONFIRMED", st == "UNCONFIRMED", st)
    st, _ = S.r2_classify({"ok": True, "data": {"changed": ["mqtt_host", 1]}})
    check("changed 含非字符串 → UNCONFIRMED", st == "UNCONFIRMED", st)
    st, _ = S.r2_classify({"ok": True, "data": {"changed": ["mqtt_host", "mqtt_port"]}})
    check("两键齐全 → OK", st == "OK", st)

    print("[16] TCP 门禁：成功/失败分支固定（mock）")
    import socket as _socket

    class _FakeSock:
        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

    orig = _socket.create_connection
    try:
        _socket.create_connection = lambda *a, **k: _FakeSock()
        ok, why = S.tcp_precheck("x", 1)
        check("mock 成功 → True", ok, why)
        def _boom(*a, **k):
            raise OSError("refused")
        _socket.create_connection = _boom
        ok2, why2 = S.tcp_precheck("x", 1)
        check("mock 失败 → False", not ok2, why2)
    finally:
        _socket.create_connection = orig

    print("[17] 正式流程:TCP 门禁位于改指向之前")
    src = Path(S.__file__).read_text(encoding="utf-8")
    i_gate = src.index("if not tcp_gate():")
    i_repoint = src.index('{"patch": {"mqtt_host": EMQX_HOST')
    check("tcp_gate 调用早于改指向", i_gate < i_repoint,
          f"gate@{i_gate} repoint@{i_repoint}")


async def main():
    await case_lifecycle()
    await case_presence()
    await case_matching()
    case_r2()
    case_tcp()
    await case_v112()
    print(f"\nselftest: {PASS} passed, {FAIL} failed")
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
