#!/usr/bin/env python3
"""S6 现场预采集执行脚本 v11.3.6（MQTT 轮询 + 分阶段恢复；窄授权：仅 MQTT async expired
（须先观察到 GOT_IP）+ op-V job-clear 验证）。

v4→v5（评审阻断项与缺口）：
P0-1 证据目录新鲜度检查移到 mkdir/log 之前（any(OUT.iterdir())，非空拒绝）；
P0-2 统一受保护命令入口 ble()（_abort 拦截+逐命令 token+allow_relogin/
     reconnect=False+retries 可控），移除 gate_ble 双轨（改指向等全部走它）；
P1 改指向未获"成功回包+回读"双重确认 → 立即 STOP（恢复兜底仍由状态机保证）；
P1 cleanup_confirm 真总预算：睡眠/请求前/请求后都查同一 deadline；剩余不足
   不发请求；窗口内请求 retries=0；token 续签耗时计入同一预算；文件按窗口
   分名（retry-pre / post-expired），不再互相覆盖；
P1 全部退出路径显式设置 _results；000_end_state 写入 results；退出码 0 =
   两项 PASS **且** 设备恢复完成（改指向已尝试时必须 RECOVERY_CONFIRMED）；
P2 session_trace 实际生成（stdout Tee：脱敏截断 TX/RX 跟踪流，非原始字节流，
   口径见 docstring）；结束日志全部落盘并关闭后再生成 SHA256SUMS。
v10→v11（2026-11-15 评审裁决）：① WiFi 切换期轮询改走 **MQTT RPC**（EMQX，
   独立 client_id/唯一请求 ID 严格匹配/串行/ACL 预检/仅查本轮 op/传输失败记
   POLL_TRANSPORT_FAILED 绝不重发 tryconnect）；② 恢复分阶段 R1–R5，仅全部
   通过才 RECOVERY_CONFIRMED，无法确认记 RECOVERY_UNCONFIRMED；③ 零自动重试
   （connect_failed 维持立即 STOP，ACCEPTED_TTL 例外默认关闭）；④ 目标 SSID
   必须显式传入且不得等于持久/当前 SSID。
v9→v10（2026-11-14 现场裁决）：**删除本地 mini_broker**（无自建服务、无残留
   进程风险）；临时指向与 harness 接入点改为 **EMQX 139.9.93.245:1883**
   （匿名已实测）；设备在场检测改 **broker 侧遥测订阅**（v1/devices/me/#，
   比 RPC 往返直接），窗口 240s（覆盖实测 6 级 MQTT backoff ≈3min）；
   **恢复通道支持独立重连**（原 session 断链时重扫 BLE + 重连 ≤3 次再写回，
   固化第二轮人工恢复路径）；证据目录 s6_round3。
v6→v9（收尾加固）：gEnd 初始化移出会话分支（无会话不 UnboundLocalError）；
   _broker 模块级 None 初始化；口令缺失 SystemExit→RuntimeError；broker
   terminate/kill 后 wait 确认并逐一兜底（含 kill 后 TimeoutExpired）；经
   评审 v7/v8/v9 逐版签收。
v5→v6：① guarded_login 修正 sess.login() 接口（无 timeout 形参——原
   timeout=15 会 TypeError）；② 扫描经统一 ble()（full=True 返回 Response），
   不再保留第二套直接发送路径；③ cleanup_confirm 内 token gate 与 BLE 请求
   全部 asyncio.wait_for(剩余预算) 封顶（含登录耗时）；睡眠不设超预算下限；
   ④ 000_end_state 覆盖全部启动/退出路径（统一外层 finally；未进会话时
   gate=None、结果 NOT_RUN）；⑤ 授权申请版本标记同步。
"""

import asyncio
import hashlib
import json
import math
import os
import re
import sys
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "ble_p0"))

from bleak import BleakClient  # noqa: E402

from p0_acceptance import Session, scan_filtered, sanitize  # noqa: E402

OUT = Path("artifacts/cmd04_flash_20261115/s6_round4")
# 串口 console 日志所在目录（阶段 A 起持续录制的固定位置；不随轮次目录变化）
SERIAL_DIR = Path("artifacts/cmd04_flash_20261114/phaseA")
TARGET = "B0:A7:32:28:C8:E6"
EMQX_HOST, EMQX_PORT = "139.9.93.245", 1883   # 临时指向与 harness 接入点（外部 EMQX，匿名）
TB_HOST, TB_PORT = "139.9.93.245", 21883        # 恢复目标（1 号 ThingsBoard）
CLOUD_HOST, CLOUD_PORT = TB_HOST, TB_PORT          # 授权基线（守卫用）= 恢复目标
SSID = "CMCC-502"
PROBE = "__jobclear_probe__"
# 设备在场证据（v11.3.2 P0）：只认**方向明确为设备→服务器**的主题。
#   telemetry                  设备上报
#   attributes/request/<id>    设备发起属性请求
#   rpc/response/<rid>         **且 rid ∈ 本轮 harness 发出的请求**（方向关联）
# 明确排除：
#   rpc/request/*              harness 自己发布（含 ACL 预检）→ 回环会假通过
#   attributes/response/<id>   服务器→设备（不是设备在线证据）
#   attributes（根路径）        双向语义，方向不确定
TELEMETRY_PREFIX = "v1/devices/me/telemetry"
ATTR_REQUEST_PREFIX = "v1/devices/me/attributes/request/"
ATTR_RESPONSE_PREFIX = "v1/devices/me/attributes/response/"
ATTR_ROOT = "v1/devices/me/attributes"
RPC_REQUEST_PREFIX = "v1/devices/me/rpc/request/"
RPC_RESPONSE_PREFIX = "v1/devices/me/rpc/response/"

_harness_rids = set()      # 本轮 harness 发出的 RPC 请求 ID（用于方向关联）


def classify_topic(topic):
    """返回 device / harness_request / server_response / ambiguous /
    uncorrelated_rpc_response / other。只有 device 计入在场证据。"""
    if topic.startswith(RPC_REQUEST_PREFIX):
        return "harness_request"
    if topic == TELEMETRY_PREFIX:          # 设备端就是精确主题
        return "device"
    if topic.startswith(ATTR_REQUEST_PREFIX):
        rid = topic[len(ATTR_REQUEST_PREFIX):]
        if rid and "/" not in rid:         # 仅单段非空 ID，排除非标准嵌套
            return "device"
        return "other"
    if topic.startswith(ATTR_RESPONSE_PREFIX):
        return "server_response"
    if topic == ATTR_ROOT:
        return "ambiguous"
    if topic.startswith(RPC_RESPONSE_PREFIX):
        rid = topic[len(RPC_RESPONSE_PREFIX):]
        if not rid or "/" in rid:
            return "other"                 # 非标准嵌套
        return "device" if rid in _harness_rids else "uncorrelated_rpc_response"
    return "other"
TOKEN_LIMIT_S = 480.0
ALLOWED_PERSIST_SSID = ("CMCC-502", "TQ2026")

RP_NA, RP_ATT, RP_CONF = "NOT_ATTEMPTED", "ATTEMPTED_OR_UNKNOWN", "REPOINT_CONFIRMED"
RC_OK = "RECOVERY_CONFIRMED"
RC_FAIL = "RECOVERY_FAILED"
RC_UNCONF = "RECOVERY_UNCONFIRMED"

_log = None
_token_origin = None
_abort = None
_session = None
_repoint_state = RP_NA
_recovery_state = None
_origin_snapshot = None
_results = {"op_e": "NOT_RUN", "op_v": "NOT_RUN"}
_op_registry = set()    # 仅本轮创建的 op-ID（operation.get 只查这些）
_ssid = None            # 目标测试 SSID（必须显式传入且不等于持久/当前）

# Session 日志脱敏口径（评审 P2）：本 harness 的 TX/RX 记录为 JSON 脱敏截断流
# （token/password 字段掩码、单帧截断 800/1200 字符），**不是原始字节流**；
# 原始 GATT 帧落盘涉及明文凭据脱敏，列为后续增强，本轮证据口径如实标注。


def log(t):
    line = f"{time.strftime('%H:%M:%S')} {t}"
    print(line, flush=True)
    if _log is not None:
        _log.write(line + "\n")
        _log.flush()


def save(branch, source, request, response):
    idx = len(list(OUT.glob("[0-9][0-9][0-9]_*.json"))) + 1
    path = OUT / f"{idx:03d}_{branch}.json"
    path.write_text(json.dumps({
        "branch": branch, "source": source,
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "host_mono": round(time.monotonic(), 3),
        "request": sanitize(request) if isinstance(request, dict) else request,
        "response": response,
    }, ensure_ascii=False, indent=1), encoding="utf-8")
    log(f"[save] {path.name}")


def stop(reason):
    global _abort
    if _abort is None:
        _abort = reason
    log(f"[STOP] {reason}")


def token_age():
    return 0.0 if _token_origin is None else time.monotonic() - _token_origin


