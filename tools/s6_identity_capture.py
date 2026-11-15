#!/usr/bin/env python3
"""S6 阶段 B · 65D8 六维身份 ×2（**受限**采集，需单独授权窗口）。

**本采集不是"无副作用审计"**，会话本身包含可能改变 RAM 状态的动作：
  - `auth.login`：校验口令并写入**短时 RAM token**（不写持久配置）；
  - `device.status` / `network.status`：复用 `app_cmd_build_network_status()`，
    trial 恰好过期时会调用 `app_cmd_net_trial_clear()`（清理 RAM trial，
    必要时结束配置事务）；
  - 断开连接：`app_ble_cmd_on_disconnect()` 可能取消 BLE 所属可取消异步操作、
    清理 BLE 所属 trial。

因此本工具要求固件提供**真正纯读取的状态快照**命令（`diag.snapshot`，契约见
`SNAPSHOT_CONTRACT`），用于在**断开前**确认：trial 处于 idle 且不存在 BLE
所属活动异步操作。当前固件（`a7e04b6a9`）**没有**该命令，`SNAPSHOT_SUPPORTED_BUILDS`
为空集合，工具在连接设备前即拒绝运行（rc=6）——这是如实反映能力边界，不是缺陷。

会话约束：
  - **只建立一次连接/一次断开**，连接计数由实际调用点递增；
  - 所有命令 `retries=0`、`allow_reconnect=False`、`allow_relogin=False`：
    链路异常直接失败，**绝不自动重连**；
  - 任一响应 `ok is not True` → 立即停止。

输出：`<out-dir>/identity_capture.json`（脱敏后的解析响应对象 + raw_len，不保存原始字节）。

用法：
  .venv-bleak/bin/python tools/s6_identity_capture.py \
      --console-log artifacts/cmd04_flash_20261114/phaseA/serial_ttyUSB1.log
"""

import argparse
import asyncio
import json
import math
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

# 纯读取快照命令（固件需新增；不得触发 trial/operation 清理）
SNAPSHOT_CMD = "diag.snapshot"
SNAPSHOT_CONTRACT = {
    "cmd": SNAPSHOT_CMD,
    "required_data": {
        "trial": {"state": "idle|...（原始状态，不清算）"},
        "ble_owned_active_operation": "false | {id, owner, state}（BLE 所属在飞操作）",
    },
    "规则": "构建响应时不得调用 app_cmd_net_trial_ready()/trial_clear()；只读原始字段",
}
# 支持该命令的 build_sha 白名单——固件尚未实现，故为空：工具拒绝运行
SNAPSHOT_SUPPORTED_BUILDS = frozenset()


class SnapshotError(RuntimeError):
    pass


class ReadOnlySession:
    """单次会话：禁用自动重连/重试（含 auth.login），真实计数连接与断开。"""

    def __init__(self, session):
        self.s = session
        self.connect_count = 0
        self.disconnect_count = 0

    async def open(self):
        # login_retries=0：auth.login 也不得重试（仅本工具使用该入口）
        try:
            await self.s._connect_once(login_retries=0)
        finally:
            # 物理连接成功即计数（即使随后的 login 失败也已建立过连接）
            if getattr(self.s, "cli", None) is not None:
                self.connect_count += 1

    async def close(self):
        if getattr(self.s, "cli", None) is not None:
            await self.s.close()
            self.disconnect_count += 1

    async def read(self, cmd):
        req = {"id": f"{cmd}-{self.s.next_seq()}", "version": "1.0",
               "cmd": cmd, "args": {}}
        if getattr(self.s, "token", None):
            req["token"] = self.s.token
        # 绝不重试、绝不重连：异常立即上抛
        return await self.s.send_request(req, timeout=15, retries=0,
                                        allow_relogin=False,
                                        allow_reconnect=False)


def finalize_counts(ev, rs):
    """在 `close()` **之后**把真实计数写回证据。

    `run_capture()` 返回时尚未断开，若不回写，JSON 里的 disconnect_count 恒为 0。
    语义：connect_count = 物理连接成功次数；disconnect_count = `close()` 调用次数
    （底层 `Session.close()` 吞掉 `cli.disconnect()` 异常，**不证明物理断开成功**）。
    """
    side = ev.setdefault("session_side_effects", {})
    side["connect_count"] = rs.connect_count
    side["disconnect_count"] = rs.disconnect_count
    side["counts_semantics"] = (
        "connect_count=物理连接成功次数；disconnect_count=close() 调用次数"
        "（底层吞异常，不代表物理断开成功）")
    return ev


def console_snapshot(log_path):
    """只读快照：不打开串口，不触碰采集进程。"""
    p = Path(log_path)
    st = p.stat()
    data = p.read_bytes()
    text = data.decode("utf-8", "replace")
    lines = text.splitlines()

    def line_of(idx):
        return text.count("\n", 0, idx) + 1

    snap = {"path": str(p), "size": st.st_size, "mtime_unix": st.st_mtime,
            "total_lines": len(lines), "at_unix": int(time.time())}

    ups = list(re.finditer(r"I \((\d+)\)", text))
    if ups:
        m = ups[-1]
        snap["uptime_ms"] = int(m.group(1))
        snap["uptime_line"] = line_of(m.start())
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


