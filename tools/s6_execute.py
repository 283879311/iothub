#!/usr/bin/env python3
"""S6 现场预采集执行脚本 v4（v3 复审收口；窄授权：仅 MQTT async expired
（须先观察到 GOT_IP）+ op-V job-clear 验证；两项都 PASS 才退出码 0）。

v2→v3（评审 P0×2/P1×5/P2×2）：
P0-1 _session_holder NameError → 移除容器，改模块级 _session；
P0-2 改指向状态机 NOT_ATTEMPTED→ATTEMPTED_OR_UNKNOWN（发送前置位）→
     REPOINT_CONFIRMED（收到回包）→RECOVERY_CONFIRMED/FAILED——除
     NOT_ATTEMPTED 外任何状态退出都触发恢复兜底；
P1 恢复快照 = S0 实测并核验的设备 MQTT 配置（两次读取一致且等于授权云端
     基线，否则 STOP），恢复与回读比较均用该快照；
P1 op-E 必须先观察到 GOT_IP，expired 才计目标分支 PASS；无 GOT_IP 的
     expired 单列 ACCEPTED_TTL_EXPIRED（重试至多一次，两次均如此则 STOP）；
P1 expired 后增加 cleanup 完成窗口（≤120s：STA 回持久网络 + trial idle，
     轮询全留证），未确认不进 op-V；
P1 identity_gate/recover_cmd 全部纳入逐命令 token 守卫；
P1 恢复 POWERON 锚点检查（phaseA 串口）；uptime 缺失/回退 → STOP；
P2 capture_log=事件流，session_raw.log=BLE TX/RX 原始流（stdout 重定向），
     目录内生成 SHA256SUMS。
"""

import asyncio
import hashlib
import json
import math
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "ble_p0"))

from bleak import BleakClient  # noqa: E402

from mqtt_v1_r41 import MqttV1  # noqa: E402
from p0_acceptance import Session, scan_filtered, sanitize  # noqa: E402

OUT = Path("artifacts/cmd04_flash_20261114/s6_round2")
TARGET = "B0:A7:32:28:C8:E6"
PC_IP = "192.168.1.19"
CLOUD_HOST, CLOUD_PORT = "139.9.93.245", 21883   # 授权基线（守卫用）
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
_origin_snapshot = None          # S0 实测核验的 (host, port)
_results = {"op_e": None, "op_v": None}   # PASS/SKIPPED/FAILED/…
_broker = None

# Session 日志脱敏口径（评审 P2）：本 harness 的 TX/RX 记录为 JSON 脱敏截断流
# （token/password 字段掩码、单帧截断 800/1200 字符），**不是原始字节流**；
# 原始 GATT 帧落盘涉及明文凭据脱敏，列为后续增强，本轮证据口径如实标注。


def log(t):
    line = f"{time.strftime('%H:%M:%S')} {t}"
    print(line, flush=True)
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
    r = await sess.login(timeout=15)
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


async def prot(sess, coro_fn, *a, **kw):
    """普通采集入口：_abort 拦截 + 逐命令 token 守卫。"""
    if _abort:
        raise RuntimeError(f"aborted: {_abort}")
    if not await _token_gate(sess):
        raise RuntimeError("aborted: token renewal failed")
    return await coro_fn(*a, **kw)


async def gate_ble(sess, cmd, args=None, timeout=20):
    """门禁/终态入口：token 守卫 + （非 allow_abort 时）abort 拦截。"""
    if not await _token_gate(sess):
        raise RuntimeError("token renewal failed")
    req = {"id": f"s6-{sess.next_seq()}", "version": "1.0", "cmd": cmd,
           "args": args or {}, "token": sess.token}
    res = await sess.send_request(req, timeout=timeout,
                                  allow_relogin=False, allow_reconnect=False)
    save(f"{cmd.replace('.', '_')}_ble", "ble", req, res.obj)
    return res.obj


