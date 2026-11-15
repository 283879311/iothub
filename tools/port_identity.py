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
    if not is_strict_int(rec.get("version")) or \
            rec.get("version") != MAC_RECORD_VERSION:
        return (False, f"MAC 记录版本不符（{rec.get('version')}）", rec)
    if rec.get("port") != port:
        return (False, f"MAC 记录端口不符（{rec.get('port')} vs {port}）", rec)
    if str(rec.get("log_dir")) != str(log_dir):
        return (False, f"MAC 记录日志目录不符（{rec.get('log_dir')}）", rec)
    mac = str(rec.get("mac", "")).lower()
    if mac != EXPECT_MAC_65D8:
        return (False, f"MAC 记录值不是 65D8（{mac}）", rec)
    ts = rec.get("read_at_unix")
    if not is_strict_number(ts):
        return (False, f"MAC 记录时间戳类型非数值（{ts!r}）", rec)
    age = time.time() - float(ts)
    if not (0 <= age <= max_age_min * 60):
        return (False, f"MAC 记录过期 age={age/60:.1f}min > {max_age_min}min", rec)
    return (True, mac, rec)




# ---------------- 严格校验辅助 ----------------

def is_strict_int(v):
    """排除 bool（Python 中 bool 是 int 子类）。"""
    return type(v) is int


def is_strict_number(v):
    return type(v) in (int, float)


def anchor_validate(anchor, port, log_dir, max_age_min=240):
    """锚点严格校验；返回 (ok, reasons[])。"""
    r = []
    if not isinstance(anchor, dict) or len(anchor) == 0:
        return (False, ["根对象为空或非字典"])
    if not is_strict_int(anchor.get("version")) or anchor.get("version") != 1:
        r.append(f"version 必须严格等于整数 1（实际 {anchor.get('version')!r}）")
    if anchor.get("port") != port:
        r.append(f"port 不符（{anchor.get('port')!r}）")
    if str(anchor.get("log_dir")) != str(log_dir):
        r.append(f"log_dir 不符（{anchor.get('log_dir')!r}）")
    mac = anchor.get("mac")
    if not isinstance(mac, str) or mac.lower() != EXPECT_MAC_65D8:
        r.append(f"mac 非 65D8 字符串（{mac!r}）")
    ts = anchor.get("checked_at_unix")
    if not is_strict_number(ts):
        r.append(f"checked_at_unix 类型非数值（{ts!r}）")
    else:
        age = time.time() - float(ts)
        if not (0 <= age <= max_age_min * 60):
            r.append(f"锚点过期 age={age/60:.1f}min > {max_age_min}min")
    return (not r, r)


# ---------------- 统一"停录制→读 MAC→恢复→重启录制" ----------------

def _ctl(cmd, port, log_path, capture_ctl):
    try:
        r = subprocess.run([capture_ctl, cmd, port, log_path],
                           capture_output=True, text=True, timeout=30)
        return (r.returncode == 0, (r.stdout or r.stderr).strip()[:160])
    except Exception as exc:  # noqa: BLE001
        return (False, f"{type(exc).__name__}: {exc}")


def _log_size(log_path):
    try:
        return Path(log_path).stat().st_size
    except OSError:
        return 0


def wait_new_boot(log_path, from_size, timeout_s=15.0):
    """等待日志出现新的 boot 证据（rst:0x1 或 app_init ELF SHA）。"""
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        try:
            with open(log_path, "rb") as f:
                f.seek(0, 2)
                size = f.tell()
                if size <= from_size:
                    time.sleep(1.0)
                    continue
                f.seek(from_size)
                data = f.read().decode("utf-8", "replace")
            if "rst:0x1" in data or "app_init: ELF file SHA256" in data:
                return (True, f"new boot evidence @+{time.time()-t0:.0f}s")
        except OSError as exc:
            return (False, f"日志不可读：{exc}")
        time.sleep(1.0)
    return (False, f"{timeout_s:.0f}s 内未见新的 boot 证据")