async def guarded_login(sess):
    """三件套同一成功条件：登录成功 + token 更新 + 起点重置。"""
    global _token_origin
    r = await sess.login()   # Session.login(username,password) 无 timeout 形参（v5 TypeError 修正）；内部自带 timeout=10/retries=1
    if r.obj.get("ok") is not True or not sess.token:
        stop("auth.login 失败或 token 未更新")
        return False
    _token_origin = time.monotonic()
    log("[token] 续签成功（age→0，起点已重置）")
    return True


async def _token_gate(sess):
    if token_age() >= TOKEN_LIMIT_S:
        return await guarded_login(sess)
    return True


async def ble(sess, cmd, args=None, timeout=20, retries=3, full=False):
    """统一受保护命令入口：_abort 拦截 + 逐命令 token 守卫 +
    allow_relogin/reconnect=False（token 生命周期由脚本独占）。
    full=True 返回完整 Response（扫描需要 frag_sizes）——不保留第二套
    直接发送路径。
    v10：链路中断时 ensure_session() 重连 ≤2 次并重发一次（第二轮教训）。"""
    if _abort:
        raise RuntimeError(f"aborted: {_abort}")
    s = _session if (_session is not None and _session.is_connected()) else sess
    if not await _token_gate(s):
        raise RuntimeError("token renewal failed")
    req = {"id": f"s6-{s.next_seq()}", "version": "1.0", "cmd": cmd,
           "args": args or {}, "token": s.token}
    try:
        res = await s.send_request(req, timeout=timeout, retries=retries,
                                   allow_relogin=False, allow_reconnect=False)
    except Exception as exc:  # noqa: BLE001
        if type(exc).__name__ not in ("ConnectionLost", "BleakError"):
            raise
        log(f"[ble] 链路中断于 {cmd}（{type(exc).__name__}）→ 重连后重发")
        s2 = await ensure_session(tries=2)
        if s2 is None:
            raise
        req["token"] = s2.token
        res = await s2.send_request(req, timeout=timeout, retries=retries,
                                    allow_relogin=False, allow_reconnect=False)
    save(f"{cmd.replace('.', '_')}_ble", "ble", req, res.obj)
    return res if full else res.obj


async def recover_cmd(sess, cmd, args=None, timeout=20):
    """恢复 allowlist：不受 _abort 阻断；逐命令 token 守卫；白名单命令。"""
    if cmd not in ("config.set", "config.get", "network.status", "device.status",
                   "device.info"):
        raise RuntimeError(f"recover_cmd 拒绝非恢复命令 {cmd}")
    s = _session if (_session is not None and _session.is_connected()) else sess
    if not await _token_gate(s):
        raise RuntimeError("token renewal failed")
    req = {"id": f"rcv-{s.next_seq()}", "version": "1.0", "cmd": cmd,
           "args": args or {}, "token": s.token}
    try:
        res = await s.send_request(req, timeout=timeout, retries=0,
                                   allow_relogin=False, allow_reconnect=False)
    except Exception as exc:  # noqa: BLE001
        if type(exc).__name__ not in ("ConnectionLost", "BleakError"):
            raise
        log(f"[recover] 链路中断于 {cmd}（{type(exc).__name__}）→ 重连后重发")
        s2 = await ensure_session(tries=2)
        if s2 is None:
            raise
        req["token"] = s2.token
        res = await s2.send_request(req, timeout=timeout, retries=0,
                                    allow_relogin=False, allow_reconnect=False)
    save(f"recover_{cmd.replace('.', '_')}", "ble", req, res.obj)
    return res.obj


async def ble_scan_full(sess):
    """受保护扫描：经统一 ble() 发送（v6——不保留第二套直接发送路径）。"""
    res = await ble(sess, "network.scan", timeout=30, retries=0, full=True)
    return res.obj, res.frag_sizes


def scan_five_conditions(sc, frag_sizes):
    d = sc.get("data") or {}
    aps = d.get("aps")
    reasons = []
    if sc.get("ok") is not True:
        reasons.append("envelope ok!=true")
    if d.get("status") != "ok":
        reasons.append(f"data.status={d.get('status')!r}")
    if not frag_sizes:
        reasons.append("无分片记录")
    if not isinstance(aps, list):
        reasons.append(f"aps 非数组({type(aps).__name__})")
    else:
        if d.get("count") != len(aps):
            reasons.append(f"count={d.get('count')} != len(aps)={len(aps)}")
        if any((a or {}).get("ssid") == PROBE for a in aps):
            reasons.append("probe SSID 在结果中")
    return (not reasons), reasons


class Rpc:
    """MQTT RPC 轮询通道（v11）：独立 client_id、唯一请求 ID、串行、严格匹配。
    传输层失败一律返回 ("TRANSPORT", 原因)——绝不与设备语义终态混淆。"""

    def __init__(self, host, port, cli=None):
        import paho.mqtt.client as mqtt
        self._mqtt = mqtt
        self.host, self.port = host, port
        self.client_id = f"iothub-harness-{uuid.uuid4().hex[:8]}"
        self.cli = cli if cli is not None else mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2, client_id=self.client_id,
            protocol=mqtt.MQTTv311)
        self._waiters = {}         # rid -> (asyncio.Event, holder)
        self._suback = None
        self._connack = None
        self._subscribe_rc = None
        self._connected = False
        self.connect_error = None
        self.generation = 0        # v11.2：连接代次
        self._subscribe_mid = None
        self._suback_mid = None
        self._loop = None
        self._lock = asyncio.Lock()
        self.seq = 0
        self.cli.on_connect = self._on_conn
        self.cli.on_message = self._on_msg
        self.cli.on_subscribe = self._on_suback
        self.cli.on_disconnect = self._on_disc
        self.disconnected = False

    def _on_conn(self, _c, _u, _flags, rc, _props=None):
        """CONNACK 之后才订阅。v11.2：每次 CONNACK 开新代次并清空旧订阅状态，
        旧代 SUBACK 永不代表新连接就绪。"""
        self.generation += 1
        self._suback = None
        self._suback_mid = None
        self._subscribe_rc = None
        self._subscribe_mid = None
        self._connack = rc
        if int(getattr(rc, "value", rc)) != 0:
            self.connect_error = f"connack_rc={rc}"
            self._connected = False
            return
        self._connected = True
        self.disconnected = False        # 可恢复策略：新代次重新就绪
        self.connect_error = None
        try:
            result, mid = self.cli.subscribe(
                "v1/devices/me/rpc/response/+", 1)
            self._subscribe_rc = int(result)
            self._subscribe_mid = mid
        except Exception as exc:  # noqa: BLE001
            self._subscribe_rc = -1
            self.connect_error = f"subscribe_exc:{type(exc).__name__}"

    def _on_suback(self, _c, _u, mid, reason_codes, _props=None):
        # 只接受本代 SUBSCRIBE 的 mid
        if self._subscribe_mid is None or mid != self._subscribe_mid:
            return
        self._suback = list(reason_codes or [])
        self._suback_mid = mid

    def _on_disc(self, _c, _u, _flags, _rc, _props=None):
        self.disconnected = True
        self._connected = False

    def _is_ready(self):
        """当前连接代次的完整就绪判据（v11.3）：wait_ready/call/acl_preflight
        共用同一口径。返回 (bool, 原因)。"""
        if self.connect_error:
            return (False, self.connect_error)
        if self.disconnected or not self._connected \
                or not self.cli.is_connected():
            return (False, "emqx_disconnected")
        if self._subscribe_rc != 0:
            return (False, f"subscribe_rc={self._subscribe_rc}")
        if self._subscribe_mid is None:
            return (False, "no_subscribe_mid")
        if self._suback is None:
            return (False, "suback_pending")
        if self._suback_mid != self._subscribe_mid:
            return (False, "suback_stale_generation")
        if not self._suback:
            return (False, "suback_empty")
        bad = [str(rc) for rc in self._suback
               if int(getattr(rc, "value", rc)) == 128]
        if bad:
            return (False, f"suback_rejected={bad}")
        return (True, "ready")

    async def wait_ready(self, timeout=12.0):
        """连接 → 订阅 → SUBACK 授权 全部确认后才允许使用。"""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            ok, why = self._is_ready()
            if ok:
                return (True, "ready")
            # 终态类原因不必等：连接被拒/订阅被拒/授权被拒
            if why.startswith(("connack_rc", "subscribe_exc", "subscribe_rc=",
                               "suback_rejected", "suback_empty")):
                return (False, why)
            await asyncio.sleep(0.1)
        ok, why = self._is_ready()
        return (ok, "ready" if ok else f"ready_timeout:{why}")

    def _on_msg(self, _c, _u, msg):
        try:
            obj = json.loads(msg.payload.decode("utf-8", "replace"))
        except Exception:  # noqa: BLE001
            return
        rid = obj.get("id")
        topic_rid = msg.topic.rsplit("/", 1)[-1]
        # 严格匹配：body.id 必须等于在飞 rid，且 topic 尾段一致（防串台）
        if rid not in self._waiters or topic_rid != str(rid):
            return
        ev, holder = self._waiters[rid]
        holder["resp"] = obj
        if self._loop is not None:
            self._loop.call_soon_threadsafe(ev.set)

    def start(self, loop):
        self._loop = loop
        try:
            self.cli.connect(self.host, self.port, keepalive=20)
        except Exception as exc:  # noqa: BLE001
            self.connect_error = f"connect_exc:{type(exc).__name__}"
        self.cli.loop_start()

    def acl_preflight(self):
        """发布侧预检（改指向前执行）：先过同一 _is_ready() 判据，再验发布。
        **注意**：这验证的是 harness 自身客户端身份的主题权限，不代表设备
        身份的连接/订阅权限；且预检发布的 device.info 是真实请求（共享主题
        结构，若有其他设备订阅同主题它们也会收到）——证据口径如实标注。"""
        r_ok, r_why = self._is_ready()
        sub_ok = r_ok
        if not r_ok:
            # v11.3.1 P1：未就绪连预检请求也不发（与 call() 规则一致）
            return (False, False, {"ready": False, "ready_why": r_why,
                                   "published": False,
                                   "suback": [str(x) for x in (self._suback or [])],
                                   "client_id": self.client_id})
        topic = f"v1/devices/me/rpc/request/preflight-{uuid.uuid4().hex[:6]}"
        info = self.cli.publish(topic, b'{"id":"preflight","version":"1.0",'
                                       b'"cmd":"device.info","args":{}}', qos=1)
        try:
            info.wait_for_publish(timeout=8)
        except Exception:  # noqa: BLE001
            pass
        pub_ok = info.is_published() and info.rc == self._mqtt.MQTT_ERR_SUCCESS
        return (sub_ok, pub_ok, {"ready": r_ok, "ready_why": r_why,
                                 "published": True,
                                 "suback": [str(r) for r in (self._suback or [])],
                                 "pub_rc": info.rc, "client_id": self.client_id})

    async def call(self, cmd, args=None, timeout=12.0):
        """串行 RPC。返回 ("OK", data) 或 ("TRANSPORT", 原因)。"""
        async with self._lock:
            ok, why = self._is_ready()
            if not ok:
                # v11.3 P0：未就绪绝不发布（新代 SUBACK 未到时禁止发命令）
                return ("TRANSPORT", f"not_ready:{why}")
            self.seq += 1
            rid = f"hp-{self.seq}-{uuid.uuid4().hex[:4]}"
            _harness_rids.add(rid)     # 供 Presence 关联 rpc/response 方向
            ev = asyncio.Event()
            self._waiters[rid] = (ev, {})
            try:
                payload = json.dumps({"id": rid, "version": "1.0", "cmd": cmd,
                                      "args": args or {}},
                                     separators=(",", ":")).encode()
                info = self.cli.publish(
                    f"v1/devices/me/rpc/request/{rid}", payload, qos=1)
                try:
                    info.wait_for_publish(timeout=5)
                except Exception:  # noqa: BLE001
                    pass
                save(f"{cmd.replace('.', '_')}_mqtt", "mqtt",
                     {"id": rid, "cmd": cmd, "args": args or {},
                      "client_id": self.client_id},
                     {"_note": "pending"})
                try:
                    await asyncio.wait_for(ev.wait(), timeout=timeout)
                except (asyncio.TimeoutError, TimeoutError):
                    return ("TRANSPORT", f"timeout:{cmd}")
                resp = self._waiters[rid][1].get("resp") or {}
                if resp.get("ok") is not True and "error" not in resp:
                    return ("TRANSPORT", f"malformed:{cmd}")
                return ("OK", resp)
            finally:
                self._waiters.pop(rid, None)

    async def op_state(self, op_id):
        """仅查询本轮创建的 op（评审条件 5）。"""
        if op_id not in _op_registry:
            return ("TRANSPORT", f"op_not_in_registry:{op_id}")
        kind, resp = await self.call("operation.get", {"operation_id": op_id})
        if kind != "OK":
            return (kind, resp)
        return ("OK", (resp.get("data") or {}))

    async def status(self):
        kind, resp = await self.call("network.status", {})
        if kind != "OK":
            return (kind, resp)
        return ("OK", (resp.get("data") or {}))

    def stop(self):
        try:
            self.cli.loop_stop()
            self.cli.disconnect()
        except Exception:  # noqa: BLE001
            pass


