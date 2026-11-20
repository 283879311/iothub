#!/usr/bin/env python3
"""Simple serial log capture for batch 0 verification.

2026-10-16(1.3-49)修复:打开串口时**不拉 DTR/RTS** —— 这两根线在 ESP32
自动复位电路上分别接 EN/GPIO0,open() 默认拉低会让板子复位、某些时序下
甚至停在下载模式(表现为"抓包 0 字节 + 板子网页同时不可用"，验收中实际
踩到过)。这里 open 前显式置 False,抓包只读、不扰动目标。

2026-10-17(1.3-60c)补充:**短抓包仍会复位板子** —— close() 时 termios 的
HUPCL 会拉低 DTR,open() 时驱动设置初始线态也可能产生一次跳变。已清 HUPCL
并在退出前保持 DTR/RTS;但 open 仍可能触发一次复位。使用规范:测试期间保持一路长抓包,不要反复短抓;换固件前先停抓包。

2026-11-06 补充:**已有一路在跑时不要再起第二个** —— pyserial 不设 TIOCEXCL,
第二个进程仍能 open 并重新拉一次线序,实测会把板子复位一次(POWERON_RESET),
且两个读者会把字节流拆开(两边都拿不全)。要换输出文件,先结束旧进程、再起新的;
一次会话只允许一个读者。

2026-11-15(S6 门禁 v10):启动即取**按串口唯一**的协作锁
(`port_identity.port_lock_path()`，如 /run/lock/iothub_serial_ttyUSB1.lock)。
取不到 → 退出码 3，不打开串口。门禁用同一路径试探/持有该锁，从而在不读
/proc、不需要 root 的前提下判定"协作采集是否在运行"。
边界：只约束遵循本协议的工具，**不能**排除 minicom/手工 cat 等外部持有者。
"""
import fcntl
import os
import sys
import termios

_argv = [a for a in sys.argv[1:] if a != "--lock-probe"]
PROBE = (len(_argv) != len(sys.argv) - 1)      # 只验锁、不开串口（供离线测试）
PORT = _argv[0] if len(_argv) > 0 else "/dev/ttyUSB0"
OUT = _argv[1] if len(_argv) > 1 else "/tmp/serial.log"
BAUD = 115200


def _acquire_locks(port, out):
    """取协作锁：**按串口唯一**（主）+ 按日志文件（旧版兼容）。

    端口锁路径由 `port_identity.port_lock_path()` 计算，门禁使用同一函数——
    这是"同一串口只允许一个协作使用者"的唯一判据。锁随进程退出自动释放
    （含 SIGTERM/SIGKILL：内核关闭 fd 即解锁）。
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import port_identity as PI
    held = []
    for path, label in ((PI.port_lock_path(port), "端口"),
                        (out + ".lock", "日志")):
        try:
            f = PI.open_lock_file(path)
        except OSError as exc:
            print("capture: 无法打开%s锁 %s（%s）— 拒绝启动"
                  % (label, path, exc), flush=True)
            raise SystemExit(3)
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            print("capture: %s锁已被持有：%s — 拒绝启动（同一串口只允许一个采集）"
                  % (label, path), flush=True)
            raise SystemExit(3)
        held.append((path, f))
    return held


_locks = _acquire_locks(PORT, OUT)

if PROBE:
    print("capture: lock-probe ok port=%s locks=%s"
          % (PORT, ",".join(p for p, _f in _locks)), flush=True)
    raise SystemExit(0)

import serial  # noqa: E402  （probe 模式不需要 pyserial）

ser = serial.Serial()
ser.port = PORT
ser.baudrate = BAUD
ser.timeout = 1
ser.dtr = False
ser.rts = False

with ser, open(OUT, "ab") as f:
    # 2026-10-17(1.3-60): 关闭 HUPCL —— 否则进程退出 close() 时驱动会拉低 DTR,
    # ESP32 自动复位电路收到 EN 脉冲,表现为"每次短抓包都 POWERON_RESET"。
    try:
        attrs = termios.tcgetattr(ser.fileno())
        attrs[2] &= ~termios.HUPCL
        termios.tcsetattr(ser.fileno(), termios.TCSANOW, attrs)
    except (termios.error, OSError):
        pass
    # 2026-11-07:每 60s 写一行心跳(# 注释行,SHA256SUMS 的 sha256sum -c 会
    # 跳过非 "哈希 文件名" 形态的行……不会——sha256sum -c 遇非法行会报错。
    # 故心跳写进独立 <logfile>.heartbeat,不污染 SUMS 校验对象)。
    import time as _t
    _last_beat = 0.0
    # 2026-11-08:ModemManager 周期性探测 ttyUSB 会造成 SerialException
    # ("readiness but no data / multiple access")——旧版直接死亡(三次抓包
    # 事故的根因)。改为:异常后重开端口继续(探测瞬间可能丢几字节,
    # 但抓包不死);重开前恢复 DTR/RTS 电平,防止 EN 复位。
    _reopens = 0
    while True:
        try:
            if not ser.is_open:
                # DTR/RTS 必须在 open 之前置低——open() 默认拉高 DTR,
                # EN 脉冲会把板子 POWERON 复位(2026-11-08 干净环境 3 连复位根因)
                ser.dtr = False
                ser.rts = False
                ser.open()
            while True:
                data = ser.read(4096)
                if data:
                    f.write(data)
                    f.flush()
                now = _t.time()
                if now - _last_beat >= 60.0:
                    _last_beat = now
                    try:
                        with open(OUT + ".heartbeat", "a") as hf:
                            hf.write("# heartbeat pid=%d bytes=%d reopens=%d %s\n" % (
                                __import__("os").getpid(), f.tell(), _reopens,
                                _t.strftime("%Y-%m-%dT%H:%M:%S")))
                    except OSError:
                        pass
        except serial.SerialException as e:
            _reopens += 1
            print("capture: SerialException #%d (%s) — reopening in 2s" % (
                _reopens, str(e)[:80]), flush=True)
            try:
                ser.close()
            except Exception:
                pass
            _t.sleep(2.0)
            if _reopens > 200:
                print("capture: too many reopens, giving up", flush=True)
                break
    # 退出:恢复电平(HUPCL 已清,通常无 EN 脉冲;防御性保留)
    try:
        ser.dtr = False
        ser.rts = False
        ser.close()
    except (serial.SerialException, OSError):
        pass
