#!/usr/bin/env python3
"""本地最小路由型 MQTT 3.1.1 broker（DEF-NET-001 41 例实跑用）。

与 mqtt_ack_gate_inject.py 的区别：那个是单设备注入器，本文件是
**多客户端按主题路由**的最小 broker —— 41 例套件(harness)与设备互为
发布/订阅两端，需要真正的 PUB/SUB 转发。

支持：CONNECT/CONNACK、SUBSCRIBE/SUBACK(授予 QoS0)、PUBLISH QoS0/1
(QoS1 对发布者回 PUBACK；向订阅者**降级 QoS0 投递**，合法)、
PINGREQ/PINGRESP、DISCONNECT。主题匹配按 MQTT 规范支持 '+' 与 '#'。

不做：QoS2、retained、will、鉴权、TLS、keepalive 强制。仅限本地测试床。

用法：
  python3 tools/broker_local/mini_broker.py --port 21883 [--log-every 50]
"""
from __future__ import annotations

import argparse
import socket
import struct
import sys
import threading
import time


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def enc_remaining(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        if n > 0:
            b |= 0x80
        out.append(b)
        if n == 0:
            return bytes(out)


def enc_str(s: str) -> bytes:
    b = s.encode()
    return struct.pack("!H", len(b)) + b


def topic_matches(pattern: str, topic: str) -> bool:
    p = pattern.split("/")
    t = topic.split("/")
    for i, seg in enumerate(p):
        if seg == "#":
            return True
        if i >= len(t):
            return False
        if seg != "+" and seg != t[i]:
            return False
    return len(p) == len(t)


class Client:
    def __init__(self, sock: socket.socket, addr):
        self.sock = sock
        self.addr = addr
        self.cid = "?"
        self.filters: list[str] = []
        self.lock = threading.Lock()
        self.buf = bytearray()

    def send(self, data: bytes) -> None:
        with self.lock:
            self.sock.sendall(data)


class MiniBroker:
    def __init__(self, port: int, log_every: int = 50):
        self.port = port
        self.log_every = log_every
        self.msg_count = 0
        self.clients: dict[int, Client] = {}
        self.lock = threading.Lock()
        self.next_cid = 0

    def _route(self, topic: str, payload: bytes) -> int:
        delivered = 0
        with self.lock:
            targets = list(self.clients.values())
        for cli in targets:
            for f in list(cli.filters):
                if topic_matches(f, topic):
                    try:
                        # 降级 QoS0 投递（合法；设备端发布仍按原 QoS 收 PUBACK）
                        body = enc_str(topic) + bytes(payload)
                        cli.send(bytes([0x30]) + enc_remaining(len(body)) + body)
                        delivered += 1
                    except OSError:
                        pass
                    break
        return delivered

    def run(self) -> None:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("0.0.0.0", self.port))
        srv.listen(8)
        log(f"mini broker listening on :{self.port}")
        try:
            # 阻塞 accept：每客户端一线程自理收包，主循环只管接入
            while True:
                conn, addr = srv.accept()
                cli = Client(conn, addr)
                with self.lock:
                    self.clients[id(cli)] = cli
                threading.Thread(target=self._client_loop, args=(cli,), daemon=True).start()
        except KeyboardInterrupt:
            log("bye")
        finally:
            srv.close()

    # 实际的每客户端循环（含 PUBLISH 路由与 QoS1 PUBACK）
    def _client_loop(self, c: Client) -> None:
        try:
            while True:
                hdr = self._read_exact(c, 1)   # 固定头:1 字节 type/flags
                if hdr is None:
                    break
                ptype = hdr[0] >> 4
                flags = hdr[0] & 0x0F
                mult, val = 1, 0
                while True:
                    b = self._read_exact(c, 1)
                    if b is None:
                        return
                    val += (b[0] & 0x7F) * mult
                    mult *= 128
                    if not (b[0] & 0x80):
                        break
                body = self._read_exact(c, val) if val else b""
                if body is None:
                    return

                if ptype == 1:  # CONNECT
                    try:
                        nlen = struct.unpack("!H", body[0:2])[0]
                        c.cid = body[2:2 + nlen].decode("utf-8", "replace") or f"anon-{id(c):x}"
                    except Exception:
                        c.cid = f"anon-{id(c):x}"
                    c.send(bytes([0x20, 0x02, 0x00, 0x00]))
                    log(f"CONNECT cid={c.cid} ({c.addr[0]})")
                elif ptype == 8:  # SUBSCRIBE
                    pid = struct.unpack("!H", body[0:2])[0]
                    filters, i = [], 2
                    while i + 2 <= len(body):
                        nlen = struct.unpack("!H", body[i:i + 2])[0]
                        filters.append(body[i + 2:i + 2 + nlen].decode("utf-8", "replace"))
                        i += 2 + nlen + 1
                    # esp-mqtt 每主题一条 SUBSCRIBE —— 必须累积而非整体替换,
                    # 否则最后一条把 v1/devices/me/rpc/request/+ 顶掉(11-07 实测)
                    for f in filters:
                        if f not in c.filters:
                            c.filters.append(f)
                    c.send(bytes([0x90]) + enc_remaining(2 + len(filters)) +
                           struct.pack("!H", pid) + bytes([0] * len(filters)))
                    log(f"SUBSCRIBE cid={c.cid} filters={filters}")
                elif ptype == 3:  # PUBLISH
                    qos = (flags >> 1) & 0x03
                    tlen = struct.unpack("!H", body[0:2])[0]
                    topic = body[2:2 + tlen].decode("utf-8", "replace")
                    off = 2 + tlen
                    pid = None
                    if qos >= 1:
                        pid = struct.unpack("!H", body[off:off + 2])[0]
                        off += 2
                    payload = body[off:]
                    self.msg_count += 1
                    n = self._route(topic, payload)
                    if self.msg_count % self.log_every == 0:
                        log(f"msg#{self.msg_count} topic={topic} len={len(payload)} -> {n}")
                    if qos >= 1 and pid is not None:
                        c.send(bytes([0x40]) + enc_remaining(2) + struct.pack("!H", pid))
                elif ptype == 12:  # PINGREQ
                    c.send(bytes([0xD0, 0x00]))
                elif ptype == 14:  # DISCONNECT
                    break
                else:
                    log(f"ignore ptype={ptype} cid={c.cid}")
        except (OSError, ConnectionResetError, struct.error):
            pass
        finally:
            with self.lock:
                self.clients.pop(id(c), None)
            try:
                c.sock.close()
            except OSError:
                pass
            log(f"disconnected cid={c.cid}")

    @staticmethod
    def _read_exact(c: Client, n: int) -> bytes | None:
        while len(c.buf) < n:
            chunk = c.sock.recv(4096)
            if not chunk:
                return None
            c.buf += chunk
        out = bytes(c.buf[:n])
        del c.buf[:n]
        return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=21883)
    ap.add_argument("--log-every", type=int, default=50)
    opts = ap.parse_args()
    MiniBroker(opts.port, opts.log_every).run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
