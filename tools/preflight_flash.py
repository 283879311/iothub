#!/usr/bin/env python3
"""烧录前四元组门禁（S6 事故后强制流程）：端口 ↔ 设备 ↔ 镜像 一次核对。

用途：在 `idf.py flash` 之前运行，打印并校验
  目标设备 MAC / device_id / 串口端口 / 镜像 sha
任一项不符 → 退出码非 0，禁止烧录。

端口↔设备映射依据：设备 console 日志里的
  `mqtt: Starting MQTT: uri=..., client_id=iothub-<WIFI_MAC>`
（**device_id = WiFi MAC 末 4 位**，如 wifi B0:A7:32:28:C8:E4 → C8E4）。
注意：ESP32 的 **BLE 地址 = WiFi MAC + 2**（实测 C8E6↔C8E4、65DA↔65D8），
所以 BLE 扫描地址不能直接与 client_id 比较。

用法（全部参数必填）：
  tools/preflight_flash.py --port /dev/ttyUSB1 \
      --log-dir artifacts/cmd04_flash_20261114/phaseA \
      --expect-device-id IotHub-65D8 \
      --bin artifacts/.../iothub_a7e04b6a9.bin \
      --expect-bin-sha256 047e4726...ea41 \
      [--elf artifacts/.../iothub_a7e04b6a9.elf --expect-build-sha a7e04b6a9]

校验项（任一不符 → 非零退出，禁止烧录）：
  1) 该端口日志的 **最近一次 boot 之后** 有 client_id=iothub-<WiFiMAC>（证明映射属于当前上电会话）；
  2) **两份日志对应的 capture 进程仍存活且持有该串口**（PID 文件 + /proc/<pid>/fd
     指向 /dev/ttyUSBx）——这是"当前会话正在采集"的主证据；
  2b) 两份日志 mtime 在 --max-age-sec 内（**辅助**新鲜度条件，不单独证明录制在运行）；
  3) 端口对应 device_id 与 --expect-device-id 一致；
  4) **冻结 BIN 的完整 SHA-256** 与 --expect-bin-sha256 一致（授权对象是字节，
     不是易漂移的 ELF build_sha）；若另给 --elf/--expect-build-sha，则额外核对内嵌身份。
注意：本门禁放行只证明"设备身份 + 镜像身份"，**不替代**烧录后 `device.info.build_sha` 反证。
"""

import argparse
import hashlib
import re
import subprocess
import sys
from pathlib import Path

SERIAL_DIR = Path("artifacts/cmd04_flash_20261114/phaseA")


def capture_chain_ok(argv, cwd, fd_targets, port, log_path):
    """完整证据链判定（P1-4）：PID → capture_serial.py → 指定串口 → 指定日志。

    argv        : /proc/<pid>/cmdline 按 NUL 拆分后的参数列表
    cwd         : /proc/<pid>/cwd 解析结果（用于解析 argv 中的相对路径）
    fd_targets  : 该进程所有 fd 的 readlink 结果集合
    port        : 端口名，如 ttyUSB1（期望 /dev/ttyUSB1）
    log_path    : 门禁正在检查的日志路径
    返回 (ok, detail)；detail 含逐项结论，便于归档。
    """
    import os
    dev = f"/dev/{port}"
    log_real = os.path.realpath(str(log_path))

    # capture_serial.py 的实际读参方式：PORT=argv[1], OUT=argv[2]
    # → cmdline 布局固定为 4 项：[python, capture_serial.py, /dev/ttyUSBx, <log>]
    # 必须**按位置**核验；在 argv 中"搜索"会把多余/伪装参数当成证据。
    argv = [str(x) for x in argv]
    # 脚本身份：必须**就是**本仓库这一份 capture_serial.py（同名文件不算）
    expected_script = os.path.realpath(
        os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "capture_serial.py"))
    script_real = None
    if len(argv) >= 2:
        a1 = argv[1]
        script_real = os.path.realpath(
            a1 if os.path.isabs(a1) else os.path.join(cwd or "", a1))
    script_ok = (script_real == expected_script)
    port_ok = len(argv) >= 3 and argv[2] == dev
    argv_log = argv[3] if len(argv) >= 4 else None
    # 参数数量必须与脚本调用形式一致（拒绝多余/缺失）
    argc_ok = (len(argv) == 4) and script_ok and port_ok and argv_log is not None
    argv_log_real = None
    if argv_log is not None:
        argv_log_real = os.path.realpath(
            argv_log if os.path.isabs(argv_log) else os.path.join(cwd or "",
                                                                  argv_log))
    log_arg_ok = (argv_log_real == log_real)

    holds_port = dev in fd_targets
    holds_log = log_real in fd_targets

    detail = {"argc": len(argv), "argc_ok": argc_ok,
              "script_real": script_real,
              "expected_script_real": expected_script,
              "script_ok": script_ok, "port_arg_ok": port_ok,
              "argv_log": argv_log, "argv_log_real": argv_log_real,
              "expect_log_real": log_real, "log_arg_ok": log_arg_ok,
              "fd_holds_port": holds_port, "fd_holds_log": holds_log}
    ok = (argc_ok and script_ok and port_ok and log_arg_ok
          and holds_port and holds_log)
    return ok, detail


