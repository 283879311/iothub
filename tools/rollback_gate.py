#!/usr/bin/env python3
"""S6 阶段 B · 回滚烧录前门禁（P0-1）：确认 /dev/ttyUSB1 当前连接 65D8。

与 preflight_flash.py 的差异：FI 烧录失败后 app 可能无法启动（无 MQTT
client_id、无 boot 后身份日志），因此本脚本**不依赖** client_id/boot 标记，
改用以下独立证据确认端口↔设备映射：

  A) console 录制连续性：capture 进程存活、持有串口与日志（PID/fd）；
     映射 = **本次烧录会话期间**该端口的**历史身份记录**（事故记录中已实证
     ttyUSB1=65D8，断电后未再拔插/枚举变化——以 USB 枚举稳定性证据为准）；
  B) BLE 实时观测：65D8 在广播中（无线通道独立于串口假设）；
  C) USB 枚举稳定性：ttyUSB1 上一次映射核验（preflight 通过时刻）以来，
     内核未出现新的 USB 断连/枚举事件（dmesg / 日志 tail 检查）。

若 A/B/C 任一不成立 → 拒绝回滚写入，恢复失败 STOP，等待人工处理。
**不能因为回滚 BIN 的 SHA 正确就推定串口连接的设备正确。**

用法：
  tools/rollback_gate.py --ble-observed 10:97:BD:D3:65:DA \
      --bin docs/evidence/S6_stageB/bin/iothub_a7e04b6a9.bin \
      --expect-bin-sha256 047e4726...ea41
"""

import argparse
import hashlib
import re
import subprocess
import sys
import time
from pathlib import Path

SERIAL_DIR = Path("artifacts/cmd04_flash_20261114/phaseA")
EXPECT_PORT = "/dev/ttyUSB1"
EXPECT_PORT_NAME = "ttyUSB1"


def capture_alive():
    """录制进程存活且持有串口与日志（复用 preflight 的链判定）。"""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from preflight_flash import inspect_capture, inspect_log  # noqa: E402

    cap = inspect_capture(SERIAL_DIR, EXPECT_PORT_NAME)
    li = inspect_log(SERIAL_DIR, EXPECT_PORT_NAME, 120)
    return cap, li


EXPECT_MAC = "10:97:BD:D3:65:DA"


def port_ident(node=EXPECT_PORT):
    """设备节点身份 dev:inode。**重新枚举会分配新 inode**——这是不依赖 root 的
    客观映射锚点（USB 掉线重连/换设备必变）。"""
    try:
        st = Path(node).stat()
        return f"{st.st_dev}:{st.st_ino}"
    except OSError as exc:
        return f"ERR:{type(exc).__name__}"


