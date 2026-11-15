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


# ---------------- MAC 记录（步骤 0 → 步骤 1 的唯一通道） ----------------

MAC_RECORD_VERSION = 1
MAC_RECORD_MAX_AGE_MIN = 120      # 固定阈值，不接受调用方放宽


def write_mac_record(path, port, mac, log_dir):
    rec = {
        "version": MAC_RECORD_VERSION,
        "port": port,
        "log_dir": str(log_dir),
        "mac": mac.lower(),
        "mac_source": "esptool read_mac (eFuse)",
        "read_at_unix": int(time.time()),
        "read_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
    }
    Path(path).write_text(json.dumps(rec, ensure_ascii=False, indent=1),
                          encoding="utf-8")
    return rec


def load_mac_record(path, port, log_dir, max_age_min=MAC_RECORD_MAX_AGE_MIN):
    """读取并校验收录；返回 (ok, mac_or_error, detail)。"""
    try:
        rec = json.loads(Path(path).read_text(encoding="utf-8"))
    except Exception as exc:  # noqa: BLE001
        return (False, f"MAC 记录不可解析（{type(exc).__name__}）", {})
    if not isinstance(rec, dict):
        return (False, "MAC 记录根对象不是字典", {})
    miss = [k for k in ("version", "port", "log_dir", "mac",
                        "read_at_unix") if k not in rec]
    if miss:
        return (False, f"MAC 记录缺字段 {miss}", rec)
    if rec.get("version") != MAC_RECORD_VERSION:
        return (False, f"MAC 记录版本不符（{rec.get('version')}）", rec)
    if rec.get("port") != port:
        return (False, f"MAC 记录端口不符（{rec.get('port')} vs {port}）", rec)
    if str(rec.get("log_dir")) != str(log_dir):
        return (False, f"MAC 记录日志目录不符（{rec.get('log_dir')}）", rec)
    mac = str(rec.get("mac", "")).lower()
    if mac != EXPECT_MAC_65D8:
        return (False, f"MAC 记录值不是 65D8（{mac}）", rec)
    age = time.time() - float(rec.get("read_at_unix", 0))
    if not (0 <= age <= max_age_min * 60):
        return (False, f"MAC 记录过期 age={age/60:.1f}min > {max_age_min}min", rec)
    return (True, mac, rec)


def _cli():
    import argparse
    ap = argparse.ArgumentParser(description="端口/板卡身份工具")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("read", help="读芯片 MAC 并写记录（需端口空闲）")
    r.add_argument("--port", required=True)
    r.add_argument("--log-dir", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--restore-app", action="store_true",
                   help="读完后 esptool run 回到 app")
    a = ap.parse_args()
    if a.cmd == "read":
        ok, mac = esptool_read_mac(a.port)
        if not ok:
            print(f"[FAIL] read_mac 失败：{mac}")
            return 2
        if mac.lower() != EXPECT_MAC_65D8:
            print(f"[FAIL] MAC 不是 65D8：{mac}")
            return 1
        rec = write_mac_record(a.out, a.port, mac, a.log_dir)
        print(f"[ok] MAC 记录已写出 {a.out} mac={mac} at={rec['read_at']}")
        if a.restore_app:
            rok, msg = esptool_run_app(a.port)
            print(f"[{'ok' if rok else 'FAIL'}] restore app: {str(msg)[:80]}")
            if not rok:
                return 2
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(_cli())
