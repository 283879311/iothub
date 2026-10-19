#!/usr/bin/env python3
"""IotHub BLE 数据面(NUS)验收 harness —— 问题与版本优化.md 1.3-03。

PC 侧充当 BLE central(bleak + BlueZ),对设备 NUS 服务做写入/回显/长压测,
并用设备的 /api/v1/device/status(admin 认证)读回 data_pipe 统计做断言。

前置:
    pip install bleak                     # 已在本机 venv 中可用
    sudo apt install bluez && sudo systemctl start bluetooth   # Linux 需 BlueZ 提供 org.bluez

用法:
    python3 tools/ble_nus/ble_nus.py scan
    python3 tools/ble_nus/ble_nus.py echo 5
    python3 tools/ble_nus/ble_nus.py soak 90 [--interval 0.012]
    python3 tools/ble_nus/ble_nus.py uartface          # 需 GPIO17<->GPIO18 跳线

环境变量:IOTHUB_HOST(默认 http://192.168.1.9)、IOTHUB_USER/IOTHUB_PASS(默认 admin/123456)。

判定口径:统计为累计值,断言一律比较"前后差值"。UART 线速 9600 8N1 ≈ 960 B/s,
每包 8 字节时间隔需 >= ~8.4ms 才不超线速;超线速出现 overrun/dropped 属**真实背压**,
不代表 bug —— 验收要求的是"不超线速时恒为 0"。
"""
import argparse
import asyncio
import base64
import http.cookiejar
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # 写(central -> device)
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # 通知(device -> central)
NAME_PREFIX = "IotHubBLE-"

HOST = os.environ.get("IOTHUB_HOST", "http://192.168.1.9")
USER = os.environ.get("IOTHUB_USER", "admin")
PASS = os.environ.get("IOTHUB_PASS", "123456")

_t_total = 0
_t_fail = 0

# 1.3-33 起 /api/v1/device/status 仅接受 Cookie 会话(拒绝 Basic):
# 先 POST 表单登录拿 iothub_sid,之后所有请求都经 opener 自动带 Cookie。
# Basic 头保留 —— uart/loopback 等接口仍接受 Basic,两种凭据并存无害。
_cookiejar = http.cookiejar.CookieJar()
_opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(_cookiejar))


def check(name, got, want):
    global _t_total, _t_fail
    _t_total += 1
    good = (got == want)
    if not good:
        _t_fail += 1
    print("  [%s] %s: got=%s want=%s" % ("PASS" if good else "FAIL", name, got, want))
    return good


def login():
    data = urllib.parse.urlencode({"username": USER, "password": PASS}).encode()
    req = urllib.request.Request(HOST + "/api/v1/auth/login", data=data, method="POST")
    req.add_header("Content-Type", "application/x-www-form-urlencoded")
    try:
        with _opener.open(req, timeout=10) as r:
            r.read()
    except urllib.error.HTTPError as e:
        e.read()   # 302 属预期(登录成功跳转 /config)
    ok = any(c.name == "iothub_sid" for c in _cookiejar)
    print("[auth] login %s (cookie=%s)" % ("OK" if ok else "FAIL", ok))
    return ok


def http_json(method, path, payload=None):
    req = urllib.request.Request(HOST + path, method=method)
    tok = base64.b64encode(("%s:%s" % (USER, PASS)).encode()).decode()
    req.add_header("Authorization", "Basic " + tok)
    body = None
    if payload is not None:
        body = json.dumps(payload).encode()
        req.add_header("Content-Type", "application/json")
    with _opener.open(req, body, timeout=10) as r:
        return json.loads(r.read().decode())


def pipe_stats():
    return http_json("GET", "/api/v1/device/status")["data_pipe"]


