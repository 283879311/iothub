#!/usr/bin/env python3
"""S6 现场预采集执行脚本 v10（改用外部 EMQX，不自建 broker；窄授权：仅 MQTT async expired
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
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "ble_p0"))

from bleak import BleakClient  # noqa: E402

from mqtt_v1_r41 import MqttV1  # noqa: E402
from p0_acceptance import Session, scan_filtered, sanitize  # noqa: E402

OUT = Path("artifacts/cmd04_flash_20261114/s6_round3")
TARGET = "B0:A7:32:28:C8:E6"
EMQX_HOST, EMQX_PORT = "139.9.93.245", 1883   # 临时指向与 harness 接入点（外部 EMQX，匿名）
TB_HOST, TB_PORT = "139.9.93.245", 21883        # 恢复目标（1 号 ThingsBoard）
CLOUD_HOST, CLOUD_PORT = TB_HOST, TB_PORT          # 授权基线（守卫用）= 恢复目标
SSID = "CMCC-502"
PROBE = "__jobclear_probe__"
TOKEN_LIMIT_S = 480.0
ALLOWED_PERSIST_SSID = ("CMCC-502", "TQ2026")

RP_NA, RP_ATT, RP_CONF = "NOT_ATTEMPTED", "ATTEMPTED_OR_UNKNOWN", "REPOINT_CONFIRMED"
RC_OK, RC_FAIL = "RECOVERY_CONFIRMED", "RECOVERY_FAILED"

_log = None
_token_origin = None
_abort = None
_session = None
_repoint_state = RP_NA
_recovery_state = None
_origin_snapshot = None
_results = {"op_e": "NOT_RUN", "op_v": "NOT_RUN"}

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


def mqtt_req(m, cmd, args=None, wait=10.0):
    req = {"id": f"mq-{m._next_rid()}", "version": "1.0", "cmd": cmd,
           "args": args or {}}
    resp, _ = m.send(req, wait=wait)
    save(f"{cmd.replace('.', '_')}_mqtt", "mqtt", req,
         resp or {"_note": "no response"})
    return resp


class Presence:
    """broker 侧在场检测：订阅 v1/devices/me/#，任一条消息即证明设备已接入。"""

    def __init__(self, host, port):
        import paho.mqtt.client as mqtt
        self.seen = []
        self.cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                               protocol=mqtt.MQTTv311)
        self.cli.on_message = self._on_msg
        self.cli.connect(host, port, keepalive=20)
        self.cli.subscribe("v1/devices/me/#", 1)
        self.cli.loop_start()

    def _on_msg(self, _c, _u, msg):
        self.seen.append((time.monotonic(), msg.topic))

    def last_mono(self):
        return self.seen[-1][0] if self.seen else None

    async def wait(self, seconds=240.0, since=None):
        """等到 since（默认启动时刻）之后的首条消息；返回 True/False。"""
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


async def rpc_ready(m, tries=3):
    """接入后再做 RPC 探测，确认收/发通路可用（不当作在场判据）。"""
    for _ in range(tries):
        r = mqtt_req(m, "device.info", wait=6.0)
        if r is not None:
            return True
        await asyncio.sleep(3)
    return False


def load_password():
    txt = Path(".local/test-env.md").read_text(encoding="utf-8")
    m = re.search(r"CMCC-502\s+PASSWORD:(\S+)", txt)
    if not m:
        raise RuntimeError("test-env.md 无 CMCC-502 口令行")   # P1：由外层 except 记 fatal 并进 finally
    return m.group(1)


def serial_anchor_check():
    """静态文件证据：两份 phaseA 串口日志各恰 1 处 POWERON（偏移留证）。
    注意："双 console 持续在录"是外部门禁，由操作人员现场确认——本检查
    不等同于在录确认。"""
    ev = {}
    ok = True
    for port in ("ttyUSB0", "ttyUSB1"):
        f = OUT.parent / "phaseA" / f"serial_{port}.log"
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


