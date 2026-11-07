#!/usr/bin/env python3
"""DEF-MQTT-001 故障注入 broker（最小 MQTT 3.1.1）。

用途：在不依赖真实 broker 的情况下，构造"受理回包丢失"的两种现场：

  cut       设备发布回包时**立即断链且不回 PUBACK**（受理瞬间断连）
  withhold  一段时间内**不回任何 PUBACK**（制造 inflight/outbox 积压），
            之后向设备下发 tryconnect 请求
  normal    正常 PUBACK 并回放响应（对照）

它不是完整 broker：只处理 CONNECT / SUBSCRIBE / PUBLISH(QoS0/1) /
PINGREQ / DISCONNECT，够设备链路 + 单条 RPC 往返。

用法：
  python3 mqtt_ack_gate_inject.py --mode cut --ssid CMCC-502 --port 21883
  设备 MQTT 指向本机:21883 后运行；脚本下发 network.tryconnect 并观察
  设备串口是否出现 resp_ack_timeout / resp acked / resp ack gate released。
"""
from __future__ import annotations

import argparse
import json
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


def server_publish(topic: str, payload: str) -> bytes:
    body = enc_str(topic) + payload.encode()
    return bytes([0x30]) + enc_remaining(len(body)) + body


class InjectBroker:
    def __init__(self, mode: str, ssid: str, password: str, delay: float, hold: float,
                 expect_client: str = ""):
        self.expect_client = expect_client
        self.mode = mode
        self.ssid = ssid
        self.password = password
        self.delay = delay
        self.hold = hold
        self.subscribed = threading.Event()
        self.conn: socket.socket | None = None
        self.puback_blocked = (mode == "withhold")
        self.request_sent = False
        self.request_scheduled = False

    # ---- 发送工具 ----
    def send(self, data: bytes) -> None:
        if self.conn is not None:
            try:
                self.conn.sendall(data)
            except OSError:
                pass

    def send_puback(self, pid: int) -> None:
        self.send(bytes([0x40, 0x02]) + struct.pack("!H", pid))

    def send_request(self) -> None:
        if self.conn is None:
            log("request skipped (no connection)")
            return
        self.request_sent = True
        rid = f"inj-{int(time.time())}"
        payload = json.dumps({
            "version": "1.0", "id": rid, "cmd": "network.tryconnect",
            "args": {"ssid": self.ssid, "password": self.password},
        }, ensure_ascii=False, separators=(",", ":"))
        topic = "v1/devices/me/rpc/request/9001"
        log(f"-> request {topic} id={rid}")
        self.send(server_publish(topic, payload))

    # ---- 包处理 ----
    @staticmethod
    def client_id_of(body: bytes) -> str:
        """CONNECT body: 10B 可变头后是 client_id(2B 长度 + UTF-8)。"""
        if len(body) < 12:
            return ""
        n = struct.unpack("!H", body[10:12])[0]
        return body[12:12 + n].decode("utf-8", "replace")

    def handle(self, b0: int, body: bytes, first_publish: bool) -> bool:
        """返回 False 表示应关闭连接。body 为去掉固定头后的内容。"""
        ptype = b0 >> 4
        if ptype == 1:      # CONNECT
            cid = self.client_id_of(body)
            if self.expect_client and self.expect_client not in cid:
                log(f"!! CONNECT from unexpected client_id={cid!r}; closing (not our device)")
                return False
            log(f"CONNECT client_id={cid} -> CONNACK")
            self.send(bytes([0x20, 0x02, 0x00, 0x00]))
        elif ptype == 8:    # SUBSCRIBE
            pid = struct.unpack("!H", body[0:2])[0]
            self.send(bytes([0x90, 0x03]) + struct.pack("!H", pid) + bytes([0x01]))
            self.subscribed.set()
            log(f"SUBSCRIBE pid={pid} -> SUBACK")
            if not self.request_scheduled:
                self.request_scheduled = True
                threading.Timer(self.delay, self.send_request).start()
        elif ptype == 3:    # PUBLISH
            qos = (b0 >> 1) & 0x03
            tlen = struct.unpack("!H", body[0:2])[0]
            topic = body[2:2 + tlen].decode("utf-8", "replace")
            rest = body[2 + tlen:]
            pid = None
            if qos > 0 and len(rest) >= 2:
                pid = struct.unpack("!H", rest[0:2])[0]
            preview = rest[2:].decode("utf-8", "replace")[:120] if qos > 0 else rest.decode("utf-8", "replace")[:120]
            log(f"PUBLISH topic={topic} qos={qos} pid={pid} {preview}")
            if qos == 0:
                return True
            if self.mode == "cut" and self.request_sent and "rpc/response" in topic:
                log("!! cut mode: closing socket WITHOUT PUBACK (response)")
                return False
            if self.puback_blocked:
                log("!! withhold mode: dropping PUBACK")
                return True
            if pid is not None:
                self.send_puback(pid)
        elif ptype == 12:   # PINGREQ
            self.send(bytes([0xD0, 0x00]))
        elif ptype == 14:   # DISCONNECT
            log("client DISCONNECT")
            return False
        return True

    def run(self, port: int) -> None:
        srv = socket.socket()
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("0.0.0.0", port))
        srv.listen(4)
        log(f"inject broker listening :{port} mode={self.mode}")
        while True:
            conn, addr = srv.accept()
            log(f"device connected from {addr[0]}:{addr[1]}")
            self.conn = conn
            self.request_sent = False
            self.request_scheduled = False
            conn.settimeout(1.0)
            buf = b""
            first_publish = True
            close = False
            while not close:
                try:
                    chunk = conn.recv(4096)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if not chunk:
                    break
                buf += chunk
                while len(buf) >= 2:
                    hdr_type = buf[0] >> 4
                    # 解析 remaining length
                    mult, value, idx = 1, 0, 1
                    while idx < len(buf):
                        b = buf[idx]
                        value += (b & 0x7F) * mult
                        mult *= 128
                        idx += 1
                        if not (b & 0x80):
                            break
                    else:
                        break
                    total = idx + value
                    if len(buf) < total:
                        break
                    pkt = buf[:total]
                    b0 = pkt[0]
                    body = pkt[idx:]
                    is_pub = (hdr_type == 3)
                    buf = buf[total:]
                    close = not self.handle(b0, body, is_pub and first_publish)
                    if is_pub:
                        first_publish = False
                    if close:
                        break
            try:
                conn.close()
            except OSError:
                pass
            self.conn = None
            log("connection closed; waiting for next")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=("cut", "withhold", "normal"), default="cut")
    ap.add_argument("--ssid", required=True, help="tryconnect 目标 SSID")
    ap.add_argument("--expect-client", default="B0A73228C8E4",
                    help="只服务包含该串的 client_id(默认目标板 C8E4),防止误伤同网段其它设备")
    ap.add_argument("--pass", dest="password", default="x",
                    help="tryconnect 口令(默认 x;V2' 用真实口令)")
    ap.add_argument("--port", type=int, default=21883)
    ap.add_argument("--delay", type=float, default=6.0, help="订阅后多久下发请求(秒)")
    ap.add_argument("--hold", type=float, default=30.0, help="withhold 模式不 ACK 的时长(秒)")
    ns = ap.parse_args()
    try:
        InjectBroker(ns.mode, ns.ssid, ns.password, ns.delay, ns.hold, ns.expect_client).run(ns.port)
    except KeyboardInterrupt:
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
