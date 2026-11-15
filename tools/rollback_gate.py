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
    ap.add_argument("--skip-restore", action="store_true",
                    help="不恢复（仅供烧录前紧邻调用；默认恢复 app+录制）")
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
    if anchor is not None and (not isinstance(anchor, dict) or len(anchor) == 0):
        check(False, "锚点根对象为空或不是字典（拒绝空 {} 绕过字段校验）")
        anchor = None
    if anchor is not None:
        required = {"version": int, "port": str, "log_dir": str,
                    "mac": (str, type(None)), "checked_at_unix": (int, float)}
        miss = [k for k in required if k not in anchor]
        check(not miss, f"锚点字段完整（缺 {miss}）" if miss else "锚点字段完整")
        badtype = [k for k, t in required.items()
                   if k in anchor and not isinstance(anchor[k], t)]
        check(not badtype, f"锚点字段类型正确（异常 {badtype}）" if badtype
              else "锚点字段类型正确")
        try:
            age = time.time() - float(anchor.get("checked_at_unix", 0))
        except Exception:  # noqa: BLE001
            age = -1
        check(0 <= age <= args.max_anchor_age_min * 60,
              f"锚点新鲜度 age={age/60:.1f}min ≤ {args.max_anchor_age_min}min")
        check(anchor.get("port") == args.port, f"锚点端口（{anchor.get('port')}）")
        check(anchor.get("mac_ok") is True and anchor.get("mac"),
              f"锚点含有效 MAC（{anchor.get('mac')}）")
        # 锚点 MAC 本身必须等于期望 65D8（在停录制之前就判，避免无谓动串口）
        check(anchor.get("mac") is not None and
              str(anchor["mac"]).lower() == EXPECT_MAC,
              f"锚点 MAC == 65D8（{anchor.get('mac')} vs {EXPECT_MAC}）")
        check(anchor.get("log_dir") == args.log_dir,
              f"锚点日志目录（{anchor.get('log_dir')}）")

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

    # 4. 板卡身份（主证据）：需端口空闲 → 停录制后 read_mac；**无论结果都恢复**
    log_path = str(Path(args.log_dir) / f"serial_{name}.log")
    restore_info = {"stopped": False, "mac_read": None, "run_app": None,
                    "capture_restarted": None}
    if not fails:
        print(f"  [step] 停录制以读取芯片 MAC：{log_path}")
        r = subprocess.run([args.capture_ctl, "stop", log_path],
                           capture_output=True, text=True)
        print(f"         {r.stdout.strip() or r.stderr.strip()}")
        restore_info["stopped"] = (r.returncode == 0)
        check(restore_info["stopped"], "录制已停止（read_mac 需端口空闲）")
        if restore_info["stopped"]:
            try:
                ok_mac, mac = PI.esptool_read_mac(args.port)
                restore_info["mac_read"] = mac if ok_mac else None
                check(ok_mac, f"esptool read_mac（{mac if ok_mac else str(mac)[:70]}）")
                if ok_mac:
                    check(mac.lower() == EXPECT_MAC, f"MAC==65D8（{mac}）")
                    if anchor and anchor.get("mac"):
                        check(mac.lower() == str(anchor["mac"]).lower(),
                              f"MAC==锚点（{mac} vs {anchor['mac']}）")
            finally:
                # 恢复设备与录制（默认）；--skip-restore 仅用于"紧接着烧录"场景
                if args.skip_restore:
                    restore_info["run_app"] = "skipped(by flag)"
                    restore_info["capture_restarted"] = "skipped(by flag)"
                    print("  [info] --skip-restore：保持下载模式与录制停止（紧接烧录）")
                else:
                    rok, rmsg = PI.esptool_run_app(args.port)
                    restore_info["run_app"] = rok
                    print(f"  [{'ok' if rok else 'FAIL'}] 恢复 app：{str(rmsg)[:70]}")
                    r2 = subprocess.run([args.capture_ctl, "start", args.port, log_path],
                                        capture_output=True, text=True)
                    restore_info["capture_restarted"] = (r2.returncode == 0)
                    print(f"  [{'ok' if r2.returncode == 0 else 'FAIL'}] 重启录制："
                          f"{r2.stdout.strip() or r2.stderr.strip()}")
                    check(rok, "读 MAC 后已恢复 app")
                    check(r2.returncode == 0, "读 MAC 后录制已重启")

    # 5. dmesg 补充
    usb_ok, usb_ev = usb_events_since(args.usb_events_min)
    if usb_ok is False:
        check(False, f"近 {args.usb_events_min}min USB 事件：{usb_ev}")
    else:
        print(f"  [info] dmesg：{usb_ev}（补充，不否决）")

    try:
        Path("artifacts/cmd04_flash_20261115/gate_test").mkdir(parents=True, exist_ok=True)
        Path("artifacts/cmd04_flash_20261115/gate_test/rollback_gate_last.json") \
            .write_text(json.dumps({"restore": restore_info, "fails": fails,
                                    "at": time.strftime("%Y-%m-%dT%H:%M:%S")},
                                   ensure_ascii=False, indent=1), encoding="utf-8")
    except Exception:  # noqa: BLE001
        pass

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
