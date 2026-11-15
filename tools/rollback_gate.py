#!/usr/bin/env python3
"""S6 阶段 B · 回滚烧录门禁 v2：以芯片 MAC 为板卡身份（独立于 app）。

证据链（全部参与放行判定，缺一即拒绝）：
  1. 板卡身份（主证据）：`esptool read_mac` 读 eFuse MAC——即使 FI 固件无法启动
     （下载模式）仍可核验；必须等于 preflight 锚点 MAC 且等于 65D8 的
     `10:97:bd:d3:65:d8`。**不使用** dev:inode / st_rdev / by-id 作身份
     （inode 可复用；本机 CH340 无序列号）。
  2. 锚点：preflight 产物，校验存在/可解析/新鲜度/端口/日志目录一致。
  3. 录制链：script_ok / port_arg_ok / log_arg_ok / fd_holds_port /
     fd_holds_log 逐项必须 True。
  4. 日志：fresh 且 boot_marker。
  5. dmesg（补充）：可读且发现 ttyUSB 事件 → 拒绝；不可读 → 仅记录。
  6. 回滚 BIN：完整 SHA-256 等于授权值。

read_mac 会令芯片停在下载模式；本门禁不写 Flash，随后由烧录（自带复位）
或 `esptool run` 回到 app。

用法：
  tools/rollback_gate.py --port /dev/ttyUSB1 --log-dir <本轮目录> \
      --anchor-file <preflight 锚点.json> \
      --bin <回滚BIN> --expect-bin-sha256 047e...
"""

import argparse
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import port_identity as PI  # noqa: E402
from preflight_flash import inspect_capture, inspect_log  # noqa: E402

EXPECT_MAC = PI.EXPECT_MAC_65D8


