#!/usr/bin/env python3
"""Simple serial log capture for batch 0 verification.

2026-10-16(1.3-49)修复:打开串口时**不拉 DTR/RTS** —— 这两根线在 ESP32
自动复位电路上分别接 EN/GPIO0,open() 默认拉低会让板子复位、某些时序下
甚至停在下载模式(表现为"抓包 0 字节 + 板子网页同时不可用"，验收中实际
踩到过)。这里 open 前显式置 False,抓包只读、不扰动目标。

2026-10-17(1.3-60c)补充:**短抓包仍会复位板子** —— close() 时 termios 的
HUPCL 会拉低 DTR,open() 时驱动设置初始线态也可能产生一次跳变。已清 HUPCL
并在退出前保持 DTR/RTS;但 open 仍可能触发一次复位。使用规范见
问题与版本优化.md 附录 B:测试期间保持一路长抓包,不要反复短抓;换固件前先停抓包。

2026-11-06 补充:**已有一路在跑时不要再起第二个** —— pyserial 不设 TIOCEXCL,
第二个进程仍能 open 并重新拉一次线序,实测会把板子复位一次(POWERON_RESET),
且两个读者会把字节流拆开(两边都拿不全)。要换输出文件,先结束旧进程、再起新的;
一次会话只允许一个读者。
"""
import sys
import termios
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
OUT = sys.argv[2] if len(sys.argv) > 2 else "/tmp/serial.log"
BAUD = 115200

ser = serial.Serial()
ser.port = PORT
ser.baudrate = BAUD
ser.timeout = 1
ser.dtr = False
ser.rts = False

# ---- 采集存活锁（S6 门禁用）：<log>.lock 上的 flock(LOCK_EX) ----
# 目的：让回滚门禁在不读 /proc、不需要 root 的情况下**可证明**地判断
# "采集是否仍在运行"。锁随进程退出自动释放。
import fcntl as _fcntl
_lock_f = open(OUT + ".lock", "a+")
try:
    _fcntl.flock(_lock_f, _fcntl.LOCK_EX | _fcntl.LOCK_NB)
except OSError:
    print("capture: another capture already holds %s.lock — 拒绝启动" % OUT,
          flush=True)
    raise SystemExit(3)

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