async def recover_cmd(sess, cmd, args=None, timeout=20):
    """恢复 allowlist：不受 _abort 阻断；逐命令 token 守卫；白名单命令。"""
    if cmd not in ("config.set", "config.get", "network.status", "device.status",
                   "device.info"):
        raise RuntimeError(f"recover_cmd 拒绝非恢复命令 {cmd}")
    if not await _token_gate(sess):
        raise RuntimeError("token renewal failed")
    req = {"id": f"rcv-{sess.next_seq()}", "version": "1.0", "cmd": cmd,
           "args": args or {}, "token": sess.token}
    res = await sess.send_request(req, timeout=timeout,
                                  allow_relogin=False, allow_reconnect=False)
    save(f"recover_{cmd.replace('.', '_')}", "ble", req, res.obj)
    return res.obj


async def ble_scan_full(sess):
    """经 prot() 的受保护扫描（token 守卫由调用方 prot 承担）。"""
    req = {"id": f"s6-{sess.next_seq()}", "version": "1.0", "cmd": "network.scan",
           "args": {}, "token": sess.token}
    res = await sess.send_request(req, timeout=30,
                                  allow_relogin=False, allow_reconnect=False)
    save("network_scan_ble", "ble", req, res.obj)
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


async def wait_broker(m, seconds=75.0):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        r, _ = m.send({"id": "probe", "version": "1.0", "cmd": "device.info",
                       "args": {}}, wait=3.0)
        if r is not None:
            return True
        await asyncio.sleep(5)
    return False


def load_password():
    txt = Path(".local/test-env.md").read_text(encoding="utf-8")
    m = re.search(r"CMCC-502\s+PASSWORD:(\S+)", txt)
    if not m:
        raise SystemExit("[fatal] test-env.md 无 CMCC-502 口令行")
    return m.group(1)


def serial_anchor_check():
    """门禁：两份 phaseA 串口日志各恰 1 处 POWERON（无新 boot）+ 偏移留证。"""
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
    log(f"[gate] 串口锚点留证 {ev}（静态文件证据；\"双 console 持续在录\"为"
        f"外部门禁，由操作人员现场确认，不等同于本检查）")
    if not ok:
        stop("POWERON 锚点数异常（疑似新 boot）")


async def identity_gate(sess, tag, allow_abort=False):
    """六维身份。allow_abort=True 用恢复通道命令（终态采集，STOP 后可用）。"""
    async def one(cmd):
        if allow_abort:
            return await recover_cmd(sess, cmd)
        return await gate_ble(sess, cmd)
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
    if d["sta_ssid"] != SSID or not d["sta_connected"]:
        problems.append("SSID/连接态不符")
    if not str(d["sta_ip"] or "").startswith("192.168.1."):
        problems.append("sta_ip 网段不符")
    if d["mqtt_connected"] is not True:
        problems.append(f"mqtt_connected={d['mqtt_connected']}（S0 须在线）")
    if problems:
        stop("；".join(problems))
    return d