async def read_round(rs, tag):
    """一轮读取；任一命令 ok is not True 或异常立即上抛。"""
    out = {"tag": tag, "at_unix": int(time.time()),
           "at": time.strftime("%Y-%m-%dT%H:%M:%S"), "responses": {}}
    for cmd in READ_CMDS:
        r = await rs.read(cmd)
        ok = r.obj.get("ok") is True
        out["responses"][cmd] = {"ok": ok, "obj": sanitize(r.obj),
                                 "raw_len": len(r.raw)}
        if not ok:
            raise RuntimeError(f"{tag} {cmd} 返回 ok!=true")
    return out


async def read_snapshot(rs):
    """读取纯读取快照；固件不支持或字段不合规 → SnapshotError。"""
    try:
        r = await rs.read(SNAPSHOT_CMD)
    except Exception as exc:  # noqa: BLE001
        raise SnapshotError(f"快照命令不可用：{type(exc).__name__}: {str(exc)[:80]}")
    if r.obj.get("ok") is not True:
        raise SnapshotError(f"快照命令 ok!=true（{r.obj.get('error')}）")
    return r.obj.get("data") or {}


def snapshot_clean(data):
    """断开前安全条件（**fail-closed**）：trial 为 idle 且字段明确为 false。

    契约要求 `ble_owned_active_operation` **必须存在且严格为 false**：
    缺失 / null / {} / "" / "none" / 其它类型 一律视为快照无效，不得当作"无活动操作"。
    """
    if not isinstance(data, dict):
        return (False, "快照 data 不是对象")
    trial = data.get("trial")
    if not isinstance(trial, dict) or trial.get("state") != "idle":
        return (False, f"trial.state={trial!r} != idle")
    if "ble_owned_active_operation" not in data:
        return (False, "缺少 ble_owned_active_operation 字段")
    active = data.get("ble_owned_active_operation")
    if active is not False:
        return (False, f"ble_owned_active_operation 非 false：{active!r}")
    return (True, "clean")


async def run_capture(rs, console_log, gap_s, expect_build=EXPECT_BUILD_SHA,
                      sleep_fn=None):
    """执行两轮采集（调用方负责连接/断开与计数）。返回 (ev, ok)。

    `sleep_fn` 仅供离线测试注入（生产用 asyncio.sleep）。
    """
    sleep_fn = sleep_fn or asyncio.sleep
    if not math.isfinite(gap_s) or gap_s < 20.0:
        raise ValueError("gap_s 必须为有限数值且 ≥20s")

    # 白名单成员校验：必须在使用快照结果作为安全依据**之前**完成
    info = await rs.read("device.info")
    if info.obj.get("ok") is not True:
        raise RuntimeError("device.info 返回 ok!=true")
    build = (info.obj.get("data") or {}).get("build_sha")
    if build not in SNAPSHOT_SUPPORTED_BUILDS:
        raise SnapshotError(
            f"build_sha={build!r} 不在快照白名单 {sorted(SNAPSHOT_SUPPORTED_BUILDS)}")

    pre = await read_snapshot(rs)          # 前置：断开安全条件
    clean, why = snapshot_clean(pre)
    if not clean:
        raise SnapshotError(f"前置快照不干净：{why}")

    c1 = console_snapshot(console_log)
    r1 = await read_round(rs, "round1")
    await sleep_fn(gap_s)
    c2 = console_snapshot(console_log)
    r2 = await read_round(rs, "round2")

    post = await read_snapshot(rs)         # 后置：断开前再确认
    clean2, why2 = snapshot_clean(post)
    if not clean2:
        raise SnapshotError(f"后置快照不干净：{why2}")

    measured = r2["at_unix"] - r1["at_unix"]
    ok, dims = compare([r1, r2], [c1, c2], expect_build)
    dims.insert(0, {"dim": "0_gap",
                    "ok": measured >= 20,
                    "detail": f"实测 {measured}s（要求 ≥20s）"})
    ok = ok and measured >= 20
    ev = {"version": 3, "address": ADDR,
          "scope": "受限采集（见 session_side_effects；不是无副作用审计）",
          "cmds": list(READ_CMDS), "snapshot_cmd": SNAPSHOT_CMD,
          "configured_gap_s": gap_s, "measured_gap_s": measured,
          "session_side_effects": {
              "connect_count": rs.connect_count,
              "disconnect_count": rs.disconnect_count,
              "auth_login": True,
              "note": "auth.login 写入短时 RAM token；device.status/network.status "
                      "在 trial 过期时可能触发 trial 清理；断开回调可能清理 BLE "
                      "所属 trial 与可取消的异步网络操作",
          },
          "precheck_snapshot": pre,
          "postcheck_snapshot": post,
          "rounds": [r1, r2], "console": [c1, c2], "dims": dims, "ok": ok}
    return (ev, ok)


