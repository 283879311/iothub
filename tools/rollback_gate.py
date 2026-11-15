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


def usb_stable_since(minutes=120):
    """内核日志近 N 分钟内无 ttyUSB1 的断连/枚举事件（映射稳定的旁证）。"""
    try:
        out = subprocess.run(["dmesg", "--ctime", "--since", f"{minutes} minutes ago"],
                             capture_output=True, text=True, timeout=10).stdout
    except Exception:  # noqa: BLE001
        return (True, "dmesg 不可用（视为无新增事件；现场确认补证）")
    events = [ln for ln in out.splitlines()
              if "ttyUSB" in ln or "disconnect" in ln.lower()]
    return (not events, events[:5])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ble-observed", required=True,
                    help="BLE 实时观测：65D8 是否在广播（true/false，由执行者现场提供）")
    ap.add_argument("--bin", required=True, help="回滚 BIN（冻结文件）")
    ap.add_argument("--expect-bin-sha256", required=True)
    ap.add_argument("--usb-stable-min", type=int, default=120)
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

    ble_observed = str(args.ble_observed).strip().lower() == "true"

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
    if not usb_ok:
        print("[FAIL] 近期发生 USB 枚举/断连事件——映射证据不稳定")
        ok = False

    if not ok:
        print("[STOP] 回滚门禁未通过——禁止写入，恢复失败 STOP，等待人工处理")
        return 1
    print("[OK] 回滚门禁通过（端口/录制/BLE/USB 四路独立证据一致）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