class Presence:
    """broker 侧在场检测（v11.3.2）：
    订阅 v1/devices/me/# 仅用于收集，**只有经主题方向分类为 device 且非
    retained 的消息**才计入 seen / wait() / R4 证据；harness 自身 rpc/request
    （含 ACL 预检）、服务器侧 attributes/response、根 attributes、未关联的
    rpc/response、retained 重放均只进 all_seen 日志。"""

    def __init__(self, host, port, cli=None):
        import paho.mqtt.client as mqtt
        self.seen = []          # 仅 device 类（在场证据）
        self.all_seen = []      # 全部（含 harness_request/other，仅日志）
        self._mqtt = mqtt
        self._connack = None
        self._suback = None
        self._subscribe_rc = None
        self._connected = False
        self.connect_error = None
        self.disconnect_count = 0
        self.generation = 0        # v11.2：连接代次
        self._subscribe_mid = None
        self._suback_mid = None
        self.cli = cli if cli is not None else mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2, protocol=mqtt.MQTTv311)
        self.cli.on_connect = self._on_conn
        self.cli.on_subscribe = self._on_suback
        self.cli.on_disconnect = self._on_disc
        self.cli.on_message = self._on_msg
        try:
            self.cli.connect(host, port, keepalive=20)
        except Exception as exc:  # noqa: BLE001
            self.connect_error = f"connect_exc:{type(exc).__name__}"
        self.cli.loop_start()

    def _on_conn(self, _c, _u, _flags, rc, _props=None):
        self.generation += 1
        self._suback = None
        self._suback_mid = None
        self._subscribe_rc = None
        self._subscribe_mid = None
        self._connack = rc
        if int(getattr(rc, "value", rc)) != 0:
            self.connect_error = f"connack_rc={rc}"
            self._connected = False
            return
        self._connected = True
        self.connect_error = None
        try:
            result, mid = self.cli.subscribe("v1/devices/me/#", 1)
            self._subscribe_rc = int(result)
            self._subscribe_mid = mid
        except Exception as exc:  # noqa: BLE001
            self._subscribe_rc = -1
            self.connect_error = f"subscribe_exc:{type(exc).__name__}"

    def _on_suback(self, _c, _u, mid, reason_codes, _props=None):
        if self._subscribe_mid is None or mid != self._subscribe_mid:
            return
        self._suback = list(reason_codes or [])
        self._suback_mid = mid

    def _on_disc(self, _c, _u, _flags, _rc, _props=None):
        self._connected = False
        self.disconnect_count += 1

    async def wait_ready(self, timeout=12.0):
        """订阅就绪判据：CONNACK 成功 + subscribe rc 正常 + SUBACK 授权。
        未就绪时不得把"没收到消息"当作任何证据。"""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.connect_error:
                return (False, self.connect_error)
            if self._subscribe_rc not in (None, 0):
                return (False, f"subscribe_rc={self._subscribe_rc}")
            if self._connected and self._suback is not None \
                    and self._suback_mid == self._subscribe_mid:
                if not self._suback:
                    return (False, "suback_empty")
                bad = [str(rc) for rc in self._suback
                       if int(getattr(rc, "value", rc)) == 128]
                if bad:
                    return (False, f"suback_rejected={bad}")
                return (True, "ready")
            await asyncio.sleep(0.1)
        return (False, "ready_timeout")

    def is_alive(self):
        """当前连接是否仍有效（R4 开始/结束都要校验）。"""
        try:
            return bool(self._connected and self.cli.is_connected())
        except Exception:  # noqa: BLE001
            return False

    def is_ready(self):
        """当前代次的订阅是否已确认生效（v11.3：R4 用同一口径，不只 is_alive）。"""
        if not self.is_alive() or self._subscribe_rc != 0:
            return False
        if self._subscribe_mid is None or self._suback is None:
            return False
        if self._suback_mid != self._subscribe_mid or not self._suback:
            return False
        return all(int(getattr(rc, "value", rc)) != 128 for rc in self._suback)

    def _on_msg(self, _c, _u, msg):
        topic = getattr(msg, "topic", "?")
        retained = bool(getattr(msg, "retain", False))
        # v11.3.2 P0：retained 是 Broker 在新订阅时重放的历史消息，
        # 不代表设备当前在线——只记日志，不进 seen、不参与 wait()/R4。
        klass = "retained" if retained else classify_topic(topic)
        self.all_seen.append((time.monotonic(), topic, klass))
        if klass == "device":
            self.seen.append((time.monotonic(), topic, klass))

    def last_mono(self):
        return self.seen[-1][0] if self.seen else None

    def evidence_class(self):
        """证据来源摘要（写入归档，便于复核是哪类消息触发）。"""
        cnt = {}
        for _t, _tp, k in self.all_seen:
            cnt[k] = cnt.get(k, 0) + 1
        return {"device_msgs": len(self.seen),
                "harness_request_msgs": cnt.get("harness_request", 0),
                "retained_msgs": cnt.get("retained", 0),
                "server_response_msgs": cnt.get("server_response", 0),
                "ambiguous_msgs": cnt.get("ambiguous", 0),
                "uncorrelated_rpc_response_msgs":
                    cnt.get("uncorrelated_rpc_response", 0),
                "other_msgs": cnt.get("other", 0)}

    async def wait(self, seconds=240.0, since=None):
        """等到 since 之后的首条消息。调用前必须 wait_ready() 成功。"""
        base = since if since is not None else 0.0
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            lm = self.last_mono()
            if lm is not None and lm >= base:
                return True
            await asyncio.sleep(2)
        return False

    def stop(self):
        try:
            self.cli.loop_stop()
            self.cli.disconnect()
        except Exception:  # noqa: BLE001
            pass


