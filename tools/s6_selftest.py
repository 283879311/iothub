#!/usr/bin/env python3
"""S6 v11.3.2 离线自检（不连设备/不连 broker，全部替身对象）。

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
        self.publish_count = 0
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
        self.publish_count += 1
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
                       "v1/devices/me/telemetry", "device"))

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


async def case_v113():
    print("[18] P0：Rpc 重连后新 SUBACK 未到 → call 必须拒发（不 publish）")
    cli = FakeCli(suback=(1,))
    r = S.Rpc("h", 1, cli=cli)
    r.start(asyncio.get_running_loop())
    ok, why = await r.wait_ready(2.0)
    check("首次就绪", ok, why)
    # 断链 → 重连，但新代 SUBACK 故意不送达
    cli.auto_suback = False
    cli.simulate_disconnect()
    cli.simulate_reconnect()
    n_before = cli.publish_count
    kind, why2 = await r.call("network.tryconnect",
                              {"ssid": "x", "password": "y"}, timeout=1.0)
    check("未就绪 → TRANSPORT", kind == "TRANSPORT", f"{kind}/{why2}")
    check("未就绪 → 零 publish（命令未发出）",
          cli.publish_count == n_before, f"{n_before}->{cli.publish_count}")
    check("原因为 not_ready", "not_ready" in str(why2), str(why2))
    # 本代 SUBACK 到达后可发
    cli.auto_suback = True
    cli._fire_suback(cli._mid)
    ok2, why3 = await r.wait_ready(1.5)
    check("本代 SUBACK → 恢复可发", ok2, why3)
    kind2, _ = await r.call("network.status", {}, timeout=0.6)
    check("恢复后 call 已发布", cli.publish_count == n_before + 1,
          f"{cli.publish_count}")

    print("[19] P1：Presence 首次 CONNACK 在 R4 启动后异步到达（应允许通过）")
    cli2 = FakeCli(auto_conn=False)     # 先不连：模拟真实 Paho 异步首连
    p2 = S.Presence("h", 1, cli=cli2)
    gen_after_construct = p2.generation

    async def first_conn_later():
        await asyncio.sleep(0.3)        # R4 已开始、gen_start 已记 0
        cli2._fire_conn()
        cli2._fire_suback(cli2._mid)

    asyncio.ensure_future(first_conn_later())
    ok4, det4 = await S.r4_silence_check(p2, 0.6)
    check("首次异步连接不误判断链", ok4, json.dumps(det4, ensure_ascii=False))
    check("构造时 generation=0（确为首次）", gen_after_construct == 0,
          str(gen_after_construct))

    print("[19b] Presence 初始化期间代次变化（子类模拟，非真实 Broker 重连）仍判负")
    cli3 = FakeCli(suback=(1,))
    p3 = S.Presence("h", 1, cli=cli3)
    await p3.wait_ready(2.0)            # 现在 generation=1

    class _ReconnectDuringReady(S.Presence):
        async def wait_ready(self, timeout=12.0):
            self.disconnect_count += 1
            self.generation += 1        # 模拟初始化期间重连
            return (True, "ready")

    p4 = _ReconnectDuringReady("h", 1, cli=FakeCli(suback=(1,)))
    p4.generation = 1                   # 已非首次
    ok4b, det4b = await S.r4_silence_check(p4, 0.2)
    check("初始化期间重连 → 判负",
          (not ok4b) and det4b["stage"] == "LISTENER_RESET_DURING_INIT",
          json.dumps(det4b, ensure_ascii=False))


async def case_v1131():
    print("[20] P1：ACL 预检未就绪 → 零 publish")
    cli = FakeCli(suback=(1,))
    r = S.Rpc("h", 1, cli=cli)
    r.start(asyncio.get_running_loop())
    await r.wait_ready(2.0)
    cli.auto_suback = False
    cli.simulate_disconnect()
    cli.simulate_reconnect()            # 新代 SUBACK 故意未到
    n_before = cli.publish_count
    sub_ok, pub_ok, det = r.acl_preflight()
    check("未就绪 → sub_ok=False", not sub_ok, str(det))
    check("未就绪 → published=False", det.get("published") is False, str(det))
    check("未就绪 → 零 publish", cli.publish_count == n_before,
          f"{n_before}->{cli.publish_count}")

    print("[21] P0：Presence 不得把 harness 自己的 rpc/request 预检当设备接入")
    cli2 = FakeCli(suback=(1,))
    p2 = S.Presence("h", 1, cli=cli2)
    ok, why = await p2.wait_ready(2.0)
    check("Presence 就绪", ok, why)
    t0 = __import__("time").monotonic()

    # 模拟 EMQX 回环：harness 发布的预检请求被 Presence 收到
    for topic in ("v1/devices/me/rpc/request/preflight-abc123",
                  "v1/devices/me/rpc/request/hp-1-dead"):
        msg = types.SimpleNamespace(topic=topic, payload=b"{}")
        p2._on_msg(None, None, msg)
    check("harness_request 不进入设备证据", p2.seen == [],
          str(p2.all_seen))
    got = await p2.wait(0.8, since=t0 - 1)
    check("预检消息不判为设备接入", got is False, str(p2.seen))
    ev = p2.evidence_class()
    check("证据分类记录 harness_request 计数",
          ev["harness_request_msgs"] == 2 and ev["device_msgs"] == 0, str(ev))

    print("[21b] 设备上报（telemetry/attributes/request/已登记 rid 的 rpc response）")
    S._harness_rids.add("hp-1-dead")     # v11.3.2：response 需与本轮请求关联
    for topic in ("v1/devices/me/telemetry",
                  "v1/devices/me/attributes/request/2588",
                  "v1/devices/me/rpc/response/hp-1-dead"):
        msg = types.SimpleNamespace(topic=topic, payload=b"{}", retain=False)
        p2._on_msg(None, None, msg)
    check("device 类进入证据", len(p2.seen) == 3, str(p2.evidence_class()))
    got2 = await p2.wait(0.5, since=t0 - 1)
    check("设备上报 → 判定接入", got2 is True, str(p2.seen))
    ev2 = p2.evidence_class()
    check("device_msgs=3 / harness_request=2",
          ev2["device_msgs"] == 3 and ev2["harness_request_msgs"] == 2, str(ev2))

    print("[21c] R4 窗口内只有 harness 预检消息 → 仍判静默通过")
    cli3 = FakeCli(suback=(1,))
    p3 = S.Presence("h", 1, cli=cli3)

    async def harness_noise():
        await asyncio.sleep(0.2)
        p3._on_msg(None, None, types.SimpleNamespace(
            topic="v1/devices/me/rpc/request/preflight-zzz", payload=b"{}"))

    asyncio.ensure_future(harness_noise())
    ok4, det4 = await S.r4_silence_check(p3, 0.6)
    check("harness 噪声不阻塞静默判定", ok4, json.dumps(det4, ensure_ascii=False))


async def case_v1132():
    print("[22] P0：retained 消息不得计入设备在线证据")
    cli = FakeCli(suback=(1,))
    p = S.Presence("h", 1, cli=cli)
    ok, why = await p.wait_ready(2.0)
    check("Presence 就绪", ok, why)
    t0 = __import__("time").monotonic()
    # Broker 重放历史 retained telemetry：topic 是 device 类，但 retain=True
    p._on_msg(None, None, types.SimpleNamespace(
        topic="v1/devices/me/telemetry", payload=b"{}", retain=True))
    check("retained 不进 seen", p.seen == [], str(p.all_seen))
    got = await p.wait(0.6, since=t0 - 1)
    check("retained 不判设备接入", got is False, str(p.seen))
    ev = p.evidence_class()
    check("证据记录 retained_msgs=1", ev["retained_msgs"] == 1, str(ev))
    # 同一主题的非 retained 消息才有效
    p._on_msg(None, None, types.SimpleNamespace(
        topic="v1/devices/me/telemetry", payload=b"{}", retain=False))
    check("非 retained 同主题 → 计入", len(p.seen) == 1, str(p.seen))

    print("[23] P0：attributes/response 与根 attributes 不计入设备证据")
    cli2 = FakeCli(suback=(1,))
    p2 = S.Presence("h", 1, cli=cli2)
    await p2.wait_ready(2.0)
    t1 = __import__("time").monotonic()
    for tp in ("v1/devices/me/attributes/response/abc",
               "v1/devices/me/attributes"):
        p2._on_msg(None, None, types.SimpleNamespace(
            topic=tp, payload=b"{}", retain=False))
    check("两类均不进 seen", p2.seen == [], str(p2.all_seen))
    got2 = await p2.wait(0.5, since=t1 - 1)
    check("不判设备接入", got2 is False, str(p2.seen))
    ev2 = p2.evidence_class()
    check("分类计数 server_response=1 / ambiguous=1",
          ev2["server_response_msgs"] == 1 and ev2["ambiguous_msgs"] == 1,
          str(ev2))
    # attributes/request 仍是设备来源
    p2._on_msg(None, None, types.SimpleNamespace(
        topic="v1/devices/me/attributes/request/2588",
        payload=b"{}", retain=False))
    check("attributes/request 计入", len(p2.seen) == 1, str(p2.seen))

    print("[24] P0：rpc/response 需与本轮请求 ID 关联")
    cli3 = FakeCli(suback=(1,))
    r = S.Rpc("h", 1, cli=cli3)
    r.start(asyncio.get_running_loop())
    await r.wait_ready(2.0)
    p3 = S.Presence("h", 1, cli=FakeCli(suback=(1,)))
    await p3.wait_ready(2.0)
    t2 = __import__("time").monotonic()
    # 未知 rid（不是本轮 harness 发出）→ 不计入
    p3._on_msg(None, None, types.SimpleNamespace(
        topic="v1/devices/me/rpc/response/unknown-999",
        payload=b"{}", retain=False))
    check("未知 rid → 不计入", p3.seen == [], str(p3.all_seen))
    # 本轮 rid（Rpc.call 会登记）→ 计入
    fut = asyncio.ensure_future(r.call("operation.get", {}, timeout=0.5))
    await asyncio.sleep(0.05)
    rid = next(iter(r._waiters), None)
    check("call 已登记 rid", rid is not None and rid in S._harness_rids, str(rid))
    if rid:
        p3._on_msg(None, None, types.SimpleNamespace(
            topic=f"v1/devices/me/rpc/response/{rid}",
            payload=json.dumps({"id": rid, "ok": True}).encode(), retain=False))
        msg = types.SimpleNamespace(
            topic=f"v1/devices/me/rpc/response/{rid}",
            payload=json.dumps({"id": rid, "ok": True,
                               "data": {"state": "got_ip"}}).encode())
        r._on_msg(None, None, msg)
    await fut
    check("本轮 rid → 计入设备证据", len(p3.seen) == 1, str(p3.seen))
    ev3 = p3.evidence_class()
    check("uncorrelated 计数=1", ev3["uncorrelated_rpc_response_msgs"] == 1,
          str(ev3))


async def case_v1132b():
    print("[25] P2-b：路径严格性（精确 telemetry / 单段非空 ID）")
    # 非精确 telemetry（嵌套）不得计入
    check("telemetry 嵌套 → other",
          S.classify_topic("v1/devices/me/telemetry/extra") == "other",
          S.classify_topic("v1/devices/me/telemetry/extra"))
    check("telemetry 精确 → device",
          S.classify_topic("v1/devices/me/telemetry") == "device")
    # attributes/request 必须单段非空 ID
    check("attributes/request 空 ID → other",
          S.classify_topic("v1/devices/me/attributes/request/") == "other",
          S.classify_topic("v1/devices/me/attributes/request/"))
    check("attributes/request 嵌套 → other",
          S.classify_topic("v1/devices/me/attributes/request/1/2") == "other",
          S.classify_topic("v1/devices/me/attributes/request/1/2"))
    check("attributes/request 单段 → device",
          S.classify_topic("v1/devices/me/attributes/request/2588") == "device")
    # rpc/response 必须单段且已登记
    S._harness_rids.add("hp-x-1")
    check("rpc/response 单段已登记 → device",
          S.classify_topic("v1/devices/me/rpc/response/hp-x-1") == "device")
    check("rpc/response 嵌套 → other",
          S.classify_topic("v1/devices/me/rpc/response/hp-x-1/x") == "other",
          S.classify_topic("v1/devices/me/rpc/response/hp-x-1/x"))
    # 嵌套路径消息进 Presence 也不计入 seen
    cli = FakeCli(suback=(1,))
    p = S.Presence("h", 1, cli=cli)
    await p.wait_ready(2.0)
    t0 = __import__("time").monotonic()
    for tp in ("v1/devices/me/telemetry/extra",
               "v1/devices/me/attributes/request/1/2",
               "v1/devices/me/rpc/response/hp-x-1/x"):
        p._on_msg(None, None, types.SimpleNamespace(
            topic=tp, payload=b"{}", retain=False))
    check("嵌套路径不进 seen", p.seen == [], str(p.all_seen))
    got = await p.wait(0.4, since=t0 - 1)
    check("嵌套路径不判接入", got is False, str(p.seen))


async def main():
    await case_lifecycle()
    await case_presence()
    await case_matching()
    case_r2()
    case_tcp()
    await case_v112()
    await case_v113()
    await case_v1131()
    await case_v1132()
    await case_v1132b()
    print(f"\nselftest: {PASS} passed, {FAIL} failed")
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
