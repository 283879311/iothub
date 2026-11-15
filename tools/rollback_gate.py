#!/usr/bin/env python3
"""S6 阶段 B · 回滚烧录门禁 v2：以芯片 MAC 为板卡身份（独立于 app）。

证据链（全部参与放行判定，缺一即拒绝）：
  1. 板卡身份（主证据）：`esptool read_mac` 读 eFuse MAC——即使 FI 固件无法启动
     （下载模式）仍可核验；必须等于 preflight 锚点 MAC 且等于 65D8 的
     `10:97:bd:d3:65:d8`。**不使用** dev:inode / st_rdev / by-id 作身份
     （inode 可复用；本机 CH340 无序列号）。
  2. 锚点：preflight 产物，校验存在/可解析/新鲜度/端口/日志目录一致。
  3. 录制链：script_ok / port_arg_ok / log_arg_ok / fd_holds_port /
     fd_holds_log 逐项必须 True。**恢复模式**（`--recovery-mode`）下改为
     仅记录、不否决——FI 可能无法启动时，"app 在跑/日志新鲜/有 boot 输出"
     不能作为回滚前提；串口互斥由端口锁保证。
  4. 日志：fresh 且 boot_marker。
  5. dmesg（补充）：可读且发现 ttyUSB 事件 → 拒绝；不可读 → 仅记录。
  6. 回滚 BIN：完整 SHA-256 等于授权值。
  7. 最终证据：`validate_final_evidence()` 校验 run_id/端口/MAC/端口锁/
     操作者声明/恢复字段/时间新鲜度；证据目录必须全新（拒绝旧产物复用）。

读 MAC 的**两个独立条件**（缺一即拒绝）：
  a) 本门禁按**串口**唯一持有端口锁（`/run/lock/iothub_serial_<ttyUSBx>.lock`），
     证明没有协作采集在运行、且读期间无法启动；
  b) `--attest-no-external-holder <说明>`：操作者声明无外部工具占用该串口。
     软件无法证明这一点（minicom / 手工 cat / 其它 esptool 不遵循本协议），
     故证据里 `port_free_proven` 恒为 false，校验端强制要求它为 false。

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
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import port_identity as PI  # noqa: E402
from preflight_flash import inspect_capture, inspect_log  # noqa: E402

EXPECT_MAC = PI.EXPECT_MAC_65D8

CHAIN_KEYS = ("script_ok", "port_arg_ok", "log_arg_ok",
              "fd_holds_port", "fd_holds_log", "running")


def evaluate_preflight(cap, loginfo, recovery_mode=False):
    """录制链/日志的放行判定；返回 (hard_fails, soft_notes, passes)。

    - 正常模式：任一不满足 → hard（禁止继续）；
    - **恢复模式**（FI 可能无法启动）：不满足项只转 soft 记录，不否决。
      理由：`app 必须能启动 / 日志必须新鲜 / 必须有 boot 输出` 不能作为
      "回滚恢复"的前提——否则真需要恢复时会被 app 日志门禁挡住。
      串口互斥改由 `read_mac_flow()` 内**强制持有端口锁**保证（协作采集
      在运行则取不到锁 → 拒绝读 MAC）。
    """
    items = [(f"录制链 {k}", cap.get(k) is True) for k in CHAIN_KEYS]
    items.append((f"日志新鲜 age={loginfo.get('age_sec')}s",
                  loginfo.get("fresh") is True))
    items.append(("日志 boot 标记", loginfo.get("boot_marker") is True))
    passes = [n for n, ok in items if ok]
    bad = [n for n, ok in items if not ok]
    if recovery_mode:
        return ([], bad, passes)
    return (bad, [], passes)


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
    ap.add_argument("--attest-no-external-holder", required=True, metavar="说明",
                    help="操作者声明：该串口无外部工具占用（minicom/手工 cat/"
                         "其它 esptool）。软件无法证明，必须显式声明并写入证据")
    ap.add_argument("--skip-restore", action="store_true",
                    help="不等待新 boot 证据（仅供紧接烧录的调用点；仍会 run+重启录制）")
    ap.add_argument("--recovery-mode", action="store_true",
                    help="恢复模式（FI 可能无法启动）：录制链/日志新鲜度/boot 证据"
                         "只记录不否决，且不等新 boot；仍强制端口/锚点/BIN sha/"
                         "端口锁/操作者声明/证据写入")
    ap.add_argument("--capture-ctl", default="tools/serial_capture_ctl.sh",
                    help="串口录制控制脚本（停录制以读取 MAC）")
    args = ap.parse_args()

    fails = []
    diags = []          # 恢复模式下"仅记录"的软诊断（不参与放行）
    restore_info = {"attempted": False, "result": None}
    # 阈值固定：不接受放宽（只允许更严）
    if args.max_anchor_age_min > 240 or args.usb_events_min > 240:
        print("[FAIL] 门禁阈值不得放宽（max-anchor-age-min / usb-events-min ≤ 240）")
        return 2

    # 0a. 证据目录必须为本轮全新：与 port_identity read 共用同一原子创建函数
    ok_dir, dir_why = PI.prepare_fresh_evidence_dir(args.evidence_dir)
    if not ok_dir:
        for w in dir_why:
            print(f"[FAIL] {w}")
        return 2
    if not args.attest_no_external_holder.strip():
        print("[FAIL] --attest-no-external-holder 不能为空")
        return 2
    print("[info] 边界：协作锁只证明**本仓库采集**是否运行；"
          "串口无外部持有者由操作者声明（软件无法证明，port_free_proven=false）")
    if args.recovery_mode:
        print("[info] 恢复模式：录制链/日志新鲜度/boot 证据**不参与否决**"
              "（FI 可能无法启动）；仍强制端口/锚点/BIN sha/端口锁/操作者声明/证据写入")

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
    try:
        cap = inspect_capture(args.log_dir, name)
    except Exception as exc:  # noqa: BLE001
        msg = f"录制链检查异常（{type(exc).__name__}: {str(exc)[:60]}）"
        if args.recovery_mode:
            print(f"  [info] {msg}（恢复模式：仅记录）")
            diags.append(msg)
        else:
            check(False, msg)
        cap = {}
    try:
        _m, loginfo = inspect_log(args.log_dir, name, 120)
    except Exception as exc:  # noqa: BLE001
        msg = f"日志检查异常（{type(exc).__name__}: {str(exc)[:60]}）"
        if args.recovery_mode:
            print(f"  [info] {msg}（恢复模式：仅记录）")
            diags.append(msg)
        else:
            check(False, msg)
        loginfo = {}
    hard, soft, passes = evaluate_preflight(cap, loginfo, args.recovery_mode)
    for msg in passes:
        check(True, msg)
    for msg in soft:
        print(f"  [info] {msg}（恢复模式：不否决）")
    for msg in hard:
        check(False, msg)

    # 4. 板卡身份（主证据）：统一恢复流程（停录制→持端口锁→read_mac→run→
    #    重启录制→新 boot 证据）。读 MAC 的两个独立条件：端口锁 + 操作者声明。
    if not fails:
        restore_info["attempted"] = True
        run_id = time.strftime("%Y%m%d_%H%M%S") + "_" + uuid.uuid4().hex[:8]
        ok_flow, ev = PI.read_mac_flow(
            args.port, args.log_dir, str(Path(args.evidence_dir) / "mac_record_gate.json"),
            capture_ctl=args.capture_ctl, evidence_dir=args.evidence_dir,
            strict_restore=not (args.skip_restore or args.recovery_mode), run_id=run_id,
            external_holder_attested=True,
            attestation_note=args.attest_no_external_holder.strip(),
            evidence_dir_prepared=True, require_chain=not args.recovery_mode)
        restore_info = ev
        check(ok_flow, "MAC 读取 + 恢复流程（stop→lock→mac→run→start→new boot）")
        # 强制消费端门禁：最终证据必须唯一属于本轮且结论 ok（不以首份快照为准）
        fin_ok, fin_reasons, _fin = PI.validate_final_evidence(
            args.evidence_dir, run_id, args.port, EXPECT_MAC,
            require_restore=True,
            require_new_boot=not (args.skip_restore or args.recovery_mode))
        for why in fin_reasons:
            print(f"  [info] 最终证据：{why}")
        check(fin_ok, f"最终证据唯一属于本轮（run_id={run_id}）")
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
                        "diagnostics": diags, "recovery_mode": args.recovery_mode,
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
        print("     --skip-restore：已执行 run_app + 重启录制，仅跳过新 boot 证据等待；")
    else:
        print("     已恢复：芯片回到 app、录制已重启（restore 结果见 gate 证据 JSON）")
        print("     若随后要烧录：再停录制（记计划内 gap）→ flash_frozen.sh → 重启录制")
    return 0


if __name__ == "__main__":
    sys.exit(main())