def bt_connected():
    """True/False = 有无 central 连接;None = 固件无该字段(status.bluetooth.connected)。

    意义:`app_ble_gatts_notify()` 在**未连接或未订阅**时返回 false ⇒ 只有 connected=True
    时 `tx_ble` 增长才算"送达证据";否则应改用 `dropped` 作为"已路由到 BLE 出站"的代理。"""
    try:
        return bool(http_json("GET", "/api/v1/device/status")["bluetooth"]["connected"])
    except KeyError:
        return None


def set_loopback(on):
    http_json("POST", "/api/v1/data/loopback", {"enabled": bool(on)})


def diff(a, b):
    return {k: b[k] - a[k]
            for k in ("rx_uart1", "rx_ble", "tx_uart1", "tx_ble", "dropped", "overrun")}


async def find_device(timeout=20):
    from bleak import BleakScanner

    print("[scan] 查找 name 前缀 %s ..." % NAME_PREFIX)
    devs = await BleakScanner.discover(timeout=timeout)
    for d in devs:
        if (d.name or "").startswith(NAME_PREFIX):
            print("[scan] 命中: %s %s" % (d.name, d.address))
            return d
    print("[scan] 未找到。可见设备: %s" % [(d.name, d.address) for d in devs])
    return None


async def with_client(fn, timeout=20):
    from bleak import BleakClient

    dev = await find_device(timeout)
    if dev is None:
        print("!! 找不到设备:确认设备在广播(status.bluetooth.adv_active=true)")
        return 2
    async with BleakClient(dev) as cli:
        print("[conn] connected=%s" % cli.is_connected)
        await fn(cli)
    return 0


async def cmd_echo(args):
    """回环态:每写一包都应从 TX 通知原样回来(验证 收->管道->发 闭环)。"""
    payloads = [("ECHO%03d-" % i).encode() for i in range(args.n)]
    got = bytearray()
    want = sum(len(p) for p in payloads)

    async def run(cli):
        def on_notify(_c, data):
            got.extend(data)
            print("[notify] +%d (累计 %d)" % (len(data), len(got)))

        set_loopback(True)
        before = pipe_stats()
        await cli.start_notify(NUS_TX, on_notify)
        for p in payloads:
            await cli.write_gatt_char(NUS_RX, p, response=False)
            await asyncio.sleep(0.3)
        await asyncio.sleep(1.5)
        await cli.stop_notify(NUS_TX)
        d = diff(before, pipe_stats())
        set_loopback(False)
        print("[echo] 写入 %d 字节,期望回显 %d;BLE 侧实收 %d" % (want, want, len(got)))
        print("[echo] pipe 差值: %s" % d)
        check("回显字节数一致", len(got), want)
        check("rx_ble 差值=写入量", d["rx_ble"], want)
        check("tx_ble 差值=写入量(回环)", d["tx_ble"], want)
        check("dropped 不增长", d["dropped"], 0)

    rc = await with_client(run)
    print("结果: 共 %d 例, 失败 %d" % (_t_total, _t_fail))
    return 0 if (_t_fail == 0 and rc == 0) else 1


async def cmd_soak(args):
    """交叉路由态(BLE->UART1):持续写入,校验 1:1 且不超线速时无丢帧。"""
    payload_len = 8

    async def run(cli):
        before = pipe_stats()
        t0 = time.time()
        sent = 0
        while time.time() - t0 < args.seconds:
            await cli.write_gatt_char(NUS_RX, ("S%07d" % sent).encode()[:payload_len],
                                      response=False)
            sent += payload_len
            await asyncio.sleep(args.interval)
        dur = time.time() - t0
        await asyncio.sleep(1.5)
        d = diff(before, pipe_stats())
        print("[soak] %.1fs 写入 %d 字节 (%.0f B/s, 间隔 %.0fms)"
              % (dur, sent, sent / max(dur, 0.001), args.interval * 1000))
        print("[soak] pipe 差值: %s" % d)
        check("rx_ble 差值=写入量", d["rx_ble"], sent)
        check("tx_uart1 差值=写入量(交叉路由到 UART)", d["tx_uart1"], sent)
        check("dropped 不增长", d["dropped"], 0)
        check("overrun 不增长", d["overrun"], 0)

    rc = await with_client(run)
    print("结果: 共 %d 例, 失败 %d" % (_t_total, _t_fail))
    return 0 if (_t_fail == 0 and rc == 0) else 1