def load_password(ssid):
    """按目标 SSID 从 test-env.md 取口令（大小写不敏感，兼容 Password:）。"""
    txt = Path(".local/test-env.md").read_text(encoding="utf-8")
    m = re.search(re.escape(ssid) + r"\s+PASSWORD:\s*(\S+)", txt, re.IGNORECASE)
    if not m:
        raise RuntimeError(f"test-env.md 无 {ssid} 口令行")
    return m.group(1)


async def ssid_precheck(sess, ssid):
    """评审裁决：目标 SSID 必须 ≠ 持久 SSID 且 ≠ 当前已连 SSID；否则拒绝运行。"""
    cfg = (await ble(sess, "config.get")).get("data", {})
    net = (await ble(sess, "network.status")).get("data", {})
    persist = ((cfg.get("network") or {}).get("sta_ssid") or "")
    cur = (net.get("connected_ssid") or "")
    (OUT / "000_ssid_precheck.json").write_text(json.dumps(
        {"target": ssid, "persistent_ssid": persist, "connected_ssid": cur},
        ensure_ascii=False, indent=1), encoding="utf-8")
    if ssid == persist:
        stop(f"目标 SSID 等于持久 SSID（{persist}）——拒绝运行")
        return False
    if ssid == cur:
        stop(f"目标 SSID 等于当前已连 SSID（{cur}）——拒绝运行")
        return False
    log(f"[gate] SSID 前置通过：目标={ssid} 持久={persist} 当前={cur}")
    return True


def tcp_gate():
    """门禁（v11.2）：改指向**之前**的 PC 侧 TCP 预检，失败即 STOP。
    限制：只证明 broker 从本机可达，不是目标 AP 的出网证明。"""
    tcp_ok, tcp_why = tcp_precheck(EMQX_HOST, EMQX_PORT)
    (OUT / "000_tcp_precheck.json").write_text(json.dumps(
        {"host": EMQX_HOST, "port": EMQX_PORT, "ok": tcp_ok, "detail": tcp_why,
         "limit": "PC 侧可达性，非目标 AP 出网证据"}, ensure_ascii=False, indent=1),
        encoding="utf-8")
    log(f"[gate] TCP 预检 {EMQX_HOST}:{EMQX_PORT} ok={tcp_ok}（{tcp_why}）")
    if not tcp_ok:
        stop("TCP 预检失败：broker 从本机不可达——不进入测试")
        return False
    return True


def serial_anchor_check():
    """静态文件证据：两份 phaseA 串口日志各恰 1 处 POWERON（偏移留证）。
    注意："双 console 持续在录"是外部门禁，由操作人员现场确认——本检查
    不等同于在录确认。"""
    ev = {}
    ok = True
    for port in ("ttyUSB0", "ttyUSB1"):
        f = SERIAL_DIR / f"serial_{port}.log"
        if not f.is_file():
            stop(f"串口日志缺失：{f}（双 console 在录是外部门禁）")
            return
        data = f.read_bytes()
        offs = [m.start() for m in re.finditer(rb"rst:0x1 \(POWERON_RESET\)", data)]
        ev[port] = {"size_bytes": len(data), "poweron_offsets": offs}
        if len(offs) != 1:
            ok = False
    (OUT / "000_gate_serial_anchor.json").write_text(
        json.dumps(ev, indent=1), encoding="utf-8")
    log(f"[gate] 串口锚点留证 {ev}（在录确认=外部门禁）")
    if not ok:
        stop("POWERON 锚点数异常（疑似新 boot）")


async def identity_gate(sess, tag, allow_abort=False, soft=False,
                        relax_ssid=False):
    """六维身份。
    allow_abort=True：走恢复通道命令（终态采集，STOP 后可用）。
    soft=True：不 stop()，把问题记入返回值的 problems（收尾采集用，避免
      把已通过的测试因瞬时状态翻成失败）。
    relax_ssid=True：运行态 SSID 允许持久集合（CMCC-502/TQ2026 回落）。"""
    async def one(cmd):
        if allow_abort:
            return await recover_cmd(sess, cmd)
        return await ble(sess, cmd)
    info = (await one("device.info")).get("data", {})
    st = (await one("device.status")).get("data", {})
    net = (await one("network.status")).get("data", {})
    cfg = (await one("config.get")).get("data", {})
    d = {
        "tag": tag, "adv": TARGET, "device_id": info.get("device_id"),
        "build_sha": info.get("build_sha"),
        "uptime_ms": st.get("uptime_ms"),
        "sta_ssid": net.get("connected_ssid"),
        "sta_connected": net.get("sta_connected"), "sta_ip": net.get("sta_ip"),
        "mqtt_host": (cfg.get("mqtt") or {}).get("mqtt_host"),
        "mqtt_port": (cfg.get("mqtt") or {}).get("mqtt_port"),
        "mqtt_connected": net.get("mqtt_connected"),
        "wall": time.strftime("%H:%M:%S"), "mono": round(time.monotonic(), 3),
    }
    problems = []
    if d["device_id"] != "IotHub-C8E4" or d["build_sha"] != "d4188551c":
        problems.append("身份不符")
    ssid_ok = (d["sta_ssid"] in ALLOWED_PERSIST_SSID) if relax_ssid \
        else (d["sta_ssid"] == SSID)
    if not ssid_ok or not d["sta_connected"]:
        problems.append(f"SSID/连接态不符（{d['sta_ssid']}/{d['sta_connected']}）")
    if not str(d["sta_ip"] or "").startswith("192.168.1."):
        problems.append("sta_ip 网段不符")
    if d["mqtt_connected"] is not True:
        problems.append(f"mqtt_connected={d['mqtt_connected']}（须在线）")
    d["problems"] = problems
    if problems:
        if soft:
            log(f"[{tag}] 收尾采集发现偏差（不阻断）：{'；'.join(problems)}")
        else:
            stop("；".join(problems))
    return d


async def cleanup_confirm(sess, budget_s=120.0, need_consecutive=2, tag=""):
    """真总预算 cleanup 窗口：同一 deadline 贯穿睡眠/token gate/请求前/请求后；
    剩余不足不发请求；窗口内请求 retries=0；连续 need_consecutive 次满足才
    确认。文件按 tag 分名不覆盖。"""
    deadline = time.monotonic() + budget_s
    polls = []
    streak = 0
    path = OUT / f"000_cleanup_window_{tag or 'x'}.json"
    path.write_text(json.dumps({"reason": tag, "budget_s": budget_s,
                                "polls": polls}, ensure_ascii=False, indent=1),
                    encoding="utf-8")

    def flush(result=None):
        path.write_text(json.dumps(
            {"reason": tag, "result": result, "polls": polls},
            ensure_ascii=False, indent=1), encoding="utf-8")

    while True:
        remain = deadline - time.monotonic()
        if remain <= 0:
            flush("TIMEOUT")
            return False
        await asyncio.sleep(min(10.0, remain / 2))   # 无超预算下限
        remain = deadline - time.monotonic()
        if remain <= 2.0:                      # 剩余不足以完成一次完整请求
            flush("TIMEOUT")
            return False
        try:                                    # token gate（含登录）受同一剩余预算封顶
            if not await asyncio.wait_for(_token_gate(sess), timeout=remain):
                flush("TOKEN_FAIL")
                return False
        except (asyncio.TimeoutError, TimeoutError):
            flush("TIMEOUT")
            return False
        remain = deadline - time.monotonic()
        if remain <= 2.0:
            flush("TIMEOUT")
            return False
        try:                                    # 整个 BLE 调用（写+等响应）受同一剩余预算封顶
            ns = await asyncio.wait_for(
                ble(sess, "network.status", timeout=min(15.0, remain - 1),
                    retries=0),
                timeout=remain)
        except (asyncio.TimeoutError, TimeoutError):
            flush("TIMEOUT")
            return False
        except Exception as exc:  # noqa: BLE001   # ConnectionLost 等：补记失败原因再抛
            flush(f"ERROR:{type(exc).__name__}:{str(exc)[:120]}")
            raise
        remain = deadline - time.monotonic()   # 请求返回后复查
        if remain <= 0:
            flush("TIMEOUT")
            return False
        dd = (ns or {}).get("data") or {}
        poll = {"t": time.strftime("%H:%M:%S"), "mono": round(time.monotonic(), 3),
                "sta": dd.get("connected_ssid"),
                "connected": dd.get("sta_connected"),
                "trial": ((dd.get("trial") or {}).get("state"))}
        polls.append(poll)
        flush(None)
        good = (poll["connected"] is True and poll["sta"] in ALLOWED_PERSIST_SSID
                and poll["trial"] == "idle")
        streak = streak + 1 if good else 0
        if streak >= need_consecutive:
            flush("CONFIRMED")
            log(f"[cleanup:{tag}] 确认（连续{streak}次满足，STA={poll['sta']}）")
            return True


