#!/usr/bin/env python3
"""S6 阶段 B · 65D8 六维身份 ×2（**受限**采集，需单独授权窗口）。

**本采集不是"无副作用审计"**，会话本身包含以下可能改变 RAM 状态的动作：
  - `auth.login`：校验口令并写入**短时 RAM token**（不写持久配置）；
  - `device.status` / `network.status`：内部复用 `app_cmd_build_network_status()`，
    若 trial 恰好过期会调用 `app_cmd_net_trial_clear()`（清理 RAM trial，
    必要时结束配置事务）；
  - 断开连接：`app_ble_cmd_on_disconnect()` 可能取消 BLE 所属的可取消
    异步网络操作、清理 BLE 所属 trial。
因此采集**前置要求** `trial.state == idle`（非 idle 立即停止，不尝试修复）；
断开只发生一次，且在证据中显式记录。

授权范围内允许的命令仅：device.info / device.status / network.status / config.get。
不做配置写入、网络扫描/连接/断开、MQTT 发布、diag 注入、复位、烧录。

输出：`<out-dir>/identity_capture.json`
  - 两轮**脱敏后的解析响应对象**与原始长度（raw_len；**不保存原始字节**）
  - 两轮 console 快照（文件大小/行数/最后 uptime/行号、client_id、boot 标记、ELF SHA）
  - 六维逐项比对 + 实测轮次间隔

用法：
  .venv-bleak/bin/python tools/s6_identity_capture.py \
      --console-log artifacts/cmd04_flash_20261114/phaseA/serial_ttyUSB1.log
"""

import argparse
import asyncio
import json
import re
import sys
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "ble_p0"))

from p0_acceptance import Session, sanitize, scan_filtered  # noqa: E402

ADDR = "10:97:BD:D3:65:DA"
EXPECT_DEVICE_ID = "IotHub-65D8"
EXPECT_BUILD_SHA = "a7e04b6a9"
EXPECT_CLIENT_ID = "iothub-1097BDD365D8"
EXPECT_SSID = "CMCC-502"          # 当前持久网络（stage A 后实测）
READ_CMDS = ("device.info", "device.status", "network.status", "config.get")


def console_snapshot(log_path):
    """只读快照：不打开串口，不触碰采集进程。"""
    p = Path(log_path)
    st = p.stat()
    data = p.read_bytes()
    text = data.decode("utf-8", "replace")
    lines = text.splitlines()
    total_lines = len(lines)

    def line_of(idx):
        return text.count("\n", 0, idx) + 1

    snap = {"path": str(p), "size": st.st_size, "mtime_unix": st.st_mtime,
            "total_lines": total_lines, "at_unix": int(time.time())}

    ups = list(re.finditer(r"I \((\d+)\)", text))
    if ups:
        m = ups[-1]
        snap["uptime_ms"] = int(m.group(1))
        snap["uptime_line"] = line_of(m.start())
        snap["uptime_tail"] = lines[snap["uptime_line"] - 1][:200] if lines else ""
    boots = list(re.finditer(r"rst:0x1", text))
    if boots:
        snap["boot_marker_line"] = line_of(boots[-1].start())
        before = text[:boots[-1].start()]
        ups_b = list(re.finditer(r"I \((\d+)\)", before))
        snap["boot_uptime_ms"] = int(ups_b[-1].group(1)) if ups_b else None
    cids = list(re.finditer(r"client_id=(iothub-[0-9A-Fa-f]+)", text))
    if cids:
        snap["client_id"] = cids[-1].group(1)
        snap["client_id_line"] = line_of(cids[-1].start())
    shas = list(re.finditer(r"app_init: ELF file SHA256:\s+([0-9a-fA-F]+)", text))
    if shas:
        snap["elf_sha"] = shas[-1].group(1).lower()
        snap["elf_sha_line"] = line_of(shas[-1].start())
    return snap


async def read_round(s, tag):
    """一轮读取；任一命令 ok=false 或异常立即上抛（授权要求：失败即停）。"""
    out = {"tag": tag, "at_unix": int(time.time()),
           "at": time.strftime("%Y-%m-%dT%H:%M:%S"), "responses": {}}
    for cmd in READ_CMDS:
        r = await s.v1(cmd, timeout=15)
        ok = bool(r.obj.get("ok"))
        out["responses"][cmd] = {"ok": ok, "obj": sanitize(r.obj),
                                 "raw_len": len(r.raw)}
        if not ok:
            raise RuntimeError(f"{tag} {cmd} 返回 ok=false")
    return out


