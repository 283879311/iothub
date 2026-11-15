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
    if anchor.get("mac_ok") is not True:
        r.append(f"mac_ok 非 True（{anchor.get('mac_ok')!r}）")
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




# ---------------- 录制进程三态（STOPPED / RUNNING / UNKNOWN） ----------------
# 注意：不能用 inspect_capture().running 代替——那是"完整链校验通过"，
# 进程可能仍存活但 argv/fd 校验失败；此处只看**进程存活与端口占用**。

STOPPED, RUNNING, UNKNOWN = "STOPPED", "RUNNING", "UNKNOWN"


def capture_state(port, log_path=None, proc_root="/proc"):
    """返回 (state, info)。

    STOPPED : 无任何进程持有该串口，且 PID 文件（若有）指向的进程不存在/非采集进程
    RUNNING : 有进程持有该串口，或 PID 文件指向的进程仍存活
    UNKNOWN : 无法读取进程/FD 信息（权限、/proc 不可用等）——**必须按不可读 MAC 处理**
    """
    import os
    inf = {"port": port, "holders": [], "pidfile_pid": None, "reason": None}
    root = Path(proc_root)
    if not root.is_dir():
        inf["reason"] = f"proc_root 不可用：{root}"
        return (UNKNOWN, inf)

    scan_error = False
    try:
        entries = list(root.iterdir())
    except OSError as exc:
        inf["reason"] = f"无法列出 {root}：{exc}"
        return (UNKNOWN, inf)
    for ent in entries:
        if not ent.name.isdigit():
            continue
        try:
            fds = list((ent / "fd").iterdir())
        except OSError:
            # 无权限读取其它进程的 fd 是常态；但我们也无法确认它是否占用端口
            continue
        for fd in fds:
            try:
                if os.readlink(fd) == port:
                    inf["holders"].append(int(ent.name))
                    break
            except OSError:
                continue

    pidfile = None
    if log_path:
        pidfile = Path(str(log_path) + ".pid")
        if pidfile.is_file():
            try:
                inf["pidfile_pid"] = int(pidfile.read_text(encoding="utf-8").strip())
            except Exception:  # noqa: BLE001
                inf["pidfile_pid"] = None

    if inf["holders"]:
        return (RUNNING, inf)
    if inf["pidfile_pid"] and (root / str(inf["pidfile_pid"])).is_dir():
        return (RUNNING, inf)      # 进程存活但此刻未持端口（可能正在 reopen）
    if scan_error:
        return (UNKNOWN, inf)
    # 无 holder、pidfile 进程不存在 → 视为已停止
    return (STOPPED, inf)


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
                  read_mac=None, run_app=None, strict_restore=True,
                  chain_check=None, state_fn=None):
    """步骤 0 / 回滚门禁共用的唯一实现。

    状态流程：检查录制链 → 停录制 → read_mac → **无论结果都恢复**
    （run_app + 重启录制 + 等新 boot 证据）→ 判定 → 写记录与证据。

    每一步都独立异常保护：read/run/start/boot 任一抛出或失败都不会阻断
    后续恢复步骤；最终统一计算 state_confirmed 与 ok。
    成功条件：MAC 正确 **且** run 成功 **且** 录制重启成功 **且**
    （strict_restore 时）新 boot 证据出现 **且** 记录/证据写入成功。
    `evidence_dir` 为**必填**（恢复证据是成功条件的一部分）。
    """
    read_mac = read_mac or esptool_read_mac
    run_app = run_app or esptool_run_app
    state_fn = state_fn or (lambda: capture_state(port, log_path))
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from preflight_flash import inspect_capture  # noqa: E402

    log_path = str(Path(log_dir) / f"serial_{Path(port).name}.log")
    ev = {"port": port, "log_path": log_path, "evidence_dir": str(evidence_dir),
          "chain_before": None, "capture_stopped": False, "mac": None,
          "mac_ok": False, "run_app_ok": False, "capture_restarted": False,
          "new_boot": False, "record_written": False,
          "evidence_written": False, "state_confirmed": False,
          "errors": [], "at": time.strftime("%Y-%m-%dT%H:%M:%S")}

    def note_error(stage, exc):
        msg = f"{stage}:{type(exc).__name__}:{str(exc)[:80]}"
        ev["errors"].append(msg)
        print(f"  [ERR] {msg}")

    if not evidence_dir:
        print("  [FAIL] evidence_dir 必填（恢复证据是成功条件）")
        ev["errors"].append("evidence_dir_missing")
        return (False, ev)

    # ---- 1. 录制链 ----
    try:
        cap = (chain_check or inspect_capture)(log_dir, Path(port).name) \
            if chain_check else inspect_capture(log_dir, Path(port).name)
        ev["chain_before"] = {k: cap.get(k) for k in
                              ("running", "script_ok", "port_arg_ok", "log_arg_ok",
                               "fd_holds_port", "fd_holds_log")}
    except Exception as exc:  # noqa: BLE001
        note_error("chain_check", exc)
        ev["chain_before"] = {}
    chain_ok = bool(ev["chain_before"]) and all(
        v is True for v in ev["chain_before"].values())
    if not chain_ok:
        print("  [FAIL] 录制链不完整——不停录制、不读 MAC")

    # ---- 2. 停录制（**发起过停止就不等于无需恢复**）----
    stop_issued = False
    if chain_ok:
        stop_issued = True
        try:
            stopped_ok, msg = _ctl("stop", port, log_path, capture_ctl)
        except Exception as exc:  # noqa: BLE001
            note_error("capture_stop", exc)
            stopped_ok, msg = False, "exception"
        # 以**三态**为准（不看 inspect_capture.running，那是完整链校验）
        try:
            st, stinfo = state_fn()
        except Exception as exc:  # noqa: BLE001
            note_error("post_stop_state", exc)
            st, stinfo = UNKNOWN, {"reason": "exception"}
        ev["stop_cmd_ok"] = stopped_ok
        ev["state_after_stop"] = st
        ev["state_after_stop_info"] = stinfo
        ev["capture_stopped"] = (st == STOPPED)
        print(f"  [{'ok' if st == STOPPED else 'FAIL'}] 停录制后状态={st} "
              f"cmd={stopped_ok} holders={stinfo.get('holders')} msg={msg[:50]}")
        if st == UNKNOWN:
            print("  [STOP] 无法确认录制进程/端口占用——不得读 MAC")

    # ---- 3. 读 MAC（异常也继续到恢复）----
    mac_read_attempted = False
    if ev["capture_stopped"]:
        mac_read_attempted = True
        try:
            ok_mac, mac_or_err = read_mac(port)
            if ok_mac:
                ev["mac"] = mac_or_err
                ev["mac_ok"] = (str(mac_or_err).lower() == expect_mac)
                print(f"  [{'ok' if ev['mac_ok'] else 'FAIL'}] read_mac={mac_or_err}"
                      f"{'' if ev['mac_ok'] else f'（期望 {expect_mac}）'}")
            else:
                print(f"  [FAIL] read_mac 失败：{str(mac_or_err)[:100]}")
        except Exception as exc:  # noqa: BLE001
            note_error("read_mac", exc)

    # ---- 4. 恢复：run_app 与 重启录制 各自独立 ----
    # 触发条件：读 MAC（进程确实停了）**或** 曾发起停止且当前未运行
    restore_needed = bool(mac_read_attempted or
                          (stop_issued and ev.get("state_after_stop") == STOPPED))
    if restore_needed:
        size_before = _log_size(log_path)
        try:
            run_ok, rmsg = run_app(port)
        except Exception as exc:  # noqa: BLE001
            note_error("run_app", exc)
            run_ok, rmsg = False, "exception"
        ev["run_app_ok"] = bool(run_ok)
        print(f"  [{'ok' if run_ok else 'FAIL'}] 恢复 app：{str(rmsg)[:70]}")

        try:
            cap3 = inspect_capture(log_dir, Path(port).name)
            already = cap3.get("running") is True
        except Exception:  # noqa: BLE001
            already = False
        if already:
            start_ok, smsg = True, "already running (skip start)"
        else:
            try:
                start_ok, smsg = _ctl("start", port, log_path, capture_ctl)
            except Exception as exc:  # noqa: BLE001
                note_error("capture_start", exc)
                start_ok, smsg = False, "exception"
        ev["capture_restarted"] = bool(start_ok)
        print(f"  [{'ok' if start_ok else 'FAIL'}] 重启录制：{smsg}")

        try:
            st2, st2info = state_fn()
        except Exception:  # noqa: BLE001
            st2, st2info = UNKNOWN, {}
        ev["state_after_restore"] = st2
        if st2 != RUNNING:
            ev["errors"].append(f"state_after_restore={st2}")
            print(f"  [FAIL] 恢复后录制状态={st2}（期望 RUNNING）")
            start_ok = False
            ev["capture_restarted"] = False

        if start_ok and strict_restore:
            try:
                boot_ok, bmsg = wait_new_boot(log_path, size_before)
            except Exception as exc:  # noqa: BLE001
                note_error("wait_new_boot", exc)
                boot_ok, bmsg = False, "exception"
            ev["new_boot"] = bool(boot_ok)
            print(f"  [{'ok' if boot_ok else 'FAIL'}] 新 boot 证据：{bmsg}")
        elif not strict_restore:
            ev["new_boot"] = None
            print("  [info] strict_restore=False：跳过新 boot 证据等待")

    # ---- 5. 判定 ----
    mac_ok = ev["mac_ok"] is True
    ev["state_confirmed"] = bool(
        mac_ok and ev["run_app_ok"] and ev["capture_restarted"]
        and (ev["new_boot"] is True or not strict_restore))
    ok = bool(chain_ok and ev["capture_stopped"] and ev["state_confirmed"])

    # ---- 6. 记录与证据（失败也尽力写；写入失败计入 ok）----
    if ok and ev["mac"]:
        try:
            write_mac_record(out_record, port, ev["mac"], log_dir)
            ev["record_written"] = True
            print(f"  [ok] MAC 记录已写出：{out_record}")
        except Exception as exc:  # noqa: BLE001
            note_error("write_mac_record", exc)
            ok = False

    ev["evidence_write_attempted"] = True
    ev["evidence_write_result"] = None
    try:
        Path(evidence_dir).mkdir(parents=True, exist_ok=True)
        path = Path(evidence_dir, "mac_read_flow.json")
        path.write_text(json.dumps(ev, ensure_ascii=False, indent=1),
                        encoding="utf-8")
        ev["evidence_write_result"] = "ok"
        ev["evidence_written"] = True
        # 落盘后写入"最终状态"副本，使磁盘证据与返回值一致（失败也记录）
        final = dict(ev)
        final["evidence_write_result"] = "ok"
        Path(evidence_dir, "mac_read_flow.final.json").write_text(
            json.dumps(final, ensure_ascii=False, indent=1), encoding="utf-8")
    except Exception as exc:  # noqa: BLE001
        print(f"  [FAIL] 恢复证据写入失败（{type(exc).__name__}）——按失败处理")
        ev["evidence_write_result"] = f"fail:{type(exc).__name__}"
        ev["evidence_written"] = False
        ok = False
        # 尽力把失败结论回写到首份证据，避免磁盘上留 null 的"无结论"状态
        try:
            Path(evidence_dir, "mac_read_flow.json").write_text(
                json.dumps(ev, ensure_ascii=False, indent=1), encoding="utf-8")
        except Exception:  # noqa: BLE001
            print("  [WARN] 失败状态回写首份证据亦失败（磁盘可能残留 attempted 快照）")

    if not ok:
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