_extra_clients = []       # 恢复阶段新建的 BleakClient（收尾统一关闭）


async def ensure_session(tries=3):
    """返回可用 BLE 会话：原会话可用则直接用；断链则重扫+重连（≤tries）。
    v10：把第二轮的人工恢复路径固化——恢复不再受原链路存活限制。"""
    global _session, _token_origin
    if _session is not None and _session.is_connected():
        return _session
    for attempt in range(1, tries + 1):
        try:
            recs = await scan_filtered(8.0)
            hit = [r for r in recs if r["address"].lower() == TARGET.lower()]
            if not hit:
                log(f"[reconnect] attempt{attempt}: 设备不在广播")
                await asyncio.sleep(3)
                continue
            cli = BleakClient(TARGET, timeout=25)
            await cli.connect()
            s2 = Session(cli, address=TARGET)
            await s2._connect_once()
            _token_origin = time.monotonic()
            _session = s2
            _extra_clients.append(cli)
            log(f"[reconnect] attempt{attempt}: 已重连并登录（token 起点重置）")
            return s2
        except Exception as exc:  # noqa: BLE001
            log(f"[reconnect] attempt{attempt} 失败 {type(exc).__name__}: "
                f"{str(exc)[:100]}")
            await asyncio.sleep(3)
    return None


async def cleanup_confirm_mqtt(rpc, budget_s=120.0, need_consecutive=2):
    """cleanup 完成窗口（MQTT 轮询）：硬 deadline，连续两次满足 STA+trial，
    逐轮落盘；传输失败记 POLL_TRANSPORT_FAILED 并返回 None（区别于未达成）。"""
    deadline = time.monotonic() + budget_s
    polls, streak = [], 0
    path = OUT / "000_cleanup_window.json"
    def flush(result):
        path.write_text(json.dumps({"result": result, "polls": polls},
                                   ensure_ascii=False, indent=1), encoding="utf-8")
    flush("RUNNING")
    while True:
        remain = deadline - time.monotonic()
        if remain <= 3.0:
            flush("CLEANUP_UNCONFIRMED")
            return False
        await asyncio.sleep(min(10.0, max(1.0, remain / 3)))
        kind, ns = await rpc.status()
        if kind != "OK":
            flush(f"POLL_TRANSPORT_FAILED:{ns}")
            log(f"[cleanup] POLL_TRANSPORT_FAILED（{ns}）——不判为 cleanup 失败")
            return None
        d = ns or {}
        poll = {"t": time.strftime("%H:%M:%S"), "mono": round(time.monotonic(), 3),
                "sta": d.get("connected_ssid"),
                "connected": d.get("sta_connected"),
                "trial": ((d.get("trial") or {}).get("state"))}
        polls.append(poll)
        flush("RUNNING")
        good = (poll["connected"] is True
                and poll["sta"] in ALLOWED_PERSIST_SSID
                and poll["trial"] == "idle")
        streak = streak + 1 if good else 0
        if streak >= need_consecutive:
            flush("CONFIRMED")
            log(f"[cleanup] 确认（连续{streak}次，STA={poll['sta']}）")
            return True


def tcp_precheck(host, port, timeout=5.0):
    """PC 侧 TCP 可达性预检（改指向之前）。**限制**：只证明 broker 从 PC 可达，
    不证明目标 AP 的出网条件；设备侧真实证据是它能否在 EMQX 上出现。"""
    import socket
    t0 = time.monotonic()
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return (True, f"ok {time.monotonic()-t0:.2f}s")
    except Exception as exc:  # noqa: BLE001
        return (False, f"{type(exc).__name__}: {exc}")


def expected_changed_keys(current, target):
    """写回前根据当前值算出"应该变更"的键（host 未变则只期望 port）。"""
    exp = []
    for k, tgt in zip(("mqtt_host", "mqtt_port"), target):
        cur = current.get(k) if isinstance(current, dict) else None
        if cur != tgt:
            exp.append(k)
    return exp


def r2_classify(resp, keys=("mqtt_host", "mqtt_port")):
    """R2 精确分类（v11.2）。返回 (state, detail)，state ∈
    {"OK", "REJECTED", "UNCONFIRMED"}：
      REJECTED    = 设备**明确** ok:false（唯一可称"设备拒绝写回"的情形）
      UNCONFIRMED = ok 缺失/非布尔、data/changed 结构异常、缺目标键
    另：changed 必须是**字符串数组**，否则不参与成员判断。"""
    if not isinstance(resp, dict):
        return ("UNCONFIRMED", {"why": f"response_not_dict:{type(resp).__name__}"})
    ok = resp.get("ok")
    if ok is False:
        return ("REJECTED", {"why": "ok_false"})
    if ok is not True:
        return ("UNCONFIRMED", {"why": f"ok_missing_or_nonbool:{ok!r}"})
    d = resp.get("data")
    if not isinstance(d, dict):
        return ("UNCONFIRMED", {"why": f"data_not_dict:{type(d).__name__}"})
    changed = d.get("changed")
    if not isinstance(changed, list) or \
            not all(isinstance(x, str) for x in changed):
        return ("UNCONFIRMED",
                {"why": f"changed_not_str_list:{type(changed).__name__}"})
    missing = [k for k in keys if k not in changed]
    det = {"changed": changed, "expected": list(keys), "missing": missing}
    if missing:
        # 允许 changed 多于预期（设备可能重写同值键），但不允许少于预期
        return ("UNCONFIRMED", {"why": "missing_keys", **det})
    return ("OK", det)


def r2_write_ok(resp, keys=("mqtt_host", "mqtt_port")):
    """兼容包装：True 仅当分类为 OK。"""
    state, det = r2_classify(resp, keys)
    return (state == "OK", {"state": state, **det})


async def r4_silence_check(pres, window_s=30.0):
    """R4（v11.2）：断链基线与就绪判定在同一窗口内。
    开始先记 disconnect_count → wait_ready → 期间有断链即失败 →
    开始时校验连接有效 → 观察满 window_s → 结束时再次校验连接/代次/计数。
    只有"当前连接持续有效 + 订阅本代生效 + 窗口内 0 设备消息"才通过。"""
    dc_start = pres.disconnect_count
    gen_start = pres.generation
    ready, why = await pres.wait_ready(12.0)
    if not ready:
        return (False, {"stage": "LISTENER_NOT_READY", "why": why})
    if pres.disconnect_count != dc_start:
        return (False, {"stage": "LISTENER_RESET_DURING_INIT",
                        "disconnects": pres.disconnect_count - dc_start})
    # v11.3：允许"首次 CONNACK"把代次 0→1（异步首连不是重连）；
    #        若进入时已有代次（gen_start>0），初始化期间代次变化即为重连 → 判负。
    gen_ok = (pres.generation == gen_start) or \
             (gen_start == 0 and pres.generation == 1)
    if not gen_ok:
        return (False, {"stage": "LISTENER_RESET_DURING_INIT",
                        "generation_delta": pres.generation - gen_start})
    if not pres.is_ready():
        return (False, {"stage": "LISTENER_NOT_READY_AT_START",
                        "ready": False})
    t0 = time.monotonic()
    gen0 = pres.generation
    await asyncio.sleep(window_s)
    dc = pres.disconnect_count - dc_start
    gen = pres.generation - gen_start
    alive = pres.is_alive()
    recent = [(round(t - t0, 1), topic) for t, topic, _k in pres.seen if t >= t0]
    detail = {"window_s": window_s, "msgs_in_window": len(recent),
              "sample": recent[:3], "disconnects_total": dc,
              "generation_delta": gen, "alive_at_end": alive,
              "evidence": pres.evidence_class()}
    if dc > 0:
        return (False, {"stage": "LISTENER_DISCONNECTED", **detail})
    if pres.generation != gen0:
        return (False, {"stage": "LISTENER_GENERATION_CHANGED", **detail})
    if not alive:
        return (False, {"stage": "LISTENER_LOST_AT_END", **detail})
    if not pres.is_ready():
        return (False, {"stage": "LISTENER_NOT_READY_AT_END", **detail})
    if recent:
        return (False, {"stage": "STILL_ON_EMQX", **detail})
    return (True, detail)


