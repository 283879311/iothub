#!/usr/bin/env python3
"""端口 ↔ 物理板卡身份（S6 阶段 B 证据链用）。

为什么不用 dev:inode / st_rdev / by-id：
  - `st_dev` 是文件系统设备号；`st_ino` 是 inode（可能复用）；两者都不代表板卡；
  - `by-id` 依赖适配器序列号，本机 CH340 无序列号（`usb-1a86_USB_Serial`）。

本模块提供**独立于应用的**板卡身份：`esptool read_mac` 读芯片 eFuse MAC。
即使 app 无法启动（下载模式）也成立，因此可用于"FI 烧坏后回滚前"的核验。

代价：read_mac 会把芯片复位进下载模式，需要 `esptool run` 再回 app
（属于已授权的"复位"操作；在烧录前/回滚前执行属正常时机）。
"""

import json
import re
import subprocess
import sys
import time
from pathlib import Path

EXPECT_MAC_65D8 = "10:97:bd:d3:65:d8"     # 65D8 的 WiFi STA MAC（= client_id 里那 12 位）


def idf_python():
    env = Path("/home/tc/.espressif/python_env/idf6.0_py3.12_env/bin/python")
    return str(env) if env.is_file() else sys.executable


def esptool_read_mac(port, timeout=60):
    """读芯片 MAC（下载模式亦可）。返回 (ok, mac_or_error)。

    注意：会让芯片停在下载模式；调用方需视需要执行 esptool run 回 app。
    """
    cmd = [idf_python(), "-m", "esptool", "--chip", "esp32", "--port", port,
           "--before", "default_reset", "--after", "no_reset", "read_mac"]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except Exception as exc:  # noqa: BLE001
        return (False, f"{type(exc).__name__}: {exc}")
    out = (r.stdout or "") + (r.stderr or "")
    m = re.search(r"MAC:\s*([0-9a-fA-F:]{17})", out)
    if r.returncode != 0 or not m:
        return (False, f"rc={r.returncode} out={out.strip()[-160:]}")
    return (True, m.group(1).lower())


def esptool_run_app(port, timeout=60):
    """让芯片退出下载模式并运行 app。"""
    cmd = [idf_python(), "-m", "esptool", "--chip", "esp32", "--port", port,
           "--before", "default_reset", "--after", "hard_reset", "run"]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return (r.returncode == 0, (r.stdout or r.stderr or "").strip()[-160:])
    except Exception as exc:  # noqa: BLE001
        return (False, f"{type(exc).__name__}: {exc}")


def adapter_id(port):
    """符号链接 by-id 名称（辅助证据；无序列号的适配器只给出芯片型号串）。"""
    base = Path("/dev/serial/by-id")
    name = Path(port).name
    if not base.is_dir():
        return None
    for p in base.iterdir():
        try:
            if Path(p).resolve().name == name:
                return p.name
        except OSError:
            continue
    return None


def build_anchor(port, log_dir, mac=None, expect_mac=EXPECT_MAC_65D8):
    """生成锚点：板卡 MAC（主）+ 适配器名（辅助）+ 录制链 + 日志状态 + 时刻。

    `mac` 由调用方提供（read_mac 需端口空闲，而录制链校验需 capture 运行，
    两者不能同时进行）——见 rollback_gate 内部的两阶段流程。
    """
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from preflight_flash import inspect_capture, inspect_log  # noqa: E402

    ok_mac = mac is not None
    mac_err = None if ok_mac else "未提供 MAC（调用方需先停止录制并 read_mac）"
    portname = Path(port).name
    cap = inspect_capture(log_dir, portname)
    _m, loginfo = inspect_log(log_dir, portname, 120)
    anchor = {
        "version": 1,
        "port": port,
        "port_name": portname,
        "log_dir": str(log_dir),
        "mac_ok": ok_mac,
        "mac": mac if ok_mac else None,
        "mac_error": mac_err,
        "expect_mac": expect_mac,
        "adapter_by_id": adapter_id(port),
        "capture": {k: cap.get(k) for k in
                    ("running", "script_ok", "port_arg_ok", "log_arg_ok",
                     "fd_holds_port", "fd_holds_log")},
        "log": {k: loginfo.get(k) for k in ("fresh", "boot_marker", "size")},
        "checked_at_unix": int(time.time()),
        "checked_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
    }
    return anchor


def write_anchor(path, anchor):
    Path(path).write_text(json.dumps(anchor, ensure_ascii=False, indent=1),
                          encoding="utf-8")


def load_anchor(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))