def compare(rounds, consoles):
    """六维逐项比对；返回 (ok, dims[])。"""
    dims = []
    r1, r2 = rounds
    c1, c2 = consoles

    def add(name, ok, detail):
        dims.append({"dim": name, "ok": bool(ok), "detail": detail})

    # 1 BLE 地址（scan 已过滤，这里是会话实际地址）
    add("1_ble_addr", True, ADDR)

    def dev_id(r):
        d = (r["responses"]["device.info"]["obj"].get("data") or {})
        return d.get("device_id")

    def build(r):
        d = (r["responses"]["device.info"]["obj"].get("data") or {})
        return d.get("build_sha")

    id1, id2 = dev_id(r1), dev_id(r2)
    add("2_device_id", id1 == id2 == EXPECT_DEVICE_ID, f"{id1} / {id2}")

    b1, b2 = build(r1), build(r2)
    add("3_build_sha",
        b1 == b2 == EXPECT_BUILD_SHA and c1.get("elf_sha") == c2.get("elf_sha") == EXPECT_BUILD_SHA,
        f"rounds={b1}/{b2} console={c1.get('elf_sha')}/{c2.get('elf_sha')}")

    add("4_port_mapping",
        c1.get("client_id") == c2.get("client_id") == EXPECT_CLIENT_ID,
        f"client_id={c1.get('client_id')}@{c1.get('client_id_line')} "
        f"/ {c2.get('client_id')}@{c2.get('client_id_line')}")

    same_session = (c1.get("boot_marker_line") == c2.get("boot_marker_line")
                    and c1.get("boot_uptime_ms") == c2.get("boot_uptime_ms"))
    du = (c2.get("uptime_ms") or 0) - (c1.get("uptime_ms") or 0)
    dw = (c2.get("uptime_ms") is not None and c1.get("uptime_ms") is not None
          and c2["at_unix"] - c1["at_unix"] is not None)
    dwall = c2["at_unix"] - c1["at_unix"]
    add("5_boot_session",
        same_session and abs(du / 1000.0 - dwall) <= 3.0,
        f"boot_line={c1.get('boot_marker_line')}/{c2.get('boot_marker_line')} "
        f"Δu={du/1000.0:.1f}s Δwall={dwall}s")

    def net(r):
        d = (r["responses"]["network.status"]["obj"].get("data") or {})
        return (bool(d.get("mqtt_connected")), d.get("connected_ssid"),
                (d.get("sta_ip") or d.get("ip")))

    n1, n2 = net(r1), net(r2)
    add("6_network_mqtt", n1 == n2 and n1[0] is True and n1[1] == EXPECT_SSID,
        f"{n1} / {n2}")
    return (all(d["ok"] for d in dims), dims)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--console-log", required=True,
                    help="现有 console 日志（只读；不触碰串口采集）")
    ap.add_argument("--out-dir", default=None)
    ap.add_argument("--gap-s", type=float, default=20.0)
    args = ap.parse_args()

    if args.gap_s < 20.0:
        print("[FAIL] --gap-s 必须 ≥20s（授权要求）")
        return 2

    out_dir = Path(args.out_dir or (
        "artifacts/s6_identity_20261115/run_%s_%s"
        % (time.strftime("%Y%m%d_%H%M%S"), uuid.uuid4().hex[:6])))
    out_dir.mkdir(parents=True, exist_ok=False)
    print(f"[identity] out-dir = {out_dir}")

    recs = await scan_filtered(10.0)
    hit = [r for r in recs if r["address"].lower() == ADDR.lower()]
    if not hit:
        print("[STOP] 65D8 不在广播中——按授权要求停止本轮，不做修复尝试")
        return 3

    # 只建立**一次**连接（不做"先连再 _connect_once 重连"的循环）
    s = Session(None, address=ADDR)
    await s._connect_once()          # 一次 connect + 一次 auth.login（短时 RAM token）

    try:
        try:
            # 前置安全条件：trial 必须 idle；非 idle 时断开可能触发清理，直接停
            pre = await s.v1("network.status", timeout=15)
            pre_data = pre.obj.get("data") or {}
            trial = (pre_data.get("trial") or {}).get("state")
            if trial != "idle":
                print(f"[STOP] 前置检查 trial.state={trial!r} != idle —— 不进入采集")
                return 4

            c1 = console_snapshot(args.console_log)
            r1 = await read_round(s, "round1")
            print(f"[identity] round1 @ {r1['at']} console_uptime={c1.get('uptime_ms')}")
            await asyncio.sleep(args.gap_s)
            c2 = console_snapshot(args.console_log)
            r2 = await read_round(s, "round2")
            print(f"[identity] round2 @ {r2['at']} console_uptime={c2.get('uptime_ms')}")
            post = await s.v1("network.status", timeout=15)
            post_trial = ((post.obj.get("data") or {}).get("trial") or {}).get("state")
        except Exception as exc:  # noqa: BLE001
            print(f"[STOP] 采集失败：{type(exc).__name__}: {str(exc)[:120]}"
                  "——按授权要求停止，不做修复尝试")
            return 5
    finally:
        await s.close()              # 一次断开（回调可能清理 BLE 所属 trial）

    measured = r2["at_unix"] - r1["at_unix"]
    ok, dims = compare([r1, r2], [c1, c2])
    if measured < 20:
        ok = False
        dims.append({"dim": "0_gap", "ok": False,
                     "detail": f"实测间隔 {measured}s < 20s"})
    else:
        dims.insert(0, {"dim": "0_gap", "ok": True, "detail": f"实测 {measured}s"})

    ev = {"version": 2, "address": ADDR,
          "scope": "受限采集（见 session_side_effects；不是无副作用审计）",
          "cmds": list(READ_CMDS), "configured_gap_s": args.gap_s,
          "measured_gap_s": measured,
          "session_side_effects": {
              "connect_count": 1, "disconnect_count": 1, "auth_login": True,
              "note": "auth.login 写入短时 RAM token；device.status/network.status "
                      "在 trial 过期时可能触发 trial 清理；断开回调可能清理 BLE "
                      "所属 trial 与可取消的异步网络操作",
          },
          "precheck_trial_state": trial,
          "postcheck_trial_state": post_trial,
          "rounds": [r1, r2], "console": [c1, c2], "dims": dims, "ok": ok}
    (out_dir / "identity_capture.json").write_text(
        json.dumps(ev, ensure_ascii=False, indent=1), encoding="utf-8")

    print("\n=== 六维逐项 ===")
    for d in dims:
        print(f"  [{'ok' if d['ok'] else 'FAIL'}] {d['dim']}: {d['detail']}")
    print(f"\n[{'OK' if ok else 'FAIL'}] 证据：{out_dir / 'identity_capture.json'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