def stable_sample(d):
    """单次采样是否满足稳定条件（网络表面）。"""
    d = d or {}
    return (d.get("sta_connected") is True
            and d.get("internet_verified") is True
            and d.get("last_disconnect") == "none"
            and ((d.get("trial") or {}).get("state")) == "idle")


async def stability_gate(status_fn, budget_s=120.0, interval_s=10.0, need=2):
    """网络稳定性门禁（**非**资源释放确认）。
    返回 ("OK"|"TIMEOUT"|"TRANSPORT", detail, polls)。
    注意：两次离散采样无法检测采样间隔内发生的断连—重连（last_disconnect
    在 STA 重连成功时被重置为 none），因此本门禁只证明"采样时刻稳定"。"""
    deadline = time.monotonic() + budget_s
    streak = 0
    polls = []
    while time.monotonic() < deadline:
        remain = deadline - time.monotonic()
        await asyncio.sleep(min(interval_s, max(0.5, remain)))
        kind, ns = await status_fn()
        if kind != "OK":
            return ("TRANSPORT", {"why": str(ns)}, polls)
        polls.append({"t": time.strftime("%H:%M:%S"),
                      "mono": round(time.monotonic(), 3),
                      "sta": ns.get("connected_ssid"),
                      "connected": ns.get("sta_connected"),
                      "verified": ns.get("internet_verified"),
                      "last_disconnect": ns.get("last_disconnect"),
                      "trial": ((ns.get("trial") or {}).get("state"))})
        streak = streak + 1 if stable_sample(ns) else 0
        if streak >= need:
            return ("OK", {"streak": streak}, polls)
    return ("TIMEOUT", {"streak": streak}, polls)


SERIAL_CONSOLE = "ttyUSB1"      # C8E4 console（第一轮实证映射）


def serial_mark():
    """记录 console 日志当前长度，用于事后只读取"op 创建之后"的日志段。"""
    try:
        f = SERIAL_DIR / f"serial_{SERIAL_CONSOLE}.log"
        return (f, f.stat().st_size)
    except OSError:
        return (None, 0)


def serial_hits_since(mark, patterns):
    """只读：截取 mark 之后的日志，返回含任一模样的行（诊断用，不触发任何动作）。"""
    f, off = mark
    if f is None:
        return []
    try:
        with open(f, "rb") as fh:
            fh.seek(off)
            data = fh.read().decode("utf-8", "replace")
    except OSError:
        return []
    pats = [x.lower() for x in patterns]
    return [ln.strip()[:160] for ln in data.splitlines()
            if any(p in ln.lower() for p in pats)]


async def do_recovery(sess=None):
    """分阶段恢复（评审要求 R1–R5）：只有全部确认才 RECOVERY_CONFIRMED。
    R1 会话获取（可否重连）/ R2 写回并确认响应 / R3 独立回读 /
    R4 broker 侧确认（EMQX 静默）/ R5 设备侧 mqtt_connected 二次确认。
    任何一项无法确认 → RECOVERY_UNCONFIRMED（区别于写入被拒 RECOVERY_FAILED）。"""
    global _recovery_state
    if _repoint_state == RP_NA:
        _recovery_state = RC_OK
        log("[recover] 未改指向，无需恢复")
        return
    stages = {}
    def finish(state, why):
        global _recovery_state
        _recovery_state = state
        (OUT / "000_recovery_stages.json").write_text(json.dumps(
            {"stages": stages, "final": state, "reason": why},
            ensure_ascii=False, indent=1), encoding="utf-8")
        log(f"[recover] {state}：{why}")
    try:
        # R1 会话获取
        s = await ensure_session()
        if s is None:
            stages["R1_session"] = "FAIL"
            finish(RC_UNCONF, "BLE 重连 3 次失败")
            return
        stages["R1_session"] = "OK"
        if not await _token_gate(s):
            stages["R1_session"] = "TOKEN_FAIL"
            finish(RC_UNCONF, "token 续签失败")
            return
        # R2 写回并确认响应：先读当前值，按"实际需要变更的键"判定
        cur_cfg = await recover_cmd(s, "config.get")
        cur_mq = ((cur_cfg or {}).get("data") or {}).get("mqtt") or {}
        exp_keys = expected_changed_keys(cur_mq, _origin_snapshot)
        stages["R2_expected_keys"] = json.dumps(exp_keys)
        r = await recover_cmd(s, "config.set",
                              {"patch": {"mqtt_host": _origin_snapshot[0],
                                         "mqtt_port": _origin_snapshot[1]}})
        w_state, w_det = r2_classify(r, tuple(exp_keys))
        stages["R2_write"] = json.dumps({"state": w_state, **w_det},
                                       ensure_ascii=False)
        if w_state == "REJECTED":
            finish(RC_FAIL, "设备明确拒绝写回（ok:false）")
            return
        if w_state == "UNCONFIRMED":
            finish(RC_UNCONF, f"写回结果无法确认（{w_det.get('why')}）")
            return
        # R3 独立回读
        back = await recover_cmd(s, "config.get")
        mq = ((back or {}).get("data") or {}).get("mqtt") or {}
        readback = (mq.get("mqtt_host"), mq.get("mqtt_port"))
        stages["R3_readback"] = f"{readback}"
        if readback != _origin_snapshot:
            finish(RC_UNCONF, f"回读不符 {readback} != {_origin_snapshot}")
            return
        # R4 broker 侧：监听就绪后完整静默 30s（未就绪/断链/有消息都不通过）
        pres = Presence(EMQX_HOST, EMQX_PORT)
        try:
            ok4, det4 = await r4_silence_check(pres, 30.0)
            stages["R4_emqx_silence"] = json.dumps(det4, ensure_ascii=False)
            if not ok4:
                finish(RC_UNCONF, f"R4 未通过（{det4.get('stage')}）")
                return
        finally:
            pres.stop()
        # R5 设备侧二次确认（与 R3 间隔 ≥5s）
        await asyncio.sleep(5)
        ns = await recover_cmd(s, "network.status")
        conn = (ns.get("data") or {}).get("mqtt_connected")
        stages["R5_mqtt_connected"] = str(conn)
        if conn is not True:
            finish(RC_UNCONF, f"设备侧 mqtt_connected={conn}")
            return
        finish(RC_OK, "R1–R5 全部通过")
    except Exception as exc:  # noqa: BLE001
        stages["exception"] = f"{type(exc).__name__}: {str(exc)[:120]}"
        finish(RC_UNCONF, f"恢复过程异常 {type(exc).__name__}")


