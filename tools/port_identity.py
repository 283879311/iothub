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
import math
import os
import re
import stat
import subprocess
import sys
import tempfile
import time
import uuid
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
    Path(path).parent.mkdir(parents=True, exist_ok=True)   # 记录可能落在新建的证据目录里
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
    """严格有限数值：排除 bool、NaN、±Inf。

    NaN 与任何数比较都是 False，会**绕过** `age < -300` / `age > max_age` 两个
    边界判断（NaN 时间戳就能冒充"新鲜"）；bool 是 int 子类。这里一并排除。
    """
    if type(v) is int:
        return True
    return type(v) is float and math.isfinite(v)


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




# ---------------- 串口协作锁（按**串口**唯一，不按日志文件） ----------------
# 语义边界（必须如实对待，不得包装）：
#   本协议只约束**遵循本协议**的工具（本仓库 capture_serial.py 与门禁）。
#   它**不能**排除 minicom / 手工 cat / 其它 esptool 等外部进程占用串口。
#   因此：
#     COOPERATING_CAPTURE_STOPPED ≠ PORT_FREE
#   读 MAC 需要两个**互相独立**的条件：
#     1) 门禁成功持有端口锁（证明没有协作采集在运行/启动，且在读期间不能启动）；
#     2) 操作者显式声明"无外部工具占用该串口"（软件无法证明，写入证据留痕）。

RUNNING = "RUNNING"
COOPERATING_STOPPED = "COOPERATING_CAPTURE_STOPPED"
UNKNOWN = "UNKNOWN"

PORT_FREE_NOT_PROVEN = ("协作锁三态只说明是否有遵循本协议的采集在运行；"
                        "不能证明串口无外部持有者（port_free_proven 恒为 false）")

LOCK_DIRS = ("/run/lock", "/tmp")     # 优先系统锁目录（1777）；都不可写时用 tmp


def port_lock_id(port):
    """规范化串口标识：realpath 后取 basename（by-id/别名归一到 ttyUSBx）。"""
    try:
        resolved = Path(port).resolve()
    except OSError:
        resolved = Path(port)
    return resolved.name or "unknown"


def port_lock_path(port, lock_dir=None):
    """**按串口唯一**的锁路径；capture 与门禁必须用同一函数计算。

    锁名只取决于规范化后的串口名（如 ttyUSB1），与日志文件无关——
    否则两个不同日志路径的采集会各自持锁却同时占用同一串口。
    """
    name = "iothub_serial_%s.lock" % re.sub(r"[^A-Za-z0-9_.-]", "_",
                                            port_lock_id(port))
    if lock_dir:
        return str(Path(lock_dir) / name)
    for d in LOCK_DIRS:
        if os.access(d, os.W_OK):
            return str(Path(d) / name)
    return str(Path(tempfile.gettempdir()) / name)


def open_lock_file(path, create=True):
    """打开锁文件：O_NOFOLLOW 防符号链接（/run/lock、/tmp 均为 1777）。"""
    flags = os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW
    if create:
        flags |= os.O_CREAT
    fd = os.open(path, flags, 0o664)
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            raise OSError("lock path is not a regular file")
    except OSError:
        os.close(fd)
        raise
    return os.fdopen(fd, "r+", encoding="utf-8")


class PortLock:
    """端口锁持有者：与 capture_serial.py 使用**同一路径**的 flock(LOCK_EX)。"""

    def __init__(self, port=None, lock_path=None):
        self.path = lock_path or port_lock_path(port)
        self._f = None

    def acquire(self, timeout_s=5.0):
        import fcntl
        deadline = time.time() + max(0.0, timeout_s)
        while True:
            try:
                f = open_lock_file(self.path)
            except OSError as exc:
                return (False, f"open_failed:{type(exc).__name__}")
            try:
                fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                self._f = f
                return (True, "acquired")
            except BlockingIOError:
                f.close()
                if time.time() >= deadline:
                    return (False, "held_by_other")
                time.sleep(0.2)
            except OSError as exc:
                f.close()
                return (False, f"flock_failed:{type(exc).__name__}")

    def release(self):
        import fcntl
        f, self._f = self._f, None
        if f is None:
            return
        try:
            fcntl.flock(f, fcntl.LOCK_UN)
        except OSError:
            pass
        try:
            f.close()
        except OSError:
            pass


