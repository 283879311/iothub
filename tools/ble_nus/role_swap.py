#!/usr/bin/env python3
"""A/B BLE 角色互换验收（peripheral <-> central）。

通过两块板自己的 HTTP 配置接口切换 bt_role，并轮询两侧 status 判定：
  1. peripheral 侧：runtime_mode=ble、adv_active=true；
  2. central 侧：runtime_mode=central、扫描并连上 peripheral、last_error 清空；
  3. peripheral 侧：connected=true（central 已连入）；
  4. 恢复 central 为 ble 后：runtime_mode=ble、adv_active=true，peripheral 断开。

只改蓝牙角色，不碰 WiFi/MQTT/UART/路由；失败或 Ctrl-C 时 finally 会把
central 板恢复为 ble。默认 A=192.168.1.213、B=192.168.1.42。

用法：
    python3 tools/ble_nus/role_swap.py --central B --peripheral A
    python3 tools/ble_nus/role_swap.py --central A --peripheral B

凭据环境变量：IOTHUB_USER/IOTHUB_PASS（默认 admin/123456）。
"""
import argparse
import http.cookiejar
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

DEFAULT_HOSTS = {
    "A": os.environ.get("IOTHUB_A_HOST", "http://192.168.1.213"),
    "B": os.environ.get("IOTHUB_B_HOST", "http://192.168.1.42"),
}
USER = os.environ.get("IOTHUB_USER", "admin")
PASS = os.environ.get("IOTHUB_PASS", "123456")


class Board:
    def __init__(self, name, host, user, password):
        self.name = name
        self.host = host
        self.user = user
        self.password = password
        self.jar = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(self.jar))
        self.logged_in = False

    def _login(self):
        data = urllib.parse.urlencode(
            {"username": self.user, "password": self.password}).encode()
        req = urllib.request.Request(
            self.host + "/api/v1/auth/login", data=data, method="POST")
        req.add_header("Content-Type", "application/x-www-form-urlencoded")
        try:
            with self.opener.open(req, timeout=8) as resp:
                resp.read()
        except urllib.error.HTTPError as exc:
            # 302/401 均可能；Cookie 是否下发以下面判断为准
            exc.read()
        self.logged_in = any(c.name == "iothub_sid" for c in self.jar)
        return self.logged_in

    def _request(self, method, path, payload=None, timeout=8):
        body = None
        headers = {}
        if payload is not None:
            body = json.dumps(payload).encode()
            headers["Content-Type"] = "application/json"
        for attempt in range(2):
            if not self.logged_in:
                self._login()
            req = urllib.request.Request(self.host + path, data=body,
                                         method=method, headers=headers)
            try:
                with self.opener.open(req, timeout=timeout) as resp:
                    raw = resp.read()
                    return json.loads(raw.decode()) if raw else {}
            except urllib.error.HTTPError as exc:
                if exc.code == 401 and attempt == 0:
                    self.logged_in = False
                    continue
                raise
        raise RuntimeError("request failed")

    def status(self):
        return self._request("GET", "/api/v1/device/status")

    def set_mode(self, mode):
        # 配置保存后板子约 1.5s 重启；响应可能在重启前到达，忽略断开。
        try:
            return self._request("PUT", "/api/v1/config/bluetooth", {"mode": mode})
        except (urllib.error.URLError, OSError):
            return None


def bt(status):
    return status.get("bluetooth", {})


def wait_http(board, timeout_s):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            st = board.status()
            return st
        except (urllib.error.URLError, OSError, TimeoutError, ValueError):
            board.logged_in = False
            time.sleep(1)
    return None


def wait_until(board, predicate, timeout_s, what):
    deadline = time.time() + timeout_s
    last = None
    while time.time() < deadline:
        try:
            last = board.status()
            if predicate(last):
                return last
        except (urllib.error.URLError, OSError, TimeoutError, ValueError):
            board.logged_in = False
        time.sleep(1)
    print("  !! %s 等待超时：%s（最后 bluetooth=%s）"
          % (board.name, what, bt(last) if last else "unreachable"))
    return None


def snapshot(board):
    st = board.status()
    b = bt(st)
    return {
        "mode": b.get("mode"),
        "runtime": b.get("runtime_mode"),
        "adv": b.get("adv_active"),
        "connected": b.get("connected"),
        "error": b.get("last_error"),
        "ip": st.get("network", {}).get("sta_ip"),
    }