async def run_round(sess, pw):
    """v11：门禁 → EMQX ACL 预检 → 改指向 → 在场 → MQTT 轮询（op-E/cleanup/op-V）。"""
    global _repoint_state, _origin_snapshot
    serial_anchor_check()
    if _abort:                      # 门禁失败（如串口日志缺失）立即返回，不进入身份采集
        return
    g1 = await identity_gate(sess, "S0-first")
    await asyncio.sleep(20)
    g2 = await identity_gate(sess, "S0-second")
    o1 = (g1["mqtt_host"], g1["mqtt_port"])
    o2 = (g2["mqtt_host"], g2["mqtt_port"])
    if o1 != o2 or o1 != (CLOUD_HOST, CLOUD_PORT):
        stop(f"MQTT 原值核验失败 first={o1} second={o2} 授权={CLOUD_HOST, CLOUD_PORT}")
        _results["op_e"] = _results["op_v"] = "NOT_RUN"
        return
    _origin_snapshot = o1
    log(f"[gate] MQTT 原值核验通过，恢复快照={_origin_snapshot}")
    u1, u2 = g1["uptime_ms"], g2["uptime_ms"]
    if not isinstance(u1, (int, float)) or not isinstance(u2, (int, float)) \
            or not (math.isfinite(u1) and math.isfinite(u2)) \
            or u1 <= 0 or u2 < u1:
        stop(f"uptime 缺失/非有限数/回退 u1={u1} u2={u2}")
        return
    du, dw = (u2 - u1) / 1000.0, g2["mono"] - g1["mono"]
    boot_wall = time.strftime("%H:%M:%S", time.localtime(time.time() - u1 / 1000.0))
    log(f"[gate] uptime 连续性 Δu={du:.1f}s Δwall={dw:.1f}s boot≈{boot_wall}")
    if abs(du - dw) > 3:
        stop("uptime 不连续")
        return
    t = time.strptime(boot_wall, "%H:%M:%S")
    if not (time.strptime("19:44:00", "%H:%M:%S") <= t <= time.strptime("19:46:00", "%H:%M:%S")):
        stop(f"boot {boot_wall} 偏离稳定窗口锚点")
        return
    if _abort:
        return
    # ---- SSID 前置（评审裁决 §5）：目标 ≠ 持久 ≠ 当前 ----
    if not await ssid_precheck(sess, _ssid):
        _results["op_e"] = _results["op_v"] = "NOT_RUN"
        return
    # ---- TCP 门禁（v11.2 显式调用；必须在改指向之前）----
    if not tcp_gate():
        _results["op_e"] = _results["op_v"] = "NOT_RUN"
        return
    log("[gate] 通过（本轮：MQTT async expired[须 GOT_IP] + op-V；轮询走 MQTT）")

    # ---- EMQX ACL/通路预检（改指向之前；此时设备仍在 TB，不会被误触发）----
    rpc = Rpc(EMQX_HOST, EMQX_PORT)
    try:
        rpc.start(asyncio.get_running_loop())
    except Exception as exc:  # noqa: BLE001
        stop(f"无法连接 EMQX {EMQX_HOST}:{EMQX_PORT} —— {type(exc).__name__}")
        _results["op_e"] = _results["op_v"] = "NOT_RUN"
        return
    ready, why = await rpc.wait_ready(12.0)
    if not ready:
        rpc.stop()
        stop(f"EMQX RPC 通道未就绪（{why}）——不进入测试")
        _results["op_e"] = _results["op_v"] = "NOT_RUN"
        return
    try:
        pres = Presence(EMQX_HOST, EMQX_PORT)
    except Exception as exc:  # noqa: BLE001
        rpc.stop()
        stop(f"无法建立 EMQX 在场监听 —— {type(exc).__name__}")
        _results["op_e"] = _results["op_v"] = "NOT_RUN"
        return
    pres_ready, pres_why = await pres.wait_ready(12.0)
    if not pres_ready:
        rpc.stop()
        pres.stop()
        stop(f"EMQX 在场监听未就绪（{pres_why}）——不进入测试")
        _results["op_e"] = _results["op_v"] = "NOT_RUN"
        return
    try:
        sub_ok, pub_ok, acl_ev = rpc.acl_preflight()
        (OUT / "000_acl_preflight.json").write_text(json.dumps(
            {"subscribe_ok": sub_ok, "publish_ok": pub_ok, **acl_ev},
            ensure_ascii=False, indent=1), encoding="utf-8")
        log(f"[gate] EMQX ACL 预检 sub={sub_ok} pub={pub_ok} client_id={rpc.client_id}")
        if not (sub_ok and pub_ok):
            stop("EMQX ACL/通路预检失败（设备主题权限未证实）——不进入测试")
            _results["op_e"] = _results["op_v"] = "NOT_RUN"
            return

        # ---- 改指向 EMQX ----
        repoint_mono = time.monotonic()
        _repoint_state = RP_ATT
        r = await ble(sess, "config.set",
                      {"patch": {"mqtt_host": EMQX_HOST, "mqtt_port": EMQX_PORT}})
        conf = False
        if (r or {}).get("ok") is True:
            back = await ble(sess, "config.get")
            mq = ((back or {}).get("data") or {}).get("mqtt") or {}
            if (mq.get("mqtt_host"), mq.get("mqtt_port")) == (EMQX_HOST, EMQX_PORT):
                conf = True
        _repoint_state = RP_CONF if conf else RP_ATT
        if not conf:
            stop("改指向未获成功回包及配置回读双重确认——不进入 op-E")
            _results["op_e"] = _results["op_v"] = "NOT_RUN"
            return
        log(f"[S4] 改指向已确认 → {EMQX_HOST}:{EMQX_PORT}")

        if not await pres.wait(240.0, since=repoint_mono):
            stop("设备未在 240s 内接入 EMQX（broker 侧无遥测）")
            _results["op_e"] = _results["op_v"] = "NOT_RUN"
            return
        log(f"[S4] broker 侧确认设备接入（首条 {pres.seen[-1][1]}；"
            f"证据={pres.evidence_class()}）")

        # ---- v11.3.5 网络稳定性门禁（**非**资源释放确认）----
        # 源码核实（dc254d5）：wifi.h 无 owner getter，network.status/device.status
        # 均不暴露 owner / cleanup 阶段 → 协议层无法观测 CLEANUP 是否释放。
        # 另：last_disconnect 在 STA 重连成功时被重置为 none，故两次离散采样
        # 无法检测采样间隔内的断连—重连；本门禁只证明"采样时刻稳定"。
        g_kind, g_det, g_polls = await stability_gate(rpc.status)
        (OUT / "000_stability_gate.json").write_text(json.dumps(
            {"result": g_kind, "detail": g_det, "polls": g_polls,
             "proves": "至少两次间隔约10秒的状态采样均满足网络稳定条件",
             "does_not_prove": ["采样间隔内没有发生短暂断连",
                                "Wi-Fi owner 已释放",
                                "CLEANUP 已结束"],
             "note": "owner/cleanup 在协议层不可观测；不得据本门禁推断资源已释放"},
            ensure_ascii=False, indent=1), encoding="utf-8")
        if g_kind == "TRANSPORT":
            stop(f"稳定性门禁轮询传输失败（{g_det.get('why')}）——不创建 op-E")
            _results["op_e"] = _results["op_v"] = "NOT_RUN"
            return
        if g_kind != "OK":
            stop("改指向后 120s 内未取得连续稳定采样——不创建 op-E")
            _results["op_e"] = _results["op_v"] = "NOT_RUN"
            return
        log("[S4] 稳定性门禁通过（连续两次采样；不证明 owner 已释放）")

        # ---- op-E：MQTT 创建 + MQTT 轮询 ----
        op_mark = serial_mark()            # 只读诊断基线（op 创建时刻）
        kind, resp = await rpc.call("network.tryconnect",
                                    {"ssid": _ssid, "password": pw})
        if kind != "OK":
            stop(f"POLL_TRANSPORT_FAILED: 创建 op-E 传输失败（{resp}）")
            _results["op_e"] = "INCONCLUSIVE_TRANSPORT"
            _results["op_v"] = "NOT_RUN"
            return
        opE = ((resp.get("data") or {}) or {}).get("operation_id")
        if not opE:
            stop("op-E 受理无 operation_id（设备语义失败）")
            _results["op_e"] = "FAILED"
            _results["op_v"] = "NOT_RUN"
            return
        _op_registry.add(opE)
        log(f"[S4] op-E 受理 {opE}（MQTT 源）")

        # ---- 轮询（MQTT）：accepted → GOT_IP → 自然 expired ----
        tG = time.monotonic()
        seen_got_ip, terminal, got_ip_mono = False, None, None
        while time.monotonic() < tG + 600 and not _abort:
            await asyncio.sleep(10)
            kind, st = await rpc.op_state(opE)
            if kind != "OK":
                stop(f"POLL_TRANSPORT_FAILED: operation.get 传输失败（{st}）")
                _results["op_e"] = "INCONCLUSIVE_TRANSPORT"
                return
            cur = st.get("state")
            kind2, ns = await rpc.status()
            trial = ((ns or {}).get("trial") or {}).get("state") if kind2 == "OK" else "?"
            log(f"[S4] +{time.monotonic()-tG:.0f}s op={cur} trial={trial}"
                f"{'' if kind2 == 'OK' else ' (status TRANSPORT)'}")
            if cur == "got_ip":
                if not seen_got_ip:
                    seen_got_ip = True
                    got_ip_mono = time.monotonic()
                    log("[S4] GOT_IP 达成（后续 expired 才计目标分支）")
                if trial == "idle":
                    log("[S4] 观察到 trial idle、operation 未发布终态；清理原因未判定")
            if cur in ("expired", "cancelled", "failed"):
                terminal = cur
                break
        if _abort:
            return
        if terminal is None:
            stop("op-E 轮询 600s 无终态（unknown）——不重试")
            _results["op_e"] = "FAILED"
            return
        if terminal in ("cancelled", "failed"):
            err = (st.get("error") if isinstance(st, dict) else None)
            # 纯诊断（不触发重试）：若 console 出现 owner 竞争文本，仅记录分类
            if err == "connect_failed":
                hits = serial_hits_since(op_mark, ("owner wait timeout",
                                                   "tryconnect busy",
                                                   "owner=CLEANUP"))
                if hits:
                    _results["diagnosis"] = "SUSPECTED_OWNER_CONTENTION"
                    (OUT / "000_diagnosis.json").write_text(json.dumps(
                        {"diagnosis": "SUSPECTED_OWNER_CONTENTION",
                         "retry": False,
                         "note": "仅诊断分类；按裁决不自动重试",
                         "evidence": hits[:5]}, ensure_ascii=False, indent=1),
                        encoding="utf-8")
                    log(f"[S4] 诊断：{_results['diagnosis']}（仅记录，不重试）")
            stop(f"op-E 异常终态 {terminal}/{err}——立即 STOP，不重试")
            _results["op_e"] = "FAILED"
            return
        if not seen_got_ip:
            stop("op-E expired 但未达 GOT_IP（ACCEPTED_TTL_EXPIRED；按裁决不重试）")
            _results["op_e"] = "ACCEPTED_TTL_EXPIRED"
            return
        _results["op_e"] = "PASS"
        log(f"[S4] op-E GOT_IP→expired（PASS；GOT_IP 至过期 "
            f"{time.monotonic()-got_ip_mono:.0f}s）")

        # ---- cleanup 确认（MQTT 轮询，硬 deadline 120s，连续两次）----
        cl = await cleanup_confirm_mqtt(rpc, 120.0)
        if cl is None:
            stop("POLL_TRANSPORT_FAILED: cleanup 窗口传输层失败")
            _results["op_v"] = "INCONCLUSIVE_TRANSPORT"
            return
        if cl is False:
            stop("cleanup 窗口 120s 未确认（STA/trial）——CLEANUP_UNCONFIRMED")
            _results["op_v"] = "SKIPPED"
            return

        # ---- op-V（MQTT）----
        sc, frags = await ble_scan_full(sess)   # 扫描仅 BLE 提供
        cond, reasons = scan_five_conditions(sc, frags)
        if not cond:
            log(f"[S4] 首扫未满足五条件 {reasons}，重扫一次")
            sc, frags = await ble_scan_full(sess)
            cond, reasons = scan_five_conditions(sc, frags)
        if not cond:
            _results["op_v"] = "SKIPPED"
            log(f"[S4] 降级：{reasons}——op-V 放弃（整轮非成功退出）")
            return
        kind, resp = await rpc.call("network.tryconnect", {"ssid": PROBE})
        if kind != "OK":
            stop(f"POLL_TRANSPORT_FAILED: 创建 op-V 传输失败（{resp}）")
            _results["op_v"] = "INCONCLUSIVE_TRANSPORT"
            return
        opV = ((resp.get("data") or {}) or {}).get("operation_id")
        if not opV or opV == opE:
            stop(f"op-V 受理异常 id={opV}（job 未清或拒绝）")
            _results["op_v"] = "FAILED"
            return
        _op_registry.add(opV)
        log(f"[S4] op-V 受理新 id={opV}（≠op-E，job 已清确证）")
        opv_final, opv_err = None, None
        for _ in range(14):
            await asyncio.sleep(5)
            kind, st = await rpc.op_state(opV)
            if kind != "OK":
                stop(f"POLL_TRANSPORT_FAILED: op-V 轮询传输失败（{st}）")
                _results["op_v"] = "INCONCLUSIVE_TRANSPORT"
                return
            opv_final = st.get("state")
            opv_err = st.get("error")
            if opv_final in ("failed", "expired", "cancelled"):
                break
        if opv_final == "failed" and opv_err == "ap_not_found":
            _results["op_v"] = "PASS"
            log("[S4] op-V PASS：failed/ap_not_found（run 内 clear_job_sync 路径）")
        else:
            _results["op_v"] = "FAILED"
            stop(f"op-V 终态断言失败 {opv_final}/{opv_err}")
    finally:
        rpc.stop()
        pres.stop()