async def cmd_uartface(args):
    """反向链路 UART1->BLE:需 GPIO17(TX) 与 GPIO18(RX) 短接。"""
    body = {"encoding": "hex", "data": "AA 55 01 02 03 04 05 06 07 08"}
    n = 10
    want = n * 10

    async def run(cli):
        got = bytearray()

        def on_notify(_c, data):
            got.extend(data)

        set_loopback(False)          # 交叉路由:UART1 -> BLE
        await cli.start_notify(NUS_TX, on_notify)
        before = pipe_stats()
        conn = bt_connected()
        seq0 = http_json("GET", "/api/v1/config/uart").get("tx_sequence", 0)
        for _ in range(n):
            http_json("POST", "/api/v1/uart/send", body)
            await asyncio.sleep(0.35)   # 10B @9600 ≈ 10.4ms,留足余量
        await asyncio.sleep(1.5)
        await cli.stop_notify(NUS_TX)
        d = diff(before, pipe_stats())
        u = http_json("GET", "/api/v1/config/uart")
        print("[uartface] 发送 %d 次 x %d 字节 = %d;BLE 侧实收 %d"
              % (n, len(body["data"].split()), want, len(got)))
        print("[uartface] central connected = %s" % conn)
        print("[uartface] pipe 差值: %s" % d)
        # ① UART 接口自身的写计数才是"GPIO17 真发了"的证据:
        #    交叉路由下管道不会写 UART ⇒ tx_uart1 恒为 0,不能拿它当写入证据。
        check("UART 写次数 +n", u.get("tx_sequence", 0) - seq0, n)
        check("last_bytes_sent=单包长度", u.get("last_bytes_sent", 0), len(body["data"].split()))
        # ② 跳线回灌 ⇒ 管道收到 UART1 RX
        check("rx_uart1 差值=发送量(跳线回灌)", d["rx_uart1"], want)
        check("tx_uart1 保持 0(交叉路由不写 UART)", d["tx_uart1"], 0)
        # ③ 路由到 BLE:有订阅者看 tx_ble,无订阅者用 dropped 代理
        if conn is False:
            print("[uartface] 无 central ⇒ 用 dropped 代理\"已路由到 BLE 出站\"(tx_ble 不适用)")
            check("tx_ble+dropped 差值=发送量(路由已触发)", d["tx_ble"] + d["dropped"], want)
        else:
            check("tx_ble 差值=发送量(投递给订阅的 central)", d["tx_ble"], want)
            check("dropped 不增长", d["dropped"], 0)

    rc = await with_client(run)
    print("结果: 共 %d 例, 失败 %d" % (_t_total, _t_fail))
    return 0 if (_t_fail == 0 and rc == 0) else 1


def main():
    ap = argparse.ArgumentParser(description="IotHub BLE NUS 验收 harness(1.3-03)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("scan", help="只扫描并列出可见设备")
    p = sub.add_parser("echo", help="回环态写入并校验回显")
    p.add_argument("n", type=int, nargs="?", default=5)
    p = sub.add_parser("soak", help="交叉路由态长压测 BLE->UART1")
    p.add_argument("seconds", type=float, nargs="?", default=90)
    p.add_argument("--interval", type=float, default=0.012)
    sub.add_parser("uartface", help="跳线态校验 UART1->BLE")

    args = ap.parse_args()
    if args.cmd == "scan":
        return 0 if asyncio.run(find_device()) else 2
    if not login():
        return 3
    if args.cmd == "echo":
        return asyncio.run(cmd_echo(args))
    if args.cmd == "soak":
        return asyncio.run(cmd_soak(args))
    return asyncio.run(cmd_uartface(args))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