def inspect_capture(log_dir, port):
    """录制活动性 + 目标关联（主证据）：PID 存活、argv 指向本串口与本日志、
    fd 同时持有串口与日志文件。"""
    import os
    log = Path(log_dir) / f"serial_{port}.log"
    pidfile = Path(str(log) + ".pid")
    info = {"pidfile": str(pidfile), "pidfile_exists": pidfile.is_file()}
    if not pidfile.is_file():
        info["running"] = False
        return info
    try:
        pid = int(pidfile.read_text(encoding="utf-8").strip())
    except Exception:  # noqa: BLE001
        info["running"] = False
        return info
    info["pid"] = pid
    proc = Path(f"/proc/{pid}")
    info["proc_exists"] = proc.is_dir()
    if not info["proc_exists"]:
        info["running"] = False
        return info
    try:
        argv = [x for x in (proc / "cmdline").read_bytes().split(b"\x00") if x]
        argv = [x.decode("utf-8", "replace") for x in argv]
    except OSError:
        argv = []
    try:
        cwd = os.readlink(proc / "cwd")
    except OSError:
        cwd = ""
    fd_targets = set()
    try:
        for fd in (proc / "fd").iterdir():
            try:
                fd_targets.add(os.readlink(fd))
            except OSError:
                continue
    except OSError:
        pass
    ok, detail = capture_chain_ok(argv, cwd, fd_targets, port, log)
    info.update(detail)
    info["argv"] = argv
    info["running"] = ok
    return info


def inspect_log(log_dir, port, max_age_sec):
    """返回 (wifi_mac|None, info dict)。要求 client_id 出现在最近一次 boot 之后。"""
    import time as _t
    f = Path(log_dir) / f"serial_{port}.log"
    info = {"file": str(f), "exists": f.is_file()}
    if not f.is_file():
        return None, info
    st = f.stat()
    info["size"] = st.st_size
    info["age_sec"] = round(_t.time() - st.st_mtime, 1)
    info["fresh"] = info["age_sec"] <= max_age_sec
    info["sha256_16"] = hashlib.sha256(f.read_bytes()).hexdigest()[:16]

    # 只读尾部 2MB
    size = st.st_size
    with open(f, "rb") as fh:
        if size > 2 * 1024 * 1024:
            fh.seek(size - 2 * 1024 * 1024)
        data = fh.read().decode("utf-8", "replace")

    boot_idx = data.rfind("rst:0x1")
    mac = None
    macs = list(re.finditer(r"client_id=iothub-([0-9A-Fa-f]{12})", data))
    # P1-1：必须先确认 boot 标记存在；无 boot 标记时 rfind 返回 -1，
    # 若直接比较会把任意 client_id 误判为"boot 后证据"。
    if boot_idx < 0:
        info["boot_marker"] = False
        info["client_id_after_boot"] = False
    elif macs and macs[-1].start() > boot_idx:
        info["boot_marker"] = True
        info["client_id_after_boot"] = True
        mac = macs[-1].group(1).upper()
    else:
        info["boot_marker"] = True
        info["client_id_after_boot"] = False
    return mac, info


def dev_id_from_wifi_mac(mac12):
    hexs = (mac12 or "").replace(":", "").replace("-", "").upper()
    return "IotHub-" + hexs[-4:] if len(hexs) == 12 else "?"