class _Tee:
    def __init__(self, *streams):
        self.streams = streams

    def write(self, data):
        for s in self.streams:
            try:
                s.write(data)
            except Exception:  # noqa: BLE001
                pass
        return len(data)

    def flush(self):
        for s in self.streams:
            try:
                s.flush()
            except Exception:  # noqa: BLE001
                pass


async def main():
    global _log, _token_origin, _session
    # P0-1：目录新鲜度检查在 mkdir/log 之前（any(iterdir)，非空拒绝）
    if OUT.exists() and any(OUT.iterdir()):
        print("[fatal] 证据目录非空——禁止静默复用，请归档后更换目录", flush=True)
        return 2
    OUT.mkdir(parents=True, exist_ok=True)
    _log = open(OUT / "capture_log.txt", "a", encoding="utf-8")
    trace_f = open(OUT / "session_trace.log", "a", encoding="utf-8")
    prev_stdout = sys.stdout
    sys.stdout = _Tee(prev_stdout, trace_f)   # session_trace：脱敏截断 TX/RX 跟踪流
    log(f"=== S6 round4（v11.3.6 脚本/EMQX {EMQX_HOST}:{EMQX_PORT}）开始（窄授权：MQTT async expired[须 GOT_IP] + op-V）===")
    code = 3
    cli = None
    try:                                   # 启动与运行全部纳入统一收尾
        global _ssid
        _ssid = os.environ.get("IOTHUB_TEST_SSID", "").strip()
        if not _ssid:
            raise RuntimeError("未指定目标测试 SSID（环境变量 IOTHUB_TEST_SSID）——"
                               "评审裁决：必须显式指定且 ≠ 持久/当前 SSID")
        log(f"[cfg] 目标测试 SSID = {_ssid}（口令从 .local/test-env.md 读取，不落盘）")
        recs = await scan_filtered(10.0)
        hit = [r for r in recs if r["address"].lower() == TARGET.lower()]
        if not hit:
            log("[STOP] C8E4 不在广播中（无静默回退）")
            code = 2
        else:
            cli = BleakClient(TARGET, timeout=25)
            await cli.connect()
            sess = Session(cli, address=TARGET)
            _session = sess
            await sess._connect_once()          # S0 无条件登录 = 计时起点
            _token_origin = time.monotonic()
            log("[S0] 无条件 auth.login（token 计时起点）")
            pw = load_password(_ssid)          # 口令按目标 SSID 取（失败→外层 except）
            await run_round(sess, pw)
    except Exception as exc:  # noqa: BLE001
        log(f"[fatal] {type(exc).__name__}: {str(exc)[:160]}")
        code = 3
    finally:
        # 统一收尾：恢复（必要时重连）→ 终态身份 → end_state（全路径必有）
        # → 会话关闭 → 日志关闭 → SHA256SUMS（无本地 broker 需处置）
        gEnd = None                    # 无会话路径（未广播/连接/口令失败）也为 None，不 UnboundLocalError
        if _repoint_state != RP_NA:
            try:
                await do_recovery()    # v10：内部保证 ensure_session（断链可重连）
            except Exception as exc:  # noqa: BLE001
                log(f"[recover] 兜底异常 {type(exc).__name__}: {str(exc)[:120]}")
        if _session is not None:
            try:
                gEnd = await identity_gate(_session, "end", allow_abort=True,
                                           soft=True, relax_ssid=True)
            except Exception as exc:  # noqa: BLE001
                log(f"[end] 终态身份采集失败 {type(exc).__name__}: {str(exc)[:120]}")
            try:
                closer = getattr(_session, "close", None)
                if closer is not None:
                    await closer()
                elif cli is not None:
                    await cli.disconnect()
            except Exception:  # noqa: BLE001
                pass
        for extra in _extra_clients:       # 恢复阶段新建的会话
            try:
                await extra.disconnect()
            except Exception:  # noqa: BLE001
                pass
        tests_pass = (_results.get("op_e") == "PASS"
                      and _results.get("op_v") == "PASS" and _abort is None)
        device_ok = (_repoint_state == RP_NA) or (_recovery_state == RC_OK)
        # device_ok=False 可能是 RECOVERY_FAILED / RECOVERY_UNCONFIRMED——两者都非 0
        (OUT / "000_end_state.json").write_text(json.dumps({
            "results": _results, "tests_pass": tests_pass, "device_ok": device_ok,
            "abort": _abort, "recovery": _recovery_state,
            "repoint_state": _repoint_state, "gate": gEnd,
        }, ensure_ascii=False, indent=1), encoding="utf-8")
        code = 0 if (tests_pass and device_ok) else (code if code else 3)
        log(f"[result] {_results} tests_pass={tests_pass} "
            f"device_ok={device_ok} → exit={code}")
        # 无本地 broker：无需进程处置；若未恢复，设备仍指向 EMQX（云端可观测，
        # 人工接管路径见报告——不会留下"本地服务已停、设备仍指向它"的坑）
        if _repoint_state != RP_NA and _recovery_state != RC_OK:
            log("[finally] 未恢复：设备可能仍指向 "
                f"{EMQX_HOST}:{EMQX_PORT}（EMQX 为常驻服务，不随本脚本停止）——"
                "需人工接管写回 TB 指向")
        # 结束日志全部落盘并关闭后，再生成 SHA256SUMS（哈希后不再改文件）
        log(f"[done] abort={_abort} repoint={_repoint_state} "
            f"recovery={_recovery_state} results={_results}")
        if _log is not None:
            _log.close()
        sys.stdout = prev_stdout
        trace_f.close()
        try:
            lines = []
            for f in sorted(OUT.iterdir()):
                if f.is_file() and f.name != "SHA256SUMS":
                    h = hashlib.sha256(f.read_bytes()).hexdigest()
                    lines.append(f"{h}  {f.name}")
            (OUT / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="utf-8")
            print(f"[evidence] SHA256SUMS 生成（{len(lines)} 文件）", flush=True)
        except Exception as exc:  # noqa: BLE001
            print(f"[evidence] SHA256SUMS 生成失败 {type(exc).__name__}——"
                  f"按非成功结果处理", flush=True)
            code = 3
    return code


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