async def do_recovery(sess=None):
    """专用恢复路径：除 NOT_ATTEMPTED 外都尝试；目标 = S0 实测核验快照。
    v10：会话断链时先 ensure_session() 重连（≤3 次）再写回。"""
    global _recovery_state
    if _repoint_state == RP_NA:
        _recovery_state = RC_OK
        log("[recover] 未改指向，无需恢复")
        return
    try:
        s = await ensure_session()
        if s is None:
            raise RuntimeError("BLE 重连 3 次失败")
        if not await _token_gate(s):
            raise RuntimeError("token renewal failed")
        await recover_cmd(s, "config.set",
                          {"patch": {"mqtt_host": _origin_snapshot[0],
                                     "mqtt_port": _origin_snapshot[1]}})
        ok = False
        for _ in range(10):
            await asyncio.sleep(8)
            ns = await recover_cmd(s, "network.status")
            if (ns.get("data") or {}).get("mqtt_connected") is True:
                ok = True
                break
        back = await recover_cmd(s, "config.get")
        mq = (back.get("data") or {}).get("mqtt") or {}
        readback = (mq.get("mqtt_host"), mq.get("mqtt_port"))
        ns2 = await recover_cmd(s, "network.status")   # 回读邻近重采
        ok2 = (ns2.get("data") or {}).get("mqtt_connected") is True
        if ok and ok2 and readback == _origin_snapshot:
            _recovery_state = RC_OK
            log(f"[recover] 成功：mqtt_connected=true(×2) 且回读={readback}")
        else:
            _recovery_state = RC_FAIL
            log(f"[recover] 失败：mqtt_connected={ok}/{ok2} 回读={readback}"
                f"（不宣称恢复成功；设备仍指向 EMQX {EMQX_HOST}:{EMQX_PORT}，"
                f"人工接管写回 TB）")
    except Exception as exc:  # noqa: BLE001
        _recovery_state = RC_FAIL
        log(f"[recover] 异常 {type(exc).__name__}: {str(exc)[:120]}——"
            f"BLE 重连/恢复失败；设备可能仍指向 EMQX，需人工接管")