def compare(rounds, consoles, expect_build=EXPECT_BUILD_SHA):
    """六维逐项比对；返回 (ok, dims[])。"""
    dims = []
    r1, r2 = rounds
    c1, c2 = consoles

    def add(name, ok, detail):
        dims.append({"dim": name, "ok": bool(ok), "detail": detail})

    def dev_id(r):
        d = (r["responses"]["device.info"]["obj"].get("data") or {})
        return d.get("device_id")

    def build(r):
        d = (r["responses"]["device.info"]["obj"].get("data") or {})
        return d.get("build_sha")

    def net(r):
        d = (r["responses"]["network.status"]["obj"].get("data") or {})
        return (d.get("mqtt_connected") is True, d.get("connected_ssid"),
                d.get("sta_ip"))

    add("1_ble_addr", True, ADDR)
    id1, id2 = dev_id(r1), dev_id(r2)
    add("2_device_id", id1 == id2 == EXPECT_DEVICE_ID, f"{id1} / {id2}")
    b1, b2 = build(r1), build(r2)
    add("3_build_sha",
        b1 == b2 == expect_build
        and c1.get("elf_sha") == c2.get("elf_sha") == expect_build,
        f"rounds={b1}/{b2} console={c1.get('elf_sha')}/{c2.get('elf_sha')}")
    add("4_port_mapping",
        c1.get("client_id") == c2.get("client_id") == EXPECT_CLIENT_ID,
        f"client_id={c1.get('client_id')}@{c1.get('client_id_line')} "
        f"/ {c2.get('client_id')}@{c2.get('client_id_line')}")
    same_session = (c1.get("boot_marker_line") == c2.get("boot_marker_line")
                    and c1.get("boot_uptime_ms") == c2.get("boot_uptime_ms"))
    du = (c2.get("uptime_ms") or 0) - (c1.get("uptime_ms") or 0)
    dwall = c2["at_unix"] - c1["at_unix"]
    add("5_boot_session",
        same_session and abs(du / 1000.0 - dwall) <= 3.0,
        f"boot_line={c1.get('boot_marker_line')}/{c2.get('boot_marker_line')} "
        f"Δu={du/1000.0:.1f}s Δwall={dwall}s")
    n1, n2 = net(r1), net(r2)
    add("6_network_mqtt",
        n1 == n2 and n1[0] is True and n1[1] == EXPECT_SSID
        and isinstance(n1[2], str) and bool(n1[2]),
        f"{n1} / {n2}")
    return (all(d["ok"] for d in dims), dims)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--console-log", required=True,
                    help="现有 console 日志（只读；不触碰串口采集）")
    ap.add_argument("--out-dir", default=None)
    ap.add_argument("--gap-s", type=float, default=20.0)
    args = ap.parse_args()

    if not math.isfinite(args.gap_s) or args.gap_s < 20.0:
        print("[FAIL] --gap-s 必须为有限数值且 ≥20s（授权要求）")
        return 2

    if not SNAPSHOT_SUPPORTED_BUILDS:
        print("[STOP] 当前固件无纯读取快照命令 %r，无法在断开前证明无副作用。" % SNAPSHOT_CMD)
        print("       按授权边界：**不连接设备、不采集**。需固件实现该命令后重新授权窗口。")
        print("       契约：%s" % json.dumps(SNAPSHOT_CONTRACT, ensure_ascii=False))
        return 6

    out_dir = Path(args.out_dir or (
        "artifacts/s6_identity_20261115/run_%s_%s"
        % (time.strftime("%Y%m%d_%H%M%S"), uuid.uuid4().hex[:6])))
    out_dir.mkdir(parents=True, exist_ok=False)
    print(f"[identity] out-dir = {out_dir}")

    recs = await scan_filtered(10.0)
    if not [r for r in recs if r["address"].lower() == ADDR.lower()]:
        print("[STOP] 65D8 不在广播中——按授权要求停止本轮，不做修复尝试")
        return 3

    rs = ReadOnlySession(Session(None, address=ADDR))
    ev = None
    ok = False
    try:
        await rs.open()
        ev, ok = await run_capture(rs, args.console_log, args.gap_s)
    except SnapshotError as exc:
        print(f"[STOP] 快照条件不满足：{exc}——不做修复尝试")
        return 7
    except Exception as exc:  # noqa: BLE001
        print(f"[STOP] 采集失败：{type(exc).__name__}: {str(exc)[:120]}")
        return 5
    finally:
        await rs.close()
        # 计数必须在 close() **之后**写回证据：run_capture 返回时尚未断开
        if ev is not None:
            finalize_counts(ev, rs)
        print(f"[identity] connect_count={rs.connect_count} "
              f"disconnect_count={rs.disconnect_count}")

    (out_dir / "identity_capture.json").write_text(
        json.dumps(ev, ensure_ascii=False, indent=1), encoding="utf-8")
    print("\n=== 六维逐项 ===")
    for d in ev["dims"]:
        print(f"  [{'ok' if d['ok'] else 'FAIL'}] {d['dim']}: {d['detail']}")
    print(f"\n[{'OK' if ok else 'FAIL'}] 证据：{out_dir / 'identity_capture.json'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