def _probe_lock(path):
    """试探锁；返回 (held, reason)。held=None 表示**无法判定**。"""
    import fcntl
    try:
        f = open_lock_file(path, create=False)
    except FileNotFoundError:
        return (False, "lock_file_absent")     # 从未启动过 → 视为未运行
    except OSError as exc:
        return (None, f"open_failed:{type(exc).__name__}")
    try:
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return (True, "lock_held")
        except OSError as exc:
            return (None, f"flock_failed:{type(exc).__name__}")
        try:
            fcntl.flock(f, fcntl.LOCK_UN)
        except OSError:
            pass
        return (False, "lock_free")
    finally:
        f.close()


def _scan_holders(proc_root, port):
    """保留旧接口（兼容注入），但三态判定已改为锁协议，不再使用 /proc 枚举。"""
    return ([], f"proc_scan_disabled(lock_protocol):{port}")


def capture_state(port, log_path=None, lock_path=None, scanner=None):
    """三态 RUNNING / COOPERATING_CAPTURE_STOPPED / UNKNOWN（**不是**端口空闲证明）。

    判定（不读 /proc 枚举、不需要 root、不触碰串口）：
      RUNNING               : 端口锁被持有（协作采集在运行）、或旧版采集的
                              `<log>.lock` 仍被持有、或 pidfile 进程仍存活
      COOPERATING_STOPPED   : 端口锁可取得且无 pidfile 进程存活
      UNKNOWN               : 锁打不开/加锁出错、pidfile 不可解析、或注入了未知原因

    任何返回都带 `port_free_proven: False`：本函数**不**证明串口无人占用。
    """
    inf = {"port": port,
           "port_lock": None, "log_lock": None,
           "port_free_proven": False,
           "state_meaning": PORT_FREE_NOT_PROVEN,
           "pidfile_pid": None, "pidfile_state": "absent",
           "unknown_reason": None}

    plock = lock_path or port_lock_path(port)
    held, why = _probe_lock(plock)
    inf["port_lock"] = {"path": plock, "held": held, "reason": why}
    if held is True:
        return (RUNNING, inf)
    if held is None:
        inf["unknown_reason"] = why or "port_lock_unknown"
    if not log_path:
        # 无日志路径 → 无法排除旧版采集的 <log>.lock / pidfile → 不猜
        inf["unknown_reason"] = inf["unknown_reason"] or "no_log_path"

    # 旧版本（<v10）采集只持有 <log>.lock：据该日志所在进程保守判运行
    if log_path:
        lpath = str(log_path) + ".lock"
        lheld, lwhy = _probe_lock(lpath)
        inf["log_lock"] = {"path": lpath, "held": lheld, "reason": lwhy}
        if lheld is True:
            return (RUNNING, inf)
        if lheld is None:
            inf["unknown_reason"] = (inf["unknown_reason"]
                                     or lwhy or "log_lock_unknown")

    pidfile_state = "absent"
    if log_path:
        pidfile = Path(str(log_path) + ".pid")
        try:
            if pidfile.is_file():
                try:
                    inf["pidfile_pid"] = int(pidfile.read_text(encoding="utf-8").strip())
                    pidfile_state = "parsed"
                except Exception:  # noqa: BLE001
                    pidfile_state = "unparsable"
                    inf["unknown_reason"] = inf["unknown_reason"] or "pidfile_unparsable"
        except OSError as exc:
            pidfile_state = "stat_error"
            inf["unknown_reason"] = (inf["unknown_reason"]
                                     or f"pidfile_stat:{type(exc).__name__}")
    inf["pidfile_state"] = pidfile_state

    if inf["unknown_reason"]:
        return (UNKNOWN, inf)
    if inf["pidfile_pid"] and Path(f"/proc/{inf['pidfile_pid']}").is_dir():
        return (RUNNING, inf)               # 锁已释放但进程未退出（异常态）→ 保守判运行
    if scanner is not None:                 # 测试可注入额外原因
        _h, extra = scanner(None, port)
        if extra:
            inf["unknown_reason"] = extra
            return (UNKNOWN, inf)
    return (COOPERATING_STOPPED, inf)