def usb_events_since(minutes):
    try:
        r = subprocess.run(["dmesg", "--ctime", "--since", f"{minutes} minutes ago"],
                           capture_output=True, text=True, timeout=10)
        if r.returncode != 0:
            return (None, f"dmesg 不可读（rc={r.returncode}）")
        ev = [ln for ln in r.stdout.splitlines()
              if "ttyUSB" in ln or "disconnect" in ln.lower()]
        return (not ev, ev[:5] or "无 ttyUSB/断连事件")
    except Exception as exc:  # noqa: BLE001
        return (None, f"dmesg 不可用（{type(exc).__name__}）")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--log-dir", required=True)
    ap.add_argument("--anchor-file", required=True)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--expect-bin-sha256", required=True)
    ap.add_argument("--max-anchor-age-min", type=int, default=240)
    ap.add_argument("--usb-events-min", type=int, default=240)
    ap.add_argument("--evidence-dir", required=True,
                    help="本轮证据目录（恢复流程/门禁证据写入此处；写失败即失败）")
    ap.add_argument("--skip-restore", action="store_true",
                    help="不等待新 boot 证据（仅供紧接烧录的调用点；仍会 run+重启录制）")
    ap.add_argument("--capture-ctl", default="tools/serial_capture_ctl.sh",
                    help="串口录制控制脚本（停录制以读取 MAC）")
    args = ap.parse_args()

    fails = []
    # 阈值固定：不接受放宽（只允许更严）
    if args.max_anchor_age_min > 240 or args.usb_events_min > 240:
        print("[FAIL] 门禁阈值不得放宽（max-anchor-age-min / usb-events-min ≤ 240）")
        return 2

    def check(cond, msg):
        print(("  [ok]   " if cond else "  [FAIL] ") + msg)
        if not cond:
            fails.append(msg)

    # 0. 端口
    check(args.port == "/dev/ttyUSB1", f"端口必须为 /dev/ttyUSB1（{args.port}）")

    # 1. 锚点
    anchor = None
    try:
        anchor = PI.load_anchor(args.anchor_file)
        check(True, f"锚点可解析：{args.anchor_file}")
    except Exception as exc:  # noqa: BLE001
        check(False, f"锚点不可解析（{type(exc).__name__}）")
    if anchor is not None:
        ok_a, reasons = PI.anchor_validate(anchor, args.port, args.log_dir,
                                           args.max_anchor_age_min)
        check(ok_a, "锚点严格校验" if ok_a else f"锚点严格校验失败：{reasons}")
    else:
        anchor = None

    # 1b. 回滚 BIN（提前校验：失败则不再动用串口）
    binp = Path(args.bin)
    if not binp.is_file():
        check(False, f"回滚 BIN 不存在：{binp}")
    else:
        sha = hashlib.sha256(binp.read_bytes()).hexdigest()
        check(sha == args.expect_bin_sha256.strip().lower(),
              f"回滚 BIN sha256（{sha[:16]}…）")

    # 2/3. 录制链 + 日志（此时 capture 仍在运行）
    name = Path(args.port).name
    cap = inspect_capture(args.log_dir, name)
    for k in ("script_ok", "port_arg_ok", "log_arg_ok",
              "fd_holds_port", "fd_holds_log", "running"):
        check(cap.get(k) is True, f"录制链 {k}")
    _m, loginfo = inspect_log(args.log_dir, name, 120)
    check(loginfo.get("fresh") is True, f"日志新鲜 age={loginfo.get('age_sec')}s")
    check(loginfo.get("boot_marker") is True, "日志 boot 标记")

    # 4. 板卡身份（主证据）：统一恢复流程（停录制→read_mac→run→重启录制→新 boot 证据）
    if not fails:
        ok_flow, ev = PI.read_mac_flow(
            args.port, args.log_dir, str(Path(args.evidence_dir) / "mac_record_gate.json"),
            capture_ctl=args.capture_ctl, evidence_dir=args.evidence_dir,
            strict_restore=not args.skip_restore)
        restore_info = ev
        check(ok_flow, "MAC 读取 + 恢复流程（stop→mac→run→start→new boot）")
        if ev.get("mac"):
            check(str(ev["mac"]).lower() == EXPECT_MAC, f"MAC==65D8（{ev['mac']}）")
            if anchor and anchor.get("mac"):
                check(str(ev["mac"]).lower() == str(anchor["mac"]).lower(),
                      f"MAC==锚点（{ev['mac']}）")
    # 5. dmesg 补充
    usb_ok, usb_ev = usb_events_since(args.usb_events_min)
    if usb_ok is False:
        check(False, f"近 {args.usb_events_min}min USB 事件：{usb_ev}")
    else:
        print(f"  [info] dmesg：{usb_ev}（补充，不否决）")

    try:
        Path(args.evidence_dir).mkdir(parents=True, exist_ok=True)
        Path(args.evidence_dir, "rollback_gate_last.json").write_text(
            json.dumps({"restore": restore_info, "fails": fails,
                        "at": time.strftime("%Y-%m-%dT%H:%M:%S")},
                       ensure_ascii=False, indent=1), encoding="utf-8")
    except Exception as exc:  # noqa: BLE001
        check(False, f"门禁证据写入失败（{type(exc).__name__}）")

    print()
    if fails:
        print(f"[STOP] 回滚门禁未通过（{len(fails)} 项）——禁止写入，恢复失败 STOP")
        for f in fails:
            print(f"       · {f}")
        return 1
    print("[OK] 回滚门禁通过（MAC+锚点+录制链+日志+BIN）")
    if args.skip_restore:
        print("     --skip-restore：芯片在下载模式、录制停止——可立即烧录（烧录自带复位），")
        print("     烧录后必须 serial_capture_ctl.sh start <port> <log> 重启录制")
    else:
        print("     已恢复：芯片回到 app、录制已重启（restore 结果见 gate 证据 JSON）")
        print("     若随后要烧录：再停录制（记计划内 gap）→ flash_frozen.sh → 重启录制")
    return 0


if __name__ == "__main__":
    sys.exit(main())