async def run_round(sess, pw):
    global _repoint_state, _origin_snapshot
    serial_anchor_check()
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
    log("[gate] 通过（本轮：MQTT async expired[须 GOT_IP] + op-V）")

    # ---- 在场监听先起（broker 侧证据），再改指向 ----
    pres = None
    m = None
    try:
        pres = Presence(EMQX_HOST, EMQX_PORT)
    except Exception as exc:  # noqa: BLE001
        stop(f"无法连接 EMQX {EMQX_HOST}:{EMQX_PORT} —— {type(exc).__name__}")
        _results["op_e"] = _results["op_v"] = "NOT_RUN"
        return
    repoint_mono = time.monotonic()
    # ---- 改指向 EMQX：ATTEMPTED 前置 → 成功回包+回读双重确认，否则立即 STOP ----
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
        pres.stop()
        stop("改指向未获成功回包及配置回读双重确认——不进入 op-E")
        _results["op_e"] = _results["op_v"] = "NOT_RUN"
        return
    log(f"[S4] 改指向已确认 → {EMQX_HOST}:{EMQX_PORT}（state={_repoint_state}）")
    try:
        m = MqttV1(f"{EMQX_HOST}:{EMQX_PORT}")
        # 在场判据 = broker 侧收到设备任何 v1/devices/me/# 消息（240s 覆盖 6 级 backoff）
        if not await pres.wait(240.0, since=repoint_mono):
            stop("设备未在 240s 内接入 EMQX（broker 侧无遥测）")
            _results["op_e"] = _results["op_v"] = "NOT_RUN"
            return
        log(f"[S4] broker 侧确认设备接入（首条 {pres.seen[-1][1]}）")
        if not await rpc_ready(m):
            stop("设备已接入但 RPC 通路无响应")
            _results["op_e"] = _results["op_v"] = "NOT_RUN"
            return
        # ---- async expired（须先 GOT_IP；仅 ACCEPTED_TTL_EXPIRED 可重试一次）----
        target_hit, classification = False, None
        opE = None
        for attempt in (1, 2):
            r = mqtt_req(m, "network.tryconnect", {"ssid": SSID, "password": pw})
            opE = ((r or {}).get("data") or {}).get("operation_id")
            log(f"[S4] attempt{attempt} async 受理 op-E={opE}")
            if not opE:
                stop("无 operation_id")
                _results["op_e"] = "FAILED"
                return
            tG = time.monotonic()
            seen_got_ip, terminal = False, None
            while time.monotonic() < tG + 420 and not _abort:
                await asyncio.sleep(10)
                og = await ble(sess, "operation.get", {"operation_id": opE})
                cur = (og.get("data") or {}).get("state")
                ns = await ble(sess, "network.status")
                tb = ((ns.get("data") or {}).get("trial") or {}).get("state")
                log(f"[S4] +{time.monotonic()-tG:.0f}s op={cur} trial={tb}")
                if cur == "got_ip":
                    if not seen_got_ip:
                        seen_got_ip = True
                        log("[S4] GOT_IP 达成（后续 expired 才计目标分支）")
                    if tb == "idle":
                        log("[S4] 观察到 trial idle、operation 尚未发布终态；清理原因未判定")
                if cur == "expired":
                    terminal = "expired"
                    break
                if cur in ("cancelled", "failed"):
                    terminal = cur
                    break
            if _abort:
                _results["op_e"] = "FAILED"
                return
            if terminal is None:
                stop(f"attempt{attempt} 轮询 420s 无终态（unknown）——不重试")
                _results["op_e"] = "FAILED"
                return
            if terminal in ("cancelled", "failed"):
                stop(f"attempt{attempt} 异常终态 {terminal}——立即 STOP，不重试")
                _results["op_e"] = "FAILED"
                return
            if terminal == "expired" and seen_got_ip:
                target_hit = True
                _results["op_e"] = "PASS"
                log(f"[S4] attempt{attempt} = GOT_IP→expired（目标分支 PASS）")
                break
            classification = "ACCEPTED_TTL_EXPIRED（未达 GOT_IP，不计目标验收）"
            log(f"[S4] attempt{attempt} {classification}")
            if attempt == 1:
                log("[S4] 分类重试（至多一次）；先确认前一次 cleanup 已结束")
                if not await cleanup_confirm(sess, 120.0, tag="retry-pre"):
                    stop("重试前 cleanup 确认失败")
                    _results["op_e"] = "FAILED"
                    return
        if not target_hit:
            stop(f"op-E 未达成 GOT_IP→expired（分类={classification}，重试已用）")
            _results["op_e"] = "FAILED"
            return
        # ---- cleanup 完成窗口（真总预算 120s，未确认不进 op-V）----
        if not await cleanup_confirm(sess, 120.0, tag="post-expired"):
            stop("cleanup 完成窗口 120s 未确认（STA/trial 未恢复）")
            _results["op_v"] = "SKIPPED"
            return
        # ---- op-V job-clear 验证 ----
        sc, frags = await ble_scan_full(sess)
        cond, reasons = scan_five_conditions(sc, frags)
        if not cond:
            log(f"[S4] 首扫未满足五条件 {reasons}，重扫一次")
            sc, frags = await ble_scan_full(sess)
            cond, reasons = scan_five_conditions(sc, frags)
        if cond:
            r = mqtt_req(m, "network.tryconnect", {"ssid": PROBE}, wait=10)
            opV = ((r or {}).get("data") or {}).get("operation_id")
            if not opV or opV == opE:
                stop(f"op-V 受理异常 id={opV}（job 未清或拒绝）")
                _results["op_v"] = "FAILED"
                return
            log(f"[S4] op-V 受理新 id={opV}（≠op-E，job 已清确证）")
            opv_final, opv_err = None, None
            for _ in range(12):
                await asyncio.sleep(5)
                og = await ble(sess, "operation.get", {"operation_id": opV})
                opv_final = (og.get("data") or {}).get("state")
                opv_err = (og.get("data") or {}).get("error")
                if opv_final in ("failed", "expired", "cancelled"):
                    break
            if opv_final == "failed" and opv_err == "ap_not_found":
                _results["op_v"] = "PASS"
                log("[S4] op-V PASS：failed/ap_not_found（run 内 clear_job_sync 路径）")
            else:
                _results["op_v"] = "FAILED"
                stop(f"op-V 终态断言失败 {opv_final}/{opv_err}")
                return
        else:
            _results["op_v"] = "SKIPPED"
            log(f"[S4] 降级：{reasons}——op-V 放弃，该项记证据降级/未完成"
                f"（整轮非成功退出）")
    finally:
        if m is not None:
            try:
                m.cli.loop_stop()
            except Exception:  # noqa: BLE001
                pass
        if pres is not None:
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
    log(f"=== S6 round3（v10 脚本/EMQX {EMQX_HOST}:{EMQX_PORT}）开始（窄授权：MQTT async expired[须 GOT_IP] + op-V）===")
    code = 3
    cli = None
    try:                                   # 启动与运行全部纳入统一收尾
        pw = load_password()
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