def read_mac_flow(port, log_dir, out_record, capture_ctl="tools/serial_capture_ctl.sh",
                  evidence_dir=None, expect_mac=EXPECT_MAC_65D8,
                  read_mac=None, run_app=None, strict_restore=True):
    """步骤 0 的唯一实现（CLI 与测试共用）。

    状态流程：检查录制链 → 停录制 → read_mac → **无论结果都恢复**
    （run_app + 重启录制 + 等待新 boot 证据）→ 判定。
    成功条件：MAC 正确 **且** run_app 成功 **且** 录制重启成功
    （strict_restore 时还要求新 boot 证据）。
    """
    read_mac = read_mac or esptool_read_mac
    run_app = run_app or esptool_run_app
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from preflight_flash import inspect_capture  # noqa: E402

    log_path = str(Path(log_dir) / f"serial_{Path(port).name}.log")
    ev = {"port": port, "log_path": log_path,
          "chain_before": None, "capture_stopped": None, "mac": None,
          "mac_ok": None, "run_app_ok": None, "capture_restarted": None,
          "new_boot": None, "state_confirmed": False,
          "at": time.strftime("%Y-%m-%dT%H:%M:%S")}

    cap = inspect_capture(log_dir, Path(port).name)
    ev["chain_before"] = {k: cap.get(k) for k in
                          ("running", "script_ok", "port_arg_ok", "log_arg_ok",
                           "fd_holds_port", "fd_holds_log")}
    chain_ok = all(v is True for v in ev["chain_before"].values())

    stopped = start_ok = new_boot_ok = False
    mac = None
    if chain_ok:
        stopped, msg = _ctl("stop", port, log_path, capture_ctl)
        ev["capture_stopped"] = stopped
        print(f"  [{'ok' if stopped else 'FAIL'}] 停录制：{msg}")
    else:
        ev["capture_stopped"] = False
        print("  [FAIL] 录制链不完整——不停录制、不读 MAC（先修复录制）")

    if stopped:
        try:
            ok_mac, mac_or_err = read_mac(port)
            if ok_mac:
                mac = mac_or_err
                ev["mac"] = mac
                ev["mac_ok"] = (mac.lower() == expect_mac)
                print(f"  [{'ok' if ev['mac_ok'] else 'FAIL'}] read_mac={mac}"
                      f"{'' if ev['mac_ok'] else f'（期望 {expect_mac}）'}")
            else:
                ev["mac_ok"] = False
                print(f"  [FAIL] read_mac 失败：{str(mac_or_err)[:100]}")
        finally:
            # 无论读取成功/失败/MAC 不符，都恢复
            size_before = _log_size(log_path)
            run_ok, rmsg = run_app(port)
            ev["run_app_ok"] = run_ok
            print(f"  [{'ok' if run_ok else 'FAIL'}] 恢复 app：{str(rmsg)[:70]}")
            start_ok, smsg = _ctl("start", port, log_path, capture_ctl)
            ev["capture_restarted"] = start_ok
            print(f"  [{'ok' if start_ok else 'FAIL'}] 重启录制：{smsg}")
            if start_ok:
                new_boot_ok, bmsg = wait_new_boot(log_path, size_before)
                ev["new_boot"] = new_boot_ok
                print(f"  [{'ok' if new_boot_ok else 'FAIL'}] 新 boot 证据：{bmsg}")

    mac_ok = ev.get("mac_ok") is True
    ev["state_confirmed"] = bool(mac_ok and ev.get("run_app_ok")
                                 and ev.get("capture_restarted")
                                 and (new_boot_ok or not strict_restore))
    ok = bool(chain_ok and stopped and ev["state_confirmed"])

    if evidence_dir:
        try:
            Path(evidence_dir).mkdir(parents=True, exist_ok=True)
            Path(evidence_dir, "mac_read_flow.json").write_text(
                json.dumps(ev, ensure_ascii=False, indent=1), encoding="utf-8")
        except Exception as exc:  # noqa: BLE001
            print(f"  [FAIL] 恢复证据写入失败（{type(exc).__name__}）——按失败处理")
            ok = False

    if ok and mac:
        write_mac_record(out_record, port, mac, log_dir)
        print(f"  [ok] MAC 记录已写出：{out_record}")
    elif not ok:
        print("  [STOP] 设备/录制状态不可确认（见 mac_read_flow.json），不得继续")
    return (ok, ev)


def _cli():
    import argparse
    ap = argparse.ArgumentParser(description="端口/板卡身份工具")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("read", help="读芯片 MAC 并写记录（需端口空闲）")
    r.add_argument("--port", required=True)
    r.add_argument("--log-dir", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--evidence-dir", required=True,
                   help="本轮证据目录（恢复流程证据写入此处；写失败即失败）")
    a = ap.parse_args()
    if a.cmd == "read":
        ok, ev = read_mac_flow(a.port, a.log_dir, a.out,
                               evidence_dir=a.evidence_dir)
        return 0 if ok else 1
    return 2


if __name__ == "__main__":
    sys.exit(_cli())
