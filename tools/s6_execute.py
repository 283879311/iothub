#!/usr/bin/env python3
"""S6 现场预采集执行脚本 v2（评审 2026-11-14 修订；执行口径 v11=2415983 + 分支重定界）。

v1→v2（评审五项）：
① STOP 后专用恢复路径：_abort 只拦采集命令；恢复走独立 allowlist
   （config.set 恢复 + network.status/config.get 验证），失败保留
   mini_broker 运行（不提前终止设备可能依赖的 broker）并输出人工接管指引；
② 恢复成功判据 = mqtt_connected=true **且** config.get 回读 host/port ==
   原值；失败绝不输出恢复成功；
③ ble_scan_full 经 prot()（逐命令 token 守卫）；
④ 扫描加 data.status=="ok"；op-V 断言终态 failed + error=ap_not_found；
⑤ 全程 try/finally 收尾；每轮独立证据目录（本轮 round2）。
口令只从 .local/test-env.md 内存读取，帧内 sanitize 脱敏，不落盘明文。
"""

import asyncio
import json
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
CLOUD_HOST, CLOUD_PORT = "139.9.93.245", 21883
SSID = "CMCC-502"
PROBE = "__jobclear_probe__"
TOKEN_LIMIT_S = 480.0

_log = None
_token_origin = None
_abort = None
_repointed = False
_recovered = None          # None=未执行 / True / False


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
    log("[token] 续签成功（age→0）")
    return True


async def prot(sess, coro_fn, *a, **kw):
    """受保护命令守卫（采集命令：_abort 时拒绝）。"""
    if _abort:
        raise RuntimeError(f"aborted: {_abort}")
    if token_age() >= TOKEN_LIMIT_S and not await guarded_login(sess):
        raise RuntimeError("aborted: token renewal failed")
    return await coro_fn(*a, **kw)


async def recover_cmd(sess, cmd, args=None, timeout=20):
    """恢复 allowlist：_abort 非空时仍可执行；仅限恢复/验证命令。"""
    if cmd not in ("config.set", "config.get", "network.status", "device.status",
                   "device.info"):
        raise RuntimeError(f"recover_cmd 拒绝非恢复命令 {cmd}")
    req = {"id": f"rcv-{sess.next_seq()}", "version": "1.0", "cmd": cmd,
           "args": args or {}, "token": sess.token}
    res = await sess.send_request(req, timeout=timeout)
    save(f"recover_{cmd.replace('.', '_')}", "ble", req, res.obj)
    return res.obj


async def ble(sess, cmd, args=None, timeout=20):
    req = {"id": f"s6-{sess.next_seq()}", "version": "1.0", "cmd": cmd,
           "args": args or {}, "token": sess.token}
    res = await sess.send_request(req, timeout=timeout)
    save(f"{cmd.replace('.', '_')}_ble", "ble", req, res.obj)
    return res.obj


async def ble_scan_full(sess):
    req = {"id": f"s6-{sess.next_seq()}", "version": "1.0", "cmd": "network.scan",
           "args": {}, "token": sess.token}
    res = await sess.send_request(req, timeout=30)
    save("network_scan_ble", "ble", req, res.obj)
    return res.obj, res.frag_sizes


def scan_five_conditions(sc, frag_sizes):
    """五条件：envelope ok + data.status==ok + 分片完整 + count==len(aps)
    + aps 存在且为数组且不含 probe。返回 (ok, reasons)。"""
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


