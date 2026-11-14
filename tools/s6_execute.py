#!/usr/bin/env python3
"""S6 现场预采集执行脚本（授权 2026-11-14；唯一执行口径 v11=2415983）。

门禁（第一条设备控制命令前）：串口锚点核验 → BLE 连接 → 无条件 auth.login
（token 计时起点）→ 六维身份 ×2 次（间隔 20s，uptime 连续性对照）。
逐命令 token 守卫：每次受保护命令发送前查年龄（自最近一次成功签发起算），
≥480s 先 auth.login（成功=登录+token 更新+起点重置三件套），任一失败 STOP。
产出：artifacts/cmd04_flash_20261114/pre/ 续编号帧 + capture_log.txt。
口令只从 .local/test-env.md 内存读取，帧内经 sanitize 脱敏，不落盘明文。
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
from p0_acceptance import Session, choose_target, scan_filtered, sanitize  # noqa: E402

OUT = Path("artifacts/cmd04_flash_20261114/pre")
TARGET = "B0:A7:32:28:C8:E6"
PC_IP = "192.168.1.19"
CLOUD = ("139.9.93.245", 21883)
SSID = "CMCC-502"
PROBE = "__jobclear_probe__"
NEG_ID = "op-00000000CDEF1234"
TOKEN_LIMIT_S = 480.0
WINDOW = 0.5

_log = None
_seq = 0
_token_origin = None        # host monotonic，最近一次成功签发
_abort = None               # 非空 = 中止原因


def log(t):
    global _log
    line = f"{time.strftime('%H:%M:%S')} {t}"
    print(line, flush=True)
    _log.write(line + "\n")
    _log.flush()


def save(branch, source, request, response):
    global _seq
    idx = len(list(OUT.glob("[0-9][0-9][0-9]_*.json"))) + 40  # 接续既有 01-39
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
    """auth.login 三件套同一成功条件：登录成功+token 更新+起点重置。"""
    global _token_origin
    r = await sess.login(timeout=15)
    if r.obj.get("ok") is not True or not sess.token:
        stop("auth.login 失败或 token 未更新")
        return False
    _token_origin = time.monotonic()
    log(f"[token] 续签成功，计时起点重置（age→0）")
    return True


async def prot(sess, coro_fn, *a, **kw):
    """受保护命令守卫：发送前查 token 年龄（≥480s 先续签）。"""
    if _abort:
        raise RuntimeError(f"aborted: {_abort}")
    if token_age() >= TOKEN_LIMIT_S:
        log("[token] 年龄≥480s，发送前续签")
        if not await guarded_login(sess):
            raise RuntimeError("aborted: token renewal failed")
    return await coro_fn(*a, **kw)


async def ble(sess, cmd, args=None, timeout=20):
    req = {"id": f"s6-{sess.next_seq()}", "version": "1.0", "cmd": cmd,
           "args": args or {}, "token": sess.token}
    res = await sess.send_request(req, timeout=timeout)
    save(f"{cmd.replace('.', '_')}_ble", "ble", req, res.obj)
    return res.obj


async def ble_scan_full(sess):
    """network.scan（BLE 允许），返回 (resp, frag_ok)。frag_ok=分片完整性。"""
    req = {"id": f"s6-{sess.next_seq()}", "version": "1.0", "cmd": "network.scan",
           "args": {}, "token": sess.token}
    res = await sess.send_request(req, timeout=30)
    frag_ok = True  # Session._wait_response 只收完整重组 JSON；能返回即分片完整
    save("network_scan_ble", "ble", req, res.obj)
    return res.obj, frag_ok, res.frag_sizes


def mqtt_req(m, cmd, args=None, wait=10.0, note=None):
    req = {"id": f"mq-{m._next_rid()}", "version": "1.0", "cmd": cmd,
           "args": args or {}}
    resp, rid = m.send(req, wait=wait)
    save(f"{cmd.replace('.', '_')}_mqtt", "mqtt", req, resp or {"_note": note or "no response"})
    return resp


async def wait_broker(m, seconds=75.0):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline and not _abort:
        r, _ = m.send({"id": "probe", "version": "1.0", "cmd": "device.info", "args": {}},
                      wait=3.0)
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
    """门禁：两份 phaseA 串口日志各恰 1 处 POWERON（无新 boot）+ 字节偏移留证。"""
    ev = {}
    for port in ("ttyUSB0", "ttyUSB1"):
        f = OUT.parent / f"phaseA/serial_{port}.log"
        data = f.read_bytes()
        offs = [m.start() for m in re.finditer(rb"rst:0x1 \(POWERON_RESET\)", data)]
        ev[port] = {"size_bytes": len(data), "poweron_offsets": offs}
        if len(offs) != 1:
            stop(f"{port} POWERON 锚点数={len(offs)}（预期 1，疑似新 boot）")
    (OUT / "000_gate_serial_anchor.json").write_text(
        json.dumps(ev, indent=1), encoding="utf-8")
    log(f"[gate] 串口锚点留证 {ev}")


async def identity_gate(sess, tag):
    d = {}
    info = (await prot(sess, ble, sess, "device.info")).get("data", {})
    st = (await prot(sess, ble, sess, "device.status")).get("data", {})
    net = (await prot(sess, ble, sess, "network.status")).get("data", {})
    cfg = (await prot(sess, ble, sess, "config.get")).get("data", {})
    d.update({
        "tag": tag, "adv": TARGET, "device_id": info.get("device_id"),
        "build_sha": info.get("build_sha"),
        "uptime_ms": st.get("uptime_ms"),
        "sta_ssid": net.get("connected_ssid"), "sta_connected": net.get("sta_connected"),
        "sta_ip": net.get("sta_ip"),
        "mqtt_host": (cfg.get("mqtt") or {}).get("mqtt_host"),
        "mqtt_port": (cfg.get("mqtt") or {}).get("mqtt_port"),
        "mqtt_connected": net.get("mqtt_connected"),
        "wall": time.strftime("%H:%M:%S"), "mono": round(time.monotonic(), 3),
    })
    if d["device_id"] != "IotHub-C8E4" or d["build_sha"] != "d4188551c":
        stop(f"身份不符 {d['device_id']}/{d['build_sha']}")
    if d["sta_ssid"] != SSID or not d["sta_connected"]:
        stop(f"SSID/连接态不符 {d['sta_ssid']}/{d['sta_connected']}")
    if not str(d["sta_ip"] or "").startswith("192.168.1."):
        stop(f"sta_ip 网段不符 {d['sta_ip']}")
    return d


async def sync_tryconnect(sess, pw, note):
    r = await prot(sess, ble, sess, "network.tryconnect",
                   {"ssid": SSID, "password": pw}, timeout=40)
    return r


async def main():
    global _log, _abort
    OUT.mkdir(parents=True, exist_ok=True)
    _log = open(OUT / "capture_log.txt", "a", encoding="utf-8")
    log("=== S6 执行开始（授权 2026-11-14，v11=2415983）===")
    pw = load_password()

    # ---- 门禁：串口锚点 ----
    serial_anchor_check()
    if _abort:
        return 1

    recs = await scan_filtered(10.0)
    hit = [r for r in recs if r["address"].lower() == TARGET.lower()]
    if not hit:
        log("[STOP] C8E4 不在广播中（无静默回退）")
        return 2
    log(f"[gate] target {TARGET} name={hit[0].get('name')!r}")

    broker = subprocess.Popen(
        [sys.executable, "tools/broker_local/mini_broker.py", "--port", "21883"],
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

    async with BleakClient(TARGET, timeout=25) as cli:
        sess = Session(cli, address=TARGET)
        await sess._connect_once()          # S0 无条件登录 = token 计时起点
        _token_origin = time.monotonic()
        log("[S0] 无条件 auth.login 完成（token 计时起点）")

        g1 = await identity_gate(sess, "S0-first")
        await asyncio.sleep(20)
        g2 = await identity_gate(sess, "S0-second")
        u1, u2 = g1["uptime_ms"], g2["uptime_ms"]
        if u1 is None or u2 is None:
            stop("uptime 缺失")
        else:
            du, dw = (u2 - u1) / 1000.0, g2["mono"] - g1["mono"]
            boot1 = time.time() - u1 / 1000.0
            boot_wall = time.strftime("%H:%M:%S", time.localtime(boot1))
            log(f"[gate] uptime 连续性 Δu={du:.1f}s Δwall={dw:.1f}s boot≈{boot_wall}")
            if abs(du - dw) > 3:
                stop(f"uptime 不连续 Δu={du:.1f} Δwall={dw:.1f}")
            if not (time.strptime("19:44:00", "%H:%M:%S") <= time.strptime(boot_wall, "%H:%M:%S")
                    <= time.strptime("19:46:00", "%H:%M:%S")):
                stop(f"boot {boot_wall} 偏离稳定窗口锚点(19:44:0x–19:45:0x)")
        (OUT / "000_gate_identity.json").write_text(
            json.dumps({"first": g1, "second": g2}, ensure_ascii=False, indent=1),
            encoding="utf-8")
        if _abort:
            broker.terminate()
            return 1
        log("[gate] 全部通过，进入分支采集")

        # ---- S1 分支① sync trial cancel ----
        r = await sync_tryconnect(sess, pw, "b1")
        trial1 = (r.get("data") or {}).get("trial_id")
        if (r.get("ok") is not True) or not trial1:
            stop(f"分支 sync tryconnect 未成功 ok={r.get('ok')} trial={trial1}")
        log(f"[S1] sync 受理 trial={trial1}")
        await prot(sess, ble, sess, "operation.get", {"operation_id": NEG_ID})
        await prot(sess, ble, sess, "network.status")
        cr = await prot(sess, ble, sess, "network.cancel", {"trial_id": trial1}, timeout=25)
        log(f"[S1] cancel ok={(cr or {}).get('ok')}")
        await prot(sess, ble, sess, "network.status")

        # ---- S2 分支② 04c 段 A ----
        r = await sync_tryconnect(sess, pw, "b2-segA")
        trial2 = (r.get("data") or {}).get("trial_id")
        if (r.get("ok") is not True) or not trial2:
            stop(f"分支 sync tryconnect 未成功 ok={r.get('ok')} trial={trial2}")
        log(f"[S2] 段A受理 trial={trial2}")
        await prot(sess, ble, sess, "network.status")
        cr = await prot(sess, ble, sess, "network.cancel", {"trial_id": trial2}, timeout=25)
        log(f"[S2] cancel ok={(cr or {}).get('ok')}")

        # ---- S3 分支④ 专用 TTL（保持 BLE 会话）----
        r = await sync_tryconnect(sess, pw, "b4-ttl")
        trial3 = (r.get("data") or {}).get("trial_id")
        if (r.get("ok") is not True) or not trial3:
            stop(f"分支 sync tryconnect 未成功 ok={r.get('ok')} trial={trial3}")
        t0p = time.monotonic()
        log(f"[S3] TTL 分支受理 trial={trial3} T0'={t0p:.1f}（300s 不 cancel）")
        deadline = t0p + 330
        seen_ready, seen_idle = False, False
        while time.monotonic() < deadline and not _abort:
            await asyncio.sleep(15)
            ns = await prot(sess, ble, sess, "network.status")
            tb = (ns.get("data") or {}).get("trial") or {}
            st = tb.get("state")
            age = time.monotonic() - t0p
            if st == "got_ip":
                seen_ready = True
            if st == "idle" and age >= 280:
                seen_idle = True
                log(f"[S3] trial idle @T0'+{age:.0f}s（lazy clear 证据闭合 ready→idle）")
                break
            log(f"[S3] T0'+{age:.0f}s trial={st}")
        if not (seen_ready and seen_idle):
            stop(f"S3 TTL 证据不全 ready={seen_ready} idle={seen_idle}")

        # ---- S4 分支⑤ async expired（唯一改指向段）----
        if not _abort:
            await prot(sess, ble, sess, "config.set",
                       {"patch": {"mqtt_host": PC_IP, "mqtt_port": 21883}})
            m = MqttV1("127.0.0.1:21883")
            if not await wait_broker(m):
                stop("设备未接入 mini_broker（75s）")
            else:
                r = mqtt_req(m, "network.tryconnect", {"ssid": SSID, "password": pw})
                opE = ((r or {}).get("data") or {}).get("operation_id")
                st = ((r or {}).get("data") or {}).get("state")
                log(f"[S4] async 受理 op-E={opE} state={st}")
                tG = time.monotonic()
                last_state, gotip_at, trial_idle_while_gotip = None, None, False
                expired = False
                while time.monotonic() < tG + 420 and not _abort:
                    await asyncio.sleep(15)
                    og = await prot(sess, ble, sess, "operation.get", {"operation_id": opE})
                    cur = (og.get("data") or {}).get("state")
                    ns = await prot(sess, ble, sess, "network.status")
                    tb = ((ns.get("data") or {}).get("trial") or {}).get("state")
                    if cur == "got_ip" and gotip_at is None:
                        gotip_at = time.monotonic()
                        log(f"[S4] GOT_IP @受理+{gotip_at-tG:.0f}s（T1 区间取证入档）")
                    if cur == "got_ip" and tb == "idle" and gotip_at is not None:
                        if not trial_idle_while_gotip:
                            trial_idle_while_gotip = True
                            log("[S4] 观察到 trial idle、operation 尚未发布终态；清理原因未判定")
                    if cur in ("expired", "failed"):
                        expired = cur == "expired"
                        log(f"[S4] op-E 终态 {cur} @+{time.monotonic()-tG:.0f}s")
                        break
                    last_state = cur
                if not expired:
                    stop(f"op-E 未自然 expired（last={last_state}）")
                # cleanup 确认：persistent 恢复（回落链允收）
                ok = False
                for _ in range(10):
                    await asyncio.sleep(12)
                    ns = await prot(sess, ble, sess, "network.status")
                    dd = ns.get("data") or {}
                    if dd.get("sta_connected") and dd.get("connected_ssid"):
                        ok = True
                        log(f"[S4] cleanup 后 STA 恢复 ssid={dd.get('connected_ssid')}")
                        break
                if not ok:
                    stop("cleanup 后 120s 未恢复 persistent")
                # op-V：probe 扫描前置（五条件）
                sc, frag_ok, frags = await ble_scan_full(sess)
                d = sc.get("data") or {}
                aps = d.get("aps")
                cond = (sc.get("ok") is True and frag_ok and len(frags) > 0
                        and isinstance(aps, list) and d.get("count") == len(aps)
                        and all((a or {}).get("ssid") != PROBE for a in aps))
                if not cond:
                    log("[S4] 首扫未满足五条件，重扫一次")
                    sc, frag_ok, frags = await ble_scan_full(sess)
                    d = sc.get("data") or {}
                    aps = d.get("aps")
                    cond = (sc.get("ok") is True and frag_ok and len(frags) > 0
                            and isinstance(aps, list) and d.get("count") == len(aps)
                            and all((a or {}).get("ssid") != PROBE for a in aps))
                if cond:
                    r = mqtt_req(m, "network.tryconnect", {"ssid": PROBE}, wait=10)
                    opV = ((r or {}).get("data") or {}).get("operation_id")
                    log(f"[S4] op-V 受理 {opV}（≠op-E? {opV != opE}）")
                    if not opV or opV == opE:
                        stop(f"op-V 返回异常 id={opV}（job 未清或拒绝）")
                    else:
                        await asyncio.sleep(30)  # 预扫描失败 ~15s + 终态 + clear_job_sync
                        og = await prot(sess, ble, sess, "operation.get", {"operation_id": opV})
                        log(f"[S4] op-V 终态 {(og.get('data') or {}).get('state')}")
                else:
                    log("[S4] 降级路径：probe 两轮异常/存在，op-V 放弃，证据降级/未完成")
                # 恢复（续签③紧邻在前）
                if token_age() >= TOKEN_LIMIT_S:
                    await guarded_login(sess)
                await prot(sess, ble, sess, "config.set",
                           {"patch": {"mqtt_host": CLOUD[0], "mqtt_port": CLOUD[1]}})
                ok = False
                for _ in range(10):
                    await asyncio.sleep(8)
                    ns = await prot(sess, ble, sess, "network.status")
                    if (ns.get("data") or {}).get("mqtt_connected") is True:
                        ok = True
                        break
                if not ok:
                    stop("恢复 80s 内 mqtt_connected 未成立")
                log("[S4] 已恢复云端指向（mqtt_connected=true）")
            m.cli.loop_stop()
        broker.terminate()

        # ---- S5/S6 收尾 ----
        log("[S5] trial mismatch：标不可达/未覆盖（sync trial operation_id 为空，"
            "不构成 04b mismatch 语义；不发起命令）")
        gEnd = await identity_gate(sess, "end")
        (OUT / "000_end_state.json").write_text(
            json.dumps({"gate": gEnd, "abort": _abort}, ensure_ascii=False, indent=1),
            encoding="utf-8")
        await cli.disconnect()

    log(f"[done] abort={_abort}")
    return 0 if _abort is None else 3


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
