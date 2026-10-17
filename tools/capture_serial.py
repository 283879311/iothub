#!/usr/bin/env python3
"""Simple serial log capture for batch 0 verification.

2026-10-16(1.3-49)修复:打开串口时**不拉 DTR/RTS** —— 这两根线在 ESP32
自动复位电路上分别接 EN/GPIO0,open() 默认拉低会让板子复位、某些时序下
甚至停在下载模式(表现为"抓包 0 字节 + 板子网页同时不可用"，验收中实际
踩到过)。这里 open 前显式置 False,抓包只读、不扰动目标。

2026-10-17(1.3-60c)补充:**短抓包仍会复位板子** —— close() 时 termios 的
HUPCL 会拉低 DTR,open() 时驱动设置初始线态也可能产生一次跳变。已清 HUPCL
并在退出前保持 DTR/RTS;但 open 仍可能触发一次复位。使用规范见
docs/serial_capture.md:测试期间保持一路长抓包,不要反复短抓;换固件前先停抓包。
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

with ser, open(OUT, "ab") as f:
    # 2026-10-17(1.3-60): 关闭 HUPCL —— 否则进程退出 close() 时驱动会拉低 DTR,
    # ESP32 自动复位电路收到 EN 脉冲,表现为"每次短抓包都 POWERON_RESET"。
    try:
        attrs = termios.tcgetattr(ser.fileno())
        attrs[2] &= ~termios.HUPCL
        termios.tcsetattr(ser.fileno(), termios.TCSANOW, attrs)
    except (termios.error, OSError):
        pass
    try:
        while True:
            data = ser.read(4096)
            if data:
                f.write(data)
                f.flush()
    finally:
        try:
            ser.dtr = False
            ser.rts = False
        except (serial.SerialException, OSError):
            pass