def ble_from_wifi_mac(mac12):
    """BLE 地址 = WiFi MAC + 2（末字节加 2，实测两板一致）。"""
    hexs = (mac12 or "").replace(":", "").replace("-", "").upper()
    if len(hexs) != 12:
        return "?"
    last = (int(hexs[10:], 16) + 2) & 0xFF
    body = hexs[:10]
    bh = ":".join(body[i:i + 2] for i in range(0, 10, 2))
    return f"{bh}:{last:02X}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--log-dir", required=True,
                    help="本次会话的 console 日志目录（必填，不使用历史默认值）")
    ap.add_argument("--expect-device-id", required=True,
                    help="目标 device_id，如 IotHub-65D8（由 WiFi MAC 末 4 位推出）")
    ap.add_argument("--bin", required=True,
                    help="**冻结**的烧录产物（将交给 esptool 的同一个文件）")
    ap.add_argument("--expect-bin-sha256", required=True,
                    help="冻结 BIN 的完整 SHA-256（授权对象）")
    ap.add_argument("--elf", required=False,
                    help="可选：配套 ELF，用于额外核对内嵌 build_sha")
    ap.add_argument("--expect-build-sha", required=False,
                    help="可选：期望 build_sha（需同时给 --elf）")
    ap.add_argument("--max-age-sec", type=int, default=120,
                    help="日志新鲜度上限（秒），超龄视为映射证据过期")
    ap.add_argument("--emit-anchor", required=False,
                    help="输出本轮回滚锚点 JSON（供 rollback_gate 使用）")
    ap.add_argument("--mac-record", required=False,
                    help="步骤 0 的芯片 MAC 记录（--emit-anchor 时必填）")
    args = ap.parse_args()

    if args.port not in ("/dev/ttyUSB0", "/dev/ttyUSB1"):
        print(f"[FAIL] 端口不在白名单（{args.port}）——"
              f"仅接受完整路径 /dev/ttyUSB0 或 /dev/ttyUSB1")
        return 2
    name = Path(args.port).name
    other = "ttyUSB1" if name == "ttyUSB0" else "ttyUSB0"
    observed_wifi, loginfo = inspect_log(args.log_dir, name, args.max_age_sec)
    other_wifi, other_info = inspect_log(args.log_dir, other, args.max_age_sec)
    capinfo = inspect_capture(args.log_dir, name)
    capother = inspect_capture(args.log_dir, other)
    observed_id = dev_id_from_wifi_mac(observed_wifi)
    other_id = dev_id_from_wifi_mac(other_wifi)

    # P2：--elf 与 --expect-build-sha 必须成对，禁止"只给一半"导致检查被静默跳过
    if bool(args.elf) != bool(args.expect_build_sha):
        print("[FAIL] --elf 与 --expect-build-sha 必须同时提供或同时省略")
        return 2

    binp = Path(args.bin)
    if not binp.is_file():
        print(f"[FAIL] 冻结 BIN 不存在: {binp}")
        return 2
    bin_sha = hashlib.sha256(binp.read_bytes()).hexdigest()
    expect_bin = args.expect_bin_sha256.strip().lower()
    bin_ok = (bin_sha == expect_bin)

    build_sha = None
    elf_sha = None
    if args.elf:
        elf = Path(args.elf)
        if not elf.is_file():
            print(f"[FAIL] 配套 ELF 不存在: {elf}")
            return 2
        elf_sha = hashlib.sha256(elf.read_bytes()).hexdigest()
        build_sha = elf_sha[:9]

    print("=== 烧录前四元组 ===")
    print(f"  端口          : {args.port}  (日志名 {name})")
    print(f"  日志          : {loginfo.get('file')}")
    for tag, ci in (("目标端口", capinfo), ("另一端口", capother)):
        print(f"  [{tag} capture] pid={ci.get('pid')} 存活={ci.get('proc_exists')} "
              f"argc={ci.get('argc')} argc_ok={ci.get('argc_ok')} "
              f"script_ok={ci.get('script_ok')} port_arg_ok={ci.get('port_arg_ok')} "
              f"log_arg_ok={ci.get('log_arg_ok')} fd持端口={ci.get('fd_holds_port')} "
              f"fd持日志={ci.get('fd_holds_log')} running={ci.get('running')}")
        print(f"    argv={ci.get('argv')}")
        print(f"    script_real={ci.get('script_real')}")
        print(f"    expect_script_real={ci.get('expected_script_real')}")
        print(f"    argv_log_real={ci.get('argv_log_real')} "
              f"expect_log_real={ci.get('expect_log_real')}")
    for tag, li in (("目标端口", loginfo), ("另一端口", other_info)):
        print(f"  [{tag}] {li.get('file')}")
        print(f"    存在={li.get('exists')} size={li.get('size')} "
              f"age={li.get('age_sec')}s fresh={li.get('fresh')} "
              f"sha256[:16]={li.get('sha256_16')} "
              f"boot_marker={li.get('boot_marker')} "
              f"client_id_after_boot={li.get('client_id_after_boot')}")
    print(f"  端口观测 WiFi MAC : {observed_wifi or '未知'}  → {observed_id}"
          f"（BLE≈{ble_from_wifi_mac(observed_wifi)}）")
    print(f"  期望 device_id    : {args.expect_device_id}")
    print(f"  冻结 BIN      : {binp}")
    print(f"    sha256      = {bin_sha}")
    print(f"    期望 sha256 = {expect_bin}  一致={bin_ok}")
    if elf_sha:
        print(f"  配套 ELF      : {args.elf}  sha256[:16]={elf_sha[:16]}  "
              f"build_sha={build_sha}")
        print(f"    期望 build_sha: {args.expect_build_sha}")
    print("====================")

    ok = True
    # 主证据：录制进程仍存活并持有串口；mtime 只作辅助
    if not capinfo.get("running"):
        print(f"[FAIL] 目标端口录制链不完整（存活={capinfo.get('proc_exists')} "
              f"script={capinfo.get('script_ok')} port_arg={capinfo.get('port_arg_ok')} "
              f"log_arg={capinfo.get('log_arg_ok')} fd端口={capinfo.get('fd_holds_port')} "
              f"fd日志={capinfo.get('fd_holds_log')}）——PID→进程→串口→日志 未闭合")
        ok = False
    if not capother.get("running"):
        print(f"[FAIL] 另一端口录制链不完整（存活={capother.get('proc_exists')} "
              f"script={capother.get('script_ok')} port_arg={capother.get('port_arg_ok')} "
              f"log_arg={capother.get('log_arg_ok')} fd端口={capother.get('fd_holds_port')} "
              f"fd日志={capother.get('fd_holds_log')}）")
        ok = False
    if not loginfo.get("exists"):
        print("[FAIL] 日志文件不存在（本次会话未采集）")
        ok = False
    elif not loginfo.get("fresh"):
        print(f"[FAIL] 日志过期（{loginfo.get('age_sec')}s > {args.max_age_sec}s）——"
              f"映射证据不是本次会话，请重新采集后再过门禁")
        ok = False
    if not loginfo.get("boot_marker"):
        print("[FAIL] 目标端口日志没有 boot 标记（rst:0x1），无法证明身份属于当前上电会话")
        ok = False
    if observed_wifi is None:
        print("[FAIL] 目标端口最近一次 boot 之后找不到 client_id=iothub-<WiFiMAC>")
        ok = False
    elif observed_id.lower() != args.expect_device_id.lower():
        print(f"[FAIL] 端口设备不符：观测 {observed_id} ≠ 期望 {args.expect_device_id}")
        ok = False
    # P1-2：另一端口也必须提供"当前会话有效身份"，且与目标设备不同
    if not other_info.get("exists"):
        print(f"[FAIL] 另一端口日志缺失（{other}）——无法形成双端口映射证据")
        ok = False
    elif not other_info.get("fresh"):
        print(f"[FAIL] 另一端口日志过期（{other}，{other_info.get('age_sec')}s）")
        ok = False
    elif not other_info.get("boot_marker"):
        print(f"[FAIL] 另一端口日志没有 boot 标记（{other}）")
        ok = False
    elif other_wifi is None:
        print(f"[FAIL] 另一端口 boot 之后无有效 client_id（{other}）")
        ok = False
    elif other_id.lower() == (observed_id or "").lower():
        print(f"[FAIL] 两端口指向同一设备（{other_id}）——映射证据不可信")
        ok = False
    if not bin_ok:
        print(f"[FAIL] 冻结 BIN 身份不符：实际 {bin_sha} ≠ 期望 {expect_bin}")
        ok = False
    if args.elf and args.expect_build_sha and \
            build_sha.lower() != args.expect_build_sha.lower():
        print(f"[FAIL] 配套 ELF 身份不符：实际 {build_sha} ≠ 期望 {args.expect_build_sha}")
        ok = False
    if not ok:
        print("[STOP] 拒绝烧录（四元组核验未通过）")
        return 1

    if args.emit_anchor:
        import port_identity as PI

        if not args.mac_record:
            print("[FAIL] --emit-anchor 必须同时提供 --mac-record（锚点不得静默写入 mac=null）")
            return 2
        ok_rec, mac_or_err, rec = PI.load_mac_record(
            args.mac_record, args.port, args.log_dir)
        if not ok_rec:
            print(f"[FAIL] MAC 记录校验失败：{mac_or_err}")
            return 2
        try:
            anchor = PI.build_anchor(args.port, args.log_dir, mac=mac_or_err)
            if not anchor.get("mac_ok") or not anchor.get("mac"):
                print("[FAIL] 锚点生成结果 mac_ok/mac 无效（拒绝写出）")
                return 2
            anchor["mac_record"] = str(args.mac_record)
            anchor["mac_record_read_at"] = rec.get("read_at")
            PI.write_anchor(args.emit_anchor, anchor)
            print(f"  锚点已写出：{args.emit_anchor}  mac={anchor.get('mac')} "
                  f"from_record={rec.get('read_at')} checked_at={anchor.get('checked_at')}")
        except Exception as exc:  # noqa: BLE001
            print(f"[FAIL] 锚点生成失败（{type(exc).__name__}: {exc}）")
            return 2

    print("[OK] 门禁通过（设备身份 + 冻结 BIN 字节身份）")
    print("     烧录必须使用该冻结 BIN（不得重建），并在写入前紧邻复查 sha256；")
    print("     烧录后仍以 device.info.build_sha 作固件内身份一致性检查（非字节级证明）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