async def do_recovery(sess):
    """专用恢复路径：除 NOT_ATTEMPTED 外任何改指向状态都尝试恢复。
    恢复目标 = S0 实测核验的 _origin_snapshot；成功 = mqtt_connected 且
    回读 == 快照。"""
    global _recovery_state
    if _repoint_state == RP_NA:
        _recovery_state = RC_OK
        log("[recover] 未改指向，无需恢复")
        return
    try:
        if _session is None or not _session.is_connected():
            raise RuntimeError("BLE 不可达")
        if not await _token_gate(_session):
            raise RuntimeError("token renewal failed")
        await recover_cmd(_session, "config.set",
                          {"patch": {"mqtt_host": _origin_snapshot[0],
                                     "mqtt_port": _origin_snapshot[1]}})
        ok = False
        for _ in range(10):
            await asyncio.sleep(8)
            ns = await recover_cmd(_session, "network.status")
            if (ns.get("data") or {}).get("mqtt_connected") is True:
                ok = True
                break
        back = await recover_cmd(_session, "config.get")
        mq = (back.get("data") or {}).get("mqtt") or {}
        readback = (mq.get("mqtt_host"), mq.get("mqtt_port"))
        ns2 = await recover_cmd(_session, "network.status")   # 回读邻近重采
        ok2 = (ns2.get("data") or {}).get("mqtt_connected") is True
        if ok and ok2 and readback == _origin_snapshot:
            _recovery_state = RC_OK
            log(f"[recover] 成功：mqtt_connected=true 且回读={readback}")
        else:
            _recovery_state = RC_FAIL
            log(f"[recover] 失败：mqtt_connected={ok}/{ok2} 回读={readback}"
                f"（不宣称恢复成功；保留 mini_broker 供只读观测/人工接管）")
    except Exception as exc:  # noqa: BLE001
        _recovery_state = RC_FAIL
        log(f"[recover] 异常 {type(exc).__name__}: {str(exc)[:120]}——"
            f"BLE 不可达或恢复失败；保留 mini_broker，人工接管")




async def cleanup_confirm(sess, budget_s=120.0, need_consecutive=2):
    """硬 deadline 的 cleanup 完成窗口：STA 回持久网络 + trial idle，
    连续 need_consecutive 次满足即确认；每轮立即落盘；BLE 请求 timeout
    不超过剩余预算。超时返回 False（调用方 STOP）。"""
    deadline = time.monotonic() + budget_s
    polls = []
    streak = 0
    path = OUT / "000_cleanup_window.json"
    while True:
        remain = deadline - time.monotonic()
        if remain <= 0:
            path.write_text(json.dumps(
                {"result": "TIMEOUT", "polls": polls}, ensure_ascii=False, indent=1),
                encoding="utf-8")
            return False
        await asyncio.sleep(min(10.0, max(1.0, remain - 1)))
        remain = deadline - time.monotonic()
        ns = await gate_ble(sess, "network.status", timeout=max(2.0, min(15.0, remain)))
        dd = (ns or {}).get("data") or {}
        poll = {"t": time.strftime("%H:%M:%S"), "mono": round(time.monotonic(), 3),
                "sta": dd.get("connected_ssid"),
                "connected": dd.get("sta_connected"),
                "trial": ((dd.get("trial") or {}).get("state"))}
        polls.append(poll)
        path.write_text(json.dumps(
            {"polls": polls}, ensure_ascii=False, indent=1), encoding="utf-8")
        good = (poll["connected"] is True and poll["sta"] in ALLOWED_PERSIST_SSID
                and poll["trial"] == "idle")
        streak = streak + 1 if good else 0
        if streak >= need_consecutive:
            path.write_text(json.dumps(
                {"result": "CONFIRMED", "polls": polls}, ensure_ascii=False, indent=1),
                encoding="utf-8")
            log(f"[cleanup] 确认（连续{streak}次满足，STA={poll['sta']}）")
            return True