# ---------------- 最终证据消费端校验（可单测，门禁放行的唯一依据） ----------------

FINAL_EVIDENCE_NAME = "mac_read_flow.final.json"
EVIDENCE_MAX_AGE_MIN = 240        # 固定阈值，不接受放宽
EVIDENCE_PRODUCTS = ("mac_read_flow.final.json", "mac_read_flow.json",
                     "mac_record_gate.json", "rollback_gate_last.json")


def evidence_dir_stale(evidence_dir):
    """列出证据目录里已有的本轮产物（空列表 = 干净）。"""
    d = Path(evidence_dir)
    return [n for n in EVIDENCE_PRODUCTS if (d / n).exists()]


def prepare_fresh_evidence_dir(evidence_dir):
    """为**本轮**原子地准备全新证据目录（`read` 与 `rollback_gate` 共用）。

    返回 (ok, reasons[])。用 `mkdir(exist_ok=False)` 创建，消除"检查—创建"
    竞争：目录已存在即拒绝（无论是否含旧产物），两个进程不可能同时进入同一
    目录。目录由本函数创建，调用方**不要**预先 `mkdir`。
    """
    if not evidence_dir:
        return (False, ["evidence_dir 为空"])
    try:
        Path(evidence_dir).mkdir(parents=True, exist_ok=False)
        return (True, [])
    except FileExistsError:
        stale = evidence_dir_stale(evidence_dir)
        if stale:
            return (False, [f"证据目录已存在且含旧产物 {stale}"])
        return (False, ["证据目录已存在（必须为本轮全新目录，由工具创建）"])
    except OSError as exc:  # noqa: BLE001
        return (False, [f"证据目录创建失败（{type(exc).__name__}）"])