def run_pair(central, peripheral, connect_timeout, restore_timeout):
    print("[1/4] 检查 peripheral=%s 初始状态" % peripheral.name)
    p0 = wait_until(peripheral, lambda s: bt(s).get("runtime_mode") == "ble"
                    and bt(s).get("adv_active") is True, 20,
                    "peripheral 广播")
    if p0 is None:
        return False
    print("  %s: %s" % (peripheral.name, snapshot(peripheral)))

    print("[2/4] 切换 central=%s 为 central 并等待连接" % central.name)
    central.set_mode("central")
    c_st = wait_until(central, lambda s: bt(s).get("runtime_mode") == "central"
                      and bt(s).get("connected") is True, connect_timeout,
                      "central 连接 peripheral")
    if c_st is None:
        return False
    p_st = wait_until(peripheral, lambda s: bt(s).get("connected") is True,
                      15, "peripheral 侧看到 central")
    if p_st is None:
        return False
    cs, ps = snapshot(central), snapshot(peripheral)
    print("  central=%s: %s" % (central.name, cs))
    print("  peripheral=%s: %s" % (peripheral.name, ps))
    if cs["error"] not in ("", None):
        print("  !! central last_error=%r（连接已建立但状态串未清空）" % cs["error"])
        return False

    print("[3/4] 恢复 central=%s 为 ble（角色互换回到初始）" % central.name)
    central.set_mode("ble")
    r_st = wait_until(central, lambda s: bt(s).get("runtime_mode") == "ble"
                      and bt(s).get("adv_active") is True, restore_timeout,
                      "central 恢复广播")
    if r_st is None:
        return False
    p_off = wait_until(peripheral, lambda s: bt(s).get("connected") is False,
                       20, "peripheral 断开")
    p_adv = wait_until(peripheral, lambda s: bt(s).get("adv_active") is True,
                       15, "peripheral 断开后重广播")
    print("  central=%s: %s" % (central.name, snapshot(central)))
    print("  peripheral=%s: %s" % (peripheral.name, snapshot(peripheral)))
    if p_off is None or p_adv is None:
        return False

    print("[4/4] PASS：%s peripheral -> %s central -> %s peripheral"
          % (peripheral.name, central.name, central.name))
    return True


def main():
    ap = argparse.ArgumentParser(description="A/B BLE 角色互换验收")
    ap.add_argument("--central", choices=("A", "B"), default="B")
    ap.add_argument("--peripheral", choices=("A", "B"), default="A")
    ap.add_argument("--connect-timeout", type=float, default=90)
    ap.add_argument("--restore-timeout", type=float, default=40)
    ap.add_argument("--user", default=USER)
    ap.add_argument("--password", default=PASS)
    args = ap.parse_args()
    if args.central == args.peripheral:
        print("!! central 与 peripheral 不能是同一块板", file=sys.stderr)
        return 2

    boards = {k: Board(k, v, args.user, args.password)
              for k, v in DEFAULT_HOSTS.items()}
    central, peripheral = boards[args.central], boards[args.peripheral]

    for b in (central, peripheral):
        try:
            st = wait_http(b, 15)
            if st is None:
                print("!! %s 不可达：%s" % (b.name, b.host), file=sys.stderr)
                return 2
            print("%s (%s) 初始 bluetooth=%s" % (b.name, b.host, bt(st)))
        except Exception as exc:  # noqa: BLE001 - CLI 诊断
            print("!! %s 初始化失败：%s" % (b.name, exc), file=sys.stderr)
            return 2

    ok = False
    try:
        ok = run_pair(central, peripheral,
                      args.connect_timeout, args.restore_timeout)
    finally:
        # 无论成功、失败还是 Ctrl-C，都把 central 板恢复为 ble。
        try:
            st = wait_http(central, 20)
            if st is None or bt(st).get("mode") != "ble":
                print("  [restore] 尝试把 %s 恢复为 ble" % central.name)
                central.set_mode("ble")
                wait_http(central, args.restore_timeout)
        except Exception as exc:  # noqa: BLE001
            print("  !! 恢复 %s 失败：%s；请手动 PUT mode=ble" % (central.name, exc),
                  file=sys.stderr)
    print("结果：%s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