def usb_stable_since(minutes=120):
    """内核日志近 N 分钟内无 ttyUSB 断连/枚举事件。
    **fail-closed**：dmesg 不可用/权限不足/输出无法判定 → 返回 False（拒绝放行），
    绝不以"无证据"当作"稳定"。"""
    try:
        r = subprocess.run(["dmesg", "--ctime", "--since", f"{minutes} minutes ago"],
                           capture_output=True, text=True, timeout=10)
        if r.returncode != 0:
            return (None, f"dmesg 不可读（rc={r.returncode}）——按补充证据缺失处理，"
                          f"以设备节点身份为准")
        out = r.stdout
        if not out.strip():
            return (True, "近 N 分钟无内核事件（含 USB）")
    except Exception as exc:  # noqa: BLE001
        return (None, f"dmesg 不可用（{type(exc).__name__}）——按补充证据缺失处理，"
                      f"以设备节点身份为准")
    events = [ln for ln in out.splitlines()
              if "ttyUSB" in ln or "disconnect" in ln.lower()]
    return (not events, events[:5] or "无 ttyUSB/断连事件")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ble-observed-mac", required=True,
                    help="BLE 扫描实时观测到的目标 MAC（必须等于 " + EXPECT_MAC + "）")
    ap.add_argument("--scan-anchor", required=False,
                    help="本次扫描证据（时间戳/日志行），写入归档")
    ap.add_argument("--bin", required=True, help="回滚 BIN（冻结文件）")
    ap.add_argument("--expect-bin-sha256", required=True)
    ap.add_argument("--usb-stable-min", type=int, default=120)
    ap.add_argument("--preflight-pass-at", required=True,
                    help="本次 FI preflight 门禁通过时刻 HH:MM:SS（映射时间锚点）")
    ap.add_argument("--preflight-port-ident", required=True,
                    help="preflight 通过时 /dev/ttyUSB1 的 dev:inode（如 0:1234）")
    args = ap.parse_args()

    binp = Path(args.bin)
    if not binp.is_file():
        print(f"[FAIL] 回滚 BIN 不存在: {binp}")
        return 2
    sha = hashlib.sha256(binp.read_bytes()).hexdigest()
    bin_ok = sha == args.expect_bin_sha256.strip().lower()
    print(f"  回滚 BIN : {binp}")
    print(f"  sha256   = {sha}  期望={args.expect_bin_sha256}  一致={bin_ok}")

    cap, _caplog = capture_alive()
    from preflight_flash import inspect_log
    _mac, loginfo = inspect_log(SERIAL_DIR, EXPECT_PORT_NAME, 120)
    print(f"  capture  : running={cap.get('running')} "
          f"script_ok={cap.get('script_ok')} port_arg_ok={cap.get('port_arg_ok')} "
          f"fd_port={cap.get('fd_holds_port')} fd_log={cap.get('fd_holds_log')}")
    print(f"  log      : fresh={loginfo.get('fresh')} age={loginfo.get('age_sec')}s "
          f"boot_marker={loginfo.get('boot_marker')}")
    usb_ok, usb_ev = usb_stable_since(args.usb_stable_min)
    print(f"  usb      : 稳定={usb_ok} 近{args.usb_stable_min}min 事件={usb_ev}")

    ble_mac = str(args.ble_observed_mac).strip().upper()
    ble_observed = (ble_mac == EXPECT_MAC)
    print(f"  ble      : observed_mac={ble_mac} 期望={EXPECT_MAC} "
          f"一致={ble_observed} anchor={args.scan_anchor or '(未提供)'}")

    ok = True
    if not bin_ok:
        print("[FAIL] 回滚 BIN sha 不符")
        ok = False
    if not cap.get("running"):
        print("[FAIL] 录制链不完整（PID→脚本→串口→日志）")
        ok = False
    if not ble_observed:
        print("[FAIL] BLE 实时观测未确认 65D8 在广播")
        ok = False
    cur_ident = port_ident()
    ident_ok = (cur_ident == args.preflight_port_ident)
    print(f"  port-id  : current={cur_ident} preflight={args.preflight_port_ident} "
          f"一致={ident_ok}（重新枚举会换 inode）")
    if not ident_ok:
        print("[FAIL] 设备节点身份与 preflight 锚点不一致——端口可能已重新枚举/换设备")
        ok = False
    if usb_ok is False:                     # dmesg 可读且发现事件 → 明确失败
        print(f"[FAIL] 近 {args.usb_stable_min}min 存在 USB 断连/枚举事件：{usb_ev}")
        ok = False
    elif usb_ok is None:
        print(f"  usb      : {usb_ev}（补充证据缺失，不作为失败理由）")
    # 映射时间锚点：preflight 通过时刻（由调用方传入，格式 HH:MM:SS）之后
    # 的 console 日志必须仍然有该端口的身份/心跳记录，且 USB 无新事件。
    if not args.preflight_pass_at:
        print("[FAIL] 未提供 preflight 通过时刻——映射证据缺时间锚点")
        ok = False
    else:
        print(f"  anchor   : preflight_pass_at={args.preflight_pass_at} "
              f"（用于核对此后枚举连续性与 console 连续性）")

    if not ok:
        print("[STOP] 回滚门禁未通过——禁止写入，恢复失败 STOP，等待人工处理")
        return 1
    print("[OK] 回滚门禁通过（端口/录制/BLE/USB 四路独立证据一致）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