async def identity_gate(sess, tag):
    info = (await ble(sess, "device.info")).get("data", {})
    st = (await ble(sess, "device.status")).get("data", {})
    net = (await ble(sess, "network.status")).get("data", {})
    cfg = (await ble(sess, "config.get")).get("data", {})
    d = {
        "tag": tag, "adv": TARGET, "device_id": info.get("device_id"),
        "build_sha": info.get("build_sha"), "uptime_ms": st.get("uptime_ms"),
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
    if problems:
        stop("；".join(problems))
    return d


async def do_recovery(sess):
    """专用恢复路径（STOP 后仍可执行；仅恢复/验证命令）。
    成功判据 = mqtt_connected=true 且 config.get 回读 == 原值。"""
    global _recovered
    if not _repointed:
        _recovered = True
        log("[recover] 未改指向，无需恢复")
        return
    try:
        if token_age() >= TOKEN_LIMIT_S:
            if not await guarded_login(sess):
                raise RuntimeError("token renewal failed")
        await recover_cmd(sess, "config.set",
                          {"patch": {"mqtt_host": CLOUD_HOST,
                                     "mqtt_port": CLOUD_PORT}})
        ok = False
        for _ in range(10):
            await asyncio.sleep(8)
            ns = await recover_cmd(sess, "network.status")
            if (ns.get("data") or {}).get("mqtt_connected") is True:
                ok = True
                break
        back = await recover_cmd(sess, "config.get")
        mq = (back.get("data") or {}).get("mqtt") or {}
        readback = (mq.get("mqtt_host"), mq.get("mqtt_port"))
        if ok and readback == (CLOUD_HOST, CLOUD_PORT):
            _recovered = True
            log(f"[recover] 成功：mqtt_connected=true 且回读={readback}")
        else:
            _recovered = False
            log(f"[recover] 失败：mqtt_connected={ok} 回读={readback}"
                f"（不宣称恢复成功；保留 mini_broker 供只读观测/人工接管）")
    except Exception as exc:  # noqa: BLE001
        _recovered = False
        log(f"[recover] 异常 {type(exc).__name__}: {str(exc)[:120]}"
            f"——BLE 不可达或恢复失败；保留 mini_broker，人工接管")


async def run_round(sess, pw):
    global _repointed
    # ---- 门禁 ----
    g1 = await identity_gate(sess, "S0-first")
    await asyncio.sleep(20)
    g2 = await identity_gate(sess, "S0-second")
    u1, u2 = g1["uptime_ms"], g2["uptime_ms"]
    du, dw = (u2 - u1) / 1000.0, g2["mono"] - g1["mono"]
    boot_wall = time.strftime("%H:%M:%S", time.localtime(time.time() - u1 / 1000.0))
    log(f"[gate] uptime 连续性 Δu={du:.1f}s Δwall={dw:.1f}s boot≈{boot_wall}")
    if abs(du - dw) > 3:
        stop("uptime 不连续")
    t = time.strptime(boot_wall, "%H:%M:%S")
    if not (time.strptime("19:44:00", "%H:%M:%S") <= t <= time.strptime("19:46:00", "%H:%M:%S")):
        stop(f"boot {boot_wall} 偏离稳定窗口锚点")
    if _abort:
        return
    log("[gate] 通过（本轮只执行：MQTT async expired + op-V；无 sync 分支）")

    # ---- 改指向（finally 兜底恢复）----
    await prot(sess, ble, sess, "config.set",
               {"patch": {"mqtt_host": PC_IP, "mqtt_port": 21883}})
    _repointed = True
    m = MqttV1("127.0.0.1:21883")
    try:
        if not await wait_broker(m):
            stop("设备未接入 mini_broker（75s）")
            return
        # ---- async expired ----
        r = mqtt_req(m, "network.tryconnect", {"ssid": SSID, "password": pw})
        opE = ((r or {}).get("data") or {}).get("operation_id")
        log(f"[S4] async 受理 op-E={opE}")
        if not opE:
            stop("无 operation_id")
            return
        tG = time.monotonic()
        expired, final = False, None
        while time.monotonic() < tG + 420 and not _abort:
            await asyncio.sleep(10)
            og = await prot(sess, ble, sess, "operation.get", {"operation_id": opE})
            cur = (og.get("data") or {}).get("state")
            ns = await prot(sess, ble, sess, "network.status")
            tb = ((ns.get("data") or {}).get("trial") or {}).get("state")
            log(f"[S4] +{time.monotonic()-tG:.0f}s op={cur} trial={tb}")
            if cur == "got_ip":
                log("[S4] GOT_IP（若随后 trial idle：归档清理原因未判定）")
            if cur in ("expired", "cancelled", "failed"):
                final = (cur, (og.get("data") or {}).get("error"))
                expired = cur == "expired"
                break
        if not expired:
            stop(f"op-E 非 expired 终态（final={final}）")
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
                og = await prot(sess, ble, sess, "operation.get", {"operation_id": opV})
                opv_final = (og.get("data") or {}).get("state")
                opv_err = (og.get("data") or {}).get("error")
                if opv_final in ("failed", "expired", "cancelled"):
                    break
            if opv_final == "failed" and opv_err == "ap_not_found":
                log("[S4] op-V PASS：failed/ap_not_found（run 内 clear_job_sync 路径）")
            else:
                stop(f"op-V 终态断言失败 {opv_final}/{opv_err}")
                return
        else:
            log(f"[S4] 降级：{reasons}——op-V 放弃，该项记证据降级/未完成")
    finally:
        m.cli.loop_stop()


async def main():
    global _log, _broker, _token_origin, _session_holder
    OUT.mkdir(parents=True, exist_ok=True)
    _log = open(OUT / "capture_log.txt", "a", encoding="utf-8")
    log("=== S6 round2 开始（窄授权：MQTT async expired + op-V）===")
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
            _session_holder["sess"] = sess
            await sess._connect_once()          # S0 无条件登录 = 计时起点
            _token_origin = time.monotonic()
            log("[S0] 无条件 auth.login（token 计时起点）")
            try:
                await run_round(sess, pw)
                code = 0 if _abort is None else 3
            finally:
                # 专用恢复路径：采集命令已停，恢复 allowlist 仍可执行
                sess2 = _session_holder.get("sess")
                if sess2 is not None and sess2.is_connected():
                    try:
                        await do_recovery(sess2)
                    except Exception as exc:  # noqa: BLE001
                        log(f"[recover] 兜底异常 {type(exc).__name__}: {str(exc)[:120]}")
                else:
                    log("[recover] BLE 不可达——保留 mini_broker 只读观测，人工接管")
                gEnd = None
                sess3 = _session_holder.get("sess")
                if sess3 is not None and sess3.is_connected():
                    try:
                        gEnd = await identity_gate(sess3, "end")
                    except Exception as exc:  # noqa: BLE001
                        log(f"[end] 终态身份采集失败 {type(exc).__name__}")
                (OUT / "000_end_state.json").write_text(json.dumps(
                    {"gate": gEnd, "abort": _abort, "recovered": _recovered},
                    ensure_ascii=False, indent=1), encoding="utf-8")
                await cli.disconnect()
    finally:
        # broker 收尾：恢复失败时保留（设备可能仍指向本地）并输出指引
        if _recovered is False:
            log(f"[finally] mini_broker 保留运行（pid={_broker.pid}）——"
                f"人工接管后可 kill {_broker.pid}")
        else:
            _broker.terminate()
            log("[finally] mini_broker 已终止")
    log(f"[done] abort={_abort} recovered={_recovered}")
    return code


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