async def run_round(sess, pw):
    global _repoint_state, _origin_snapshot
    # ---- 门禁 ----
    serial_anchor_check()
    g1 = await identity_gate(sess, "S0-first")
    await asyncio.sleep(20)
    g2 = await identity_gate(sess, "S0-second")
    # MQTT 原值核验：两次一致且等于授权云端基线 → 快照
    o1 = (g1["mqtt_host"], g1["mqtt_port"])
    o2 = (g2["mqtt_host"], g2["mqtt_port"])
    if o1 != o2 or o1 != (CLOUD_HOST, CLOUD_PORT):
        stop(f"MQTT 原值核验失败 first={o1} second={o2} 授权={CLOUD_HOST,CLOUD_PORT}")
        return
    _origin_snapshot = o1
    log(f"[gate] MQTT 原值核验通过，恢复快照={_origin_snapshot}")
    # uptime 数值校验
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

    # ---- 改指向（状态机：发送前置 ATTEMPTED）----
    _repoint_state = RP_ATT
    r = await prot(sess, ble, sess, "config.set",
                   {"patch": {"mqtt_host": PC_IP, "mqtt_port": 21883}})
    conf = False
    if (r or {}).get("ok") is True:
        back = await prot(sess, gate_ble, sess, "config.get")
        mq = ((back or {}).get("data") or {}).get("mqtt") or {}
        if (mq.get("mqtt_host"), mq.get("mqtt_port")) == (PC_IP, 21883):
            conf = True
    _repoint_state = RP_CONF if conf else RP_ATT
    log(f"[S4] 改指向 state={_repoint_state}（成功回包+回读确认={conf}）")
    m = MqttV1("127.0.0.1:21883")
    try:
        if not await wait_broker(m):
            stop("设备未接入 mini_broker（75s）")
            return
        # ---- async expired（须先 GOT_IP；ACCEPTED-TTL 过期单列，重试至多一次）----
        target_hit, classification, opE = False, None, None
        for attempt in (1, 2):
            r = mqtt_req(m, "network.tryconnect", {"ssid": SSID, "password": pw})
            opE = ((r or {}).get("data") or {}).get("operation_id")
            log(f"[S4] attempt{attempt} async 受理 op-E={opE}")
            if not opE:
                stop("无 operation_id")
                return
            tG = time.monotonic()
            seen_got_ip, terminal = False, None
            while time.monotonic() < tG + 420 and not _abort:
                await asyncio.sleep(10)
                og = await prot(sess, gate_ble, sess, "operation.get",
                                {"operation_id": opE})
                cur = (og.get("data") or {}).get("state")
                ns = await prot(sess, gate_ble, sess, "network.status")
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
                return
            if terminal is None:
                stop(f"attempt{attempt} 轮询 420s 无终态（unknown）——不重试")
                return
            if terminal in ("cancelled", "failed"):
                stop(f"attempt{attempt} 异常终态 {terminal}——立即 STOP，不重试")
                return
            if terminal == "expired" and seen_got_ip:
                target_hit = True
                _results["op_e"] = "PASS"
                log(f"[S4] attempt{attempt} = GOT_IP→expired（目标分支 PASS）")
                break
            # 唯一允许重试的分类：ACCEPTED_TTL_EXPIRED
            classification = "ACCEPTED_TTL_EXPIRED（未达 GOT_IP，不计目标验收）"
            log(f"[S4] attempt{attempt} {classification}")
            if attempt == 1:
                log("[S4] 分类重试（至多一次）；先确认前一次 cleanup 已结束")
                if not await cleanup_confirm(sess, 120):
                    stop("重试前 cleanup 确认失败")
                    return
        if not target_hit:
            stop(f"op-E 未达成 GOT_IP→expired（分类={classification}，重试机会已用）")
            _results["op_e"] = "FAILED"
            return
        _results["op_e"] = "PASS"
        # ---- cleanup 完成窗口（硬 deadline ≤120s，未确认不进 op-V）----
        if not await cleanup_confirm(sess, 120.0):
            stop("cleanup 完成窗口 120s 未确认（STA/trial 未恢复）")
            _results["op_v"] = "SKIPPED"
            return
        # ---- op-V job-clear 验证 ----
        sc, frags = await prot(sess, ble_scan_full, sess)
        cond, reasons = scan_five_conditions(sc, frags)
        if not cond:
            log(f"[S4] 首扫未满足五条件 {reasons}，重扫一次")
            sc, frags = await prot(sess, ble_scan_full, sess)
            cond, reasons = scan_five_conditions(sc, frags)
        if cond:
            r = mqtt_req(m, "network.tryconnect", {"ssid": PROBE}, wait=10)
            opV = ((r or {}).get("data") or {}).get("operation_id")
            if not opV or opV == opE:
                stop(f"op-V 受理异常 id={opV}（job 未清或拒绝）")
                return
            log(f"[S4] op-V 受理新 id={opV}（≠op-E，job 已清确证）")
            opv_final, opv_err = None, None
            for _ in range(12):
                await asyncio.sleep(5)
                og = await prot(sess, gate_ble, sess, "operation.get",
                                {"operation_id": opV})
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
        m.cli.loop_stop()


