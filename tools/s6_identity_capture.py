#!/usr/bin/env python3
"""S6 阶段 B · 65D8 六维身份 ×2（**只读**采集，需单独授权窗口）。

授权范围（2026-11-15 窄窗口）：
  - BLE 扫描/连接/读取；命令仅 device.info / device.status / network.status / config.get；
  - 同一 BLE 会话两轮，间隔 ≥20s；
  - console 只**读取现有日志文件**（不暂停、不重启、不重开串口采集）；
  - 不做任何配置写入、网络扫描/连接/断开、MQTT 发布、diag 注入、复位、烧录。

输出：`<out-dir>/identity_capture.json`
  - 两轮原始响应帧（sanitize 后）+ 时间戳
  - 两轮 console 快照（文件大小/行数/最后 uptime/行号、client_id、boot 标记、ELF SHA）
  - 六维逐项比对结果

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

from bleak import BleakClient  # noqa: E402

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
    """一轮只读命令；异常立即上抛（授权要求：失败即停）。"""
    out = {"tag": tag, "at_unix": int(time.time()),
           "at": time.strftime("%Y-%m-%dT%H:%M:%S"), "responses": {}}
    for cmd in READ_CMDS:
        r = await s.v1(cmd, timeout=15)
        out["responses"][cmd] = {
            "ok": bool(r.obj.get("ok")),
            "obj": sanitize(r.obj),
            "raw_len": len(r.raw),
        }
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

    cli = BleakClient(ADDR, timeout=25)
    await cli.connect()
    s = Session(cli, address=ADDR)
    await s._connect_once()          # login（读取类命令的前置鉴权）

    try:
        c1 = console_snapshot(args.console_log)
        r1 = await read_round(s, "round1")
        print(f"[identity] round1 @ {r1['at']} console_uptime={c1.get('uptime_ms')}")
        await asyncio.sleep(args.gap_s)
        c2 = console_snapshot(args.console_log)
        r2 = await read_round(s, "round2")
        print(f"[identity] round2 @ {r2['at']} console_uptime={c2.get('uptime_ms')}")
    finally:
        try:
            await s.close()
        except Exception:  # noqa: BLE001
            pass
        try:
            await cli.disconnect()
        except Exception:  # noqa: BLE001
            pass

    ok, dims = compare([r1, r2], [c1, c2])
    ev = {"version": 1, "address": ADDR, "scope": "read-only identity capture",
          "cmds": list(READ_CMDS), "gap_s": args.gap_s,
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