def validate_final_evidence(evidence_dir, run_id, port, mac=EXPECT_MAC_65D8,
                            max_age_min=EVIDENCE_MAX_AGE_MIN,
                            require_restore=True, require_new_boot=True,
                            expected_lock_path=None):
    """校验最终证据是否**唯一属于本轮**；返回 (ok, reasons[], data)。

    覆盖：run_id / 端口 / MAC / 证据写入结果 / 协作停止态 / 端口锁持有与**路径** /
    操作者声明（含非空说明）/ 恢复字段完整性 / Unix 时间戳新鲜度。
    旧目录里残留的 final 文件会因 run_id 不同而被拒绝。
    `expected_lock_path` 默认由 `port_lock_path(port)` 计算（门禁即如此），
    测试可显式传入本轮实际使用的锁路径。
    """
    if not evidence_dir:
        return (False, ["evidence_dir 为空"], None)
    path = Path(evidence_dir) / FINAL_EVIDENCE_NAME
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return (False, [f"最终证据不存在：{path}"], None)
    except Exception as exc:  # noqa: BLE001
        return (False, [f"最终证据不可解析（{type(exc).__name__}）"], None)
    if not isinstance(data, dict):
        return (False, ["最终证据根对象不是字典"], data)

    r = []

    def need(cond, msg):
        if not cond:
            r.append(msg)

    need(isinstance(run_id, str) and bool(run_id), f"run_id 非法（{run_id!r}）")
    need(data.get("run_id") == run_id,
         f"run_id 不符（{data.get('run_id')!r} vs {run_id!r}）")
    need(data.get("port") == port, f"port 不符（{data.get('port')!r}）")
    need(str(data.get("mac", "")).lower() == str(mac).lower(),
         f"mac 不符（{data.get('mac')!r}）")
    need(data.get("evidence_write_result") == "ok",
         f"证据写入结果非 ok（{data.get('evidence_write_result')!r}）")
    need(data.get("mac_ok") is True, "mac_ok 非 True")
    need(data.get("state_after_stop") == COOPERATING_STOPPED,
         f"state_after_stop 非 {COOPERATING_STOPPED}"
         f"（{data.get('state_after_stop')!r}）")
    pl = data.get("port_lock") or {}
    need(pl.get("acquired") is True, "端口锁未成功持有（port_lock.acquired != True）")
    lock_expect = expected_lock_path or port_lock_path(port)
    need(pl.get("path") == lock_expect,
         f"端口锁路径不符（{pl.get('path')!r} vs {lock_expect!r}）")
    need(data.get("external_holder_attested") is True, "缺少操作者外部占用声明")
    note = data.get("attestation_note")
    need(isinstance(note, str) and bool(note.strip()),
         f"操作者声明说明为空或非字符串（{note!r}）")
    need(data.get("port_free_proven") is False,
         "port_free_proven 必须为 False（不得伪装端口已空闲）")
    errs = data.get("errors")
    need(isinstance(errs, list) and not errs, f"errors 非空（{errs!r}）")
    if require_restore:
        need(data.get("state_after_restore") == RUNNING,
             f"state_after_restore 非 {RUNNING}"
             f"（{data.get('state_after_restore')!r}）")
        need(data.get("run_app_ok") is True, "run_app_ok 非 True")
        need(data.get("capture_restarted") is True, "capture_restarted 非 True")
        need(data.get("record_written") is True, "record_written 非 True")
        if require_new_boot:
            need(data.get("new_boot") is True, "new_boot 非 True")
    # 机器校验以 Unix 时间戳为准；`at` 仅作人读，仍需可解析
    ts = data.get("at_unix")
    if not is_strict_number(ts):
        r.append(f"at_unix 缺失或类型非数值（{ts!r}）")
    else:
        age = time.time() - float(ts)
        if age < -300:
            r.append(f"at_unix 在未来（age={age:.0f}s）")
        elif age > max_age_min * 60:
            r.append(f"证据过期 age={age/60:.1f}min > {max_age_min}min")
    at = data.get("at")
    try:
        time.strptime(str(at), "%Y-%m-%dT%H:%M:%S")
    except Exception:  # noqa: BLE001
        r.append(f"at 时间戳不可解析（{at!r}）")
    return (not r, r, data)


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
                  chain_check=None, state_fn=None, run_id=None,
                  external_holder_attested=False, attestation_note=None,
                  lock_path=None, lock_timeout_s=5.0,
                  evidence_dir_prepared=False, require_chain=True):
    """步骤 0 / 回滚门禁共用的唯一实现。

    状态流程：检查录制链 → 停录制 → **持端口锁** → read_mac → 无论结果都恢复
    （run_app → 释放锁 → 重启录制 → 等新 boot 证据）→ 判定 → 写记录与证据。

    读 MAC 的**两个独立条件**（缺一即拒绝；不得把"协作采集已停"当成"端口空闲"）：
      1) 端口锁由本流程成功持有——读期间任何协作采集都无法启动；
      2) `external_holder_attested=True`：操作者显式声明无外部工具占用该串口。
         软件**无法**证明这一点（minicom / 手工 cat / 其它 esptool 不遵循本协议），
         因此证据里 `port_free_proven` 恒为 False，且校验端强制要求它为 False。

    每一步都独立异常保护：read/run/start/boot 任一抛出或失败都不会阻断
    后续恢复步骤；最终统一计算 state_confirmed 与 ok。
    成功条件：MAC 正确 **且** 端口锁已持有 **且** 操作者已声明 **且** run 成功
    **且** 录制重启成功 **且**（strict_restore 时）新 boot 证据出现
    **且** 记录/证据写入成功。`evidence_dir` 为**必填**。
    """
    read_mac = read_mac or esptool_read_mac
    run_app = run_app or esptool_run_app
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from preflight_flash import inspect_capture  # noqa: E402

    log_path = str(Path(log_dir) / f"serial_{Path(port).name}.log")
    state_fn = state_fn or (lambda: capture_state(port, log_path, lock_path=lock_path))
    run_id = run_id or (time.strftime("%Y%m%d_%H%M%S") + "_" + uuid.uuid4().hex[:8])
    ev = {"run_id": run_id,
          "port": port, "log_path": log_path, "evidence_dir": str(evidence_dir),
          "chain_before": None, "capture_stopped": False, "mac": None,
          "mac_ok": False, "run_app_ok": False, "capture_restarted": False,
          "new_boot": False, "record_written": False,
          "evidence_written": False, "state_confirmed": False,
          "port_free_proven": False,
          "external_holder_attested": bool(external_holder_attested),
          "attestation_note": attestation_note,
          "port_lock": {"path": lock_path or port_lock_path(port),
                        "acquired": False, "reason": None},
          "errors": [], "at": time.strftime("%Y-%m-%dT%H:%M:%S"),
          "at_unix": int(time.time())}

    def note_error(stage, exc):
        msg = f"{stage}:{type(exc).__name__}:{str(exc)[:80]}"
        ev["errors"].append(msg)
        print(f"  [ERR] {msg}")

    if not evidence_dir:
        print("  [FAIL] evidence_dir 必填（恢复证据是成功条件）")
        ev["errors"].append("evidence_dir_missing")
        return (False, ev)

    # 证据目录必须为**本轮全新**（与 rollback_gate 共用同一判定）
    if evidence_dir_prepared:
        if not Path(evidence_dir).is_dir() or evidence_dir_stale(evidence_dir):
            print("  [FAIL] evidence_dir_prepared=True 但目录不存在或已有旧产物")
            ev["errors"].append("evidence_dir_prepared_invalid")
            ev["evidence_write_attempted"] = True
            ev["evidence_write_result"] = "fail:evidence_dir_invalid"
            return (False, ev)
    else:
        ok_dir, dir_why = prepare_fresh_evidence_dir(evidence_dir)
        if not ok_dir:
            for w in dir_why:
                print(f"  [FAIL] {w}")
                ev["errors"].append(f"evidence_dir:{w}")
            ev["evidence_write_attempted"] = True
            ev["evidence_write_result"] = "fail:evidence_dir_not_fresh"
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
    if not require_chain:
        # 恢复模式：FI 可能无法启动，不能把"app 在跑"当作恢复前提；
        # 串口互斥改由**端口锁**保证（下面强制持有）。
        print("  [info] require_chain=False（恢复模式）：录制链仅记录，不作放行条件")
        chain_ok = True
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
        ev["capture_stopped"] = (st == COOPERATING_STOPPED)
        print(f"  [{'ok' if st == COOPERATING_STOPPED else 'FAIL'}] 停录制后状态={st} "
              f"cmd={stopped_ok} port_lock={stinfo.get('port_lock')} msg={msg[:50]}")
        if st == UNKNOWN:
            print("  [STOP] 无法确认协作采集状态——不得读 MAC")
        elif st == COOPERATING_STOPPED:
            print("  [info] 该状态只表示**协作采集已停**，不证明串口无外部持有者")

    # ---- 3. 持端口锁 + 读 MAC（两个独立条件，缺一即拒绝）----
    lock = PortLock(port=port, lock_path=lock_path)
    mac_read_attempted = False
    try:
        if ev["capture_stopped"]:
            if not external_holder_attested:
                ev["errors"].append("external_holder_not_attested")
                print("  [STOP] 缺操作者声明（无外部工具占用该串口）——不得读 MAC")
            else:
                ok_lk, lk_why = lock.acquire(lock_timeout_s)
                ev["port_lock"]["acquired"] = bool(ok_lk)
                ev["port_lock"]["reason"] = lk_why
                print(f"  [{'ok' if ok_lk else 'FAIL'}] 端口锁 {lock.path}（{lk_why}）")
                if ok_lk:
                    mac_read_attempted = True
                    try:
                        ok_mac, mac_or_err = read_mac(port)
                        if ok_mac:
                            ev["mac"] = mac_or_err
                            ev["mac_ok"] = (str(mac_or_err).lower() == expect_mac)
                            print(f"  [{'ok' if ev['mac_ok'] else 'FAIL'}] "
                                  f"read_mac={mac_or_err}"
                                  f"{'' if ev['mac_ok'] else f'（期望 {expect_mac}）'}")
                        else:
                            print(f"  [FAIL] read_mac 失败：{str(mac_or_err)[:100]}")
                    except Exception as exc:  # noqa: BLE001
                        note_error("read_mac", exc)

        # ---- 4. 恢复：run_app 与 重启录制 各自独立 ----
        # run_app 只在"确实读过 MAC"时需要（否则芯片没进下载模式）；
        # 重启录制在"曾发起停止且确认协作采集已停"时需要。
        need_run = mac_read_attempted
        need_start = bool(stop_issued
                          and ev.get("state_after_stop") == COOPERATING_STOPPED)
        if need_run or need_start:
            size_before = _log_size(log_path)
            if need_run:
                try:
                    run_ok, rmsg = run_app(port)
                except Exception as exc:  # noqa: BLE001
                    note_error("run_app", exc)
                    run_ok, rmsg = False, "exception"
                ev["run_app_ok"] = bool(run_ok)
                print(f"  [{'ok' if run_ok else 'FAIL'}] 恢复 app：{str(rmsg)[:70]}")
            lock.release()          # 必须先释放：capture 启动需要同一把端口锁

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
    finally:
        lock.release()                  # 任何异常路径都必须放锁

    # ---- 5. 判定 ----
    mac_ok = ev["mac_ok"] is True
    ev["state_confirmed"] = bool(
        mac_ok and ev["run_app_ok"] and ev["capture_restarted"]
        and (ev["new_boot"] is True or not strict_restore))
    ok = bool(chain_ok and ev["capture_stopped"]
              and ev["port_lock"].get("acquired") is True
              and external_holder_attested is True
              and ev["state_confirmed"])

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
    r.add_argument("--attest-no-external-holder", required=True,
                   metavar="说明",
                   help="操作者声明：该串口无外部工具占用（minicom/手工 cat 等）。"
                        "软件无法证明，必须显式声明并写入证据")
    a = ap.parse_args()
    if a.cmd == "read":
        if not a.attest_no_external_holder.strip():
            print("[FAIL] --attest-no-external-holder 不能为空")
            return 2
        print("[info] 边界：本流程只能证明**协作采集已停**，"
              "不证明串口无外部持有者（port_free_proven=false）")
        ok_dir, dir_why = prepare_fresh_evidence_dir(a.evidence_dir)
        if not ok_dir:
            for w in dir_why:
                print(f"[FAIL] {w}")
            return 2
        ok, ev = read_mac_flow(a.port, a.log_dir, a.out,
                               evidence_dir=a.evidence_dir,
                               external_holder_attested=True,
                               attestation_note=a.attest_no_external_holder.strip(),
                               evidence_dir_prepared=True)
        return 0 if ok else 1
    return 2


if __name__ == "__main__":
    sys.exit(_cli())