async def main():
    global _log, _broker, _token_origin, _session
    OUT.mkdir(parents=True, exist_ok=True)
    _log = open(OUT / "capture_log.txt", "a", encoding="utf-8")
    if (OUT / "capture_log.txt").exists():
        print("[fatal] 证据目录非空（capture_log.txt 已存在）——禁止静默复用，"
              "请归档后更换目录", flush=True)
        return 2
    log("=== S6 round2（v4 脚本）开始（窄授权：MQTT async expired[须 GOT_IP] + op-V）===")
    pw = load_password()
    code = 3
    _broker = subprocess.Popen(
        [sys.executable, "tools/broker_local/mini_broker.py", "--port", "21883"],
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    try:
        recs = await scan_filtered(10.0)
        hit = [r for r in recs if r["address"].lower() == TARGET.lower()]
        if not hit:
            log("[STOP] C8E4 不在广播中（无静默回退）")
            return 2
        async with BleakClient(TARGET, timeout=25) as cli:
            sess = Session(cli, address=TARGET)
            _session = sess
            await sess._connect_once()          # S0 无条件登录 = 计时起点
            _token_origin = time.monotonic()
            log("[S0] 无条件 auth.login（token 计时起点）")
            try:
                await run_round(sess, pw)
                all_pass = (_results.get("op_e") == "PASS"
                            and _results.get("op_v") == "PASS"
                            and _abort is None)
                code = 0 if all_pass else 3
                log(f"[result] {_results} abort={_abort} → exit={code}"
                    f"（两项都 PASS 才为 0）")
            finally:
                # 专用恢复路径：除 NOT_ATTEMPTED 外都尝试（allowlist 命令）
                await do_recovery(sess)
                gEnd = None
                try:
                    gEnd = await identity_gate(sess, "end", allow_abort=True)
                except Exception as exc:  # noqa: BLE001
                    log(f"[end] 终态身份采集失败 {type(exc).__name__}: {str(exc)[:120]}")
                (OUT / "000_end_state.json").write_text(json.dumps(
                    {"gate": gEnd, "abort": _abort, "recovery": _recovery_state,
                     "repoint_state": _repoint_state},
                    ensure_ascii=False, indent=1), encoding="utf-8")
                closer = getattr(sess, "close", None)
                if closer is not None:
                    await closer()
                else:
                    await cli.disconnect()
    finally:
        # broker 收尾：仅 NOT_ATTEMPTED 或 RECOVERY_CONFIRMED 才终止
        if _repoint_state != RP_NA and _recovery_state != RC_OK:
            log(f"[finally] mini_broker 保留运行（pid={_broker.pid}）——"
                f"改指向状态={_repoint_state} 恢复={_recovery_state}；"
                f"人工接管后可 kill {_broker.pid}")
            _broker = None   # 不终止
        elif _broker is not None:
            _broker.terminate()
            log("[finally] mini_broker 已终止")
    # SHA256SUMS（目录内全部文件）
    try:
        lines = []
        for f in sorted(OUT.iterdir()):
            if f.is_file() and f.name != "SHA256SUMS":
                h = hashlib.sha256(f.read_bytes()).hexdigest()
                lines.append(f"{h}  {f.name}")
        (OUT / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="utf-8")
        log(f"[evidence] SHA256SUMS 生成（{len(lines)} 文件）")
    except Exception as exc:  # noqa: BLE001
        log(f"[evidence] SHA256SUMS 生成失败 {type(exc).__name__}")
    log(f"[done] abort={_abort} repoint={_repoint_state} recovery={_recovery_state} "
        f"results={_results}")
    return code


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
