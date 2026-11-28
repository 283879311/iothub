#!/usr/bin/env python3
"""MGMT Load Connection Parameters via raw syscalls (test-only, A0).

Facts verified against the running kernel headers
(/usr/src/linux-headers-7.0.0-43-generic/include/net/bluetooth/mgmt.h):
  - mgmt_hdr = { opcode, index, len }            (lines 49-53)
  - MGMT_OP_GET_CONNECTIONS       = 0x0015, fixed len 0
  - MGMT_OP_LOAD_CONN_PARAM       = 0x0035, MGMT_LOAD_CONN_PARAM_SIZE = 2
    payload = param_count(2) + param_count * mgmt_conn_param(15)
    struct mgmt_conn_param is __packed: bdaddr(6) type(1) min(2) max(2)
    latency(2) timeout(2)
  0x0015 was our earlier constant and is why every send got EINVAL
  (GET_CONNECTIONS takes an empty payload; hci_mgmt_cmd rejects the length).

Loads per-peer connection parameters for the ESP32:
  interval 24-40 units (30-50 ms), latency 0, supervision timeout 600 (6000 ms)

Usage:
  sudo python3 mgmt_load_connparam.py <device-mac> [addr_type] [hci_index]

addr_type follows the mgmt enum (include/net/bluetooth/bluetooth.h):
  0 = BDADDR_BREDR (kernel silently IGNORES LE params for this type)
  1 = BDADDR_LE_PUBLIC  <-- default, this device
  2 = BDADDR_LE_RANDOM
"""
import ctypes
import os
import select
import struct
import sys

libc = ctypes.CDLL(None, use_errno=True)

AF_BLUETOOTH = 31
SOCK_RAW = 3
BTPROTO_HCI = 1
HCI_CHANNEL_CONTROL = 3
HCI_DEV_NONE = 0xFFFF

SOL_BLUETOOTH = 274
BT_SNDMTU = 12

MGMT_OP_READ_VERSION = 0x0001
MGMT_OP_LOAD_CONN_PARAM = 0x0035

MGMT_STATUS = {0: "OK", 1: "unknown-command", 3: "failed", 5: "not-powered",
               8: "permission-denied", 11: "invalid-params",
               12: "invalid-index", 17: "invalid-index", 18: "unknown-command"}


def open_mgmt(index, with_sndmtu):
    fd = libc.socket(ctypes.c_int(AF_BLUETOOTH), ctypes.c_int(SOCK_RAW),
                     ctypes.c_int(BTPROTO_HCI))
    if fd < 0:
        err = ctypes.get_errno()
        raise OSError(err, "socket: " + os.strerror(err))
    buf = ctypes.create_string_buffer(
        struct.pack("<HHH", AF_BLUETOOTH, HCI_DEV_NONE, HCI_CHANNEL_CONTROL), 6)
    libc.bind.restype = ctypes.c_int
    if libc.bind(ctypes.c_int(fd), buf, ctypes.c_uint(6)) != 0:
        err = ctypes.get_errno()
        raise OSError(err, "bind: " + os.strerror(err))
    if with_sndmtu:
        vbuf = ctypes.create_string_buffer(struct.pack("<H", index), 2)
        libc.setsockopt.restype = ctypes.c_int
        rc = libc.setsockopt(ctypes.c_int(fd), ctypes.c_int(SOL_BLUETOOTH),
                             ctypes.c_int(BT_SNDMTU), vbuf, 2)
        print("  setsockopt(BT_SNDMTU %d) rc=%d" % (index, rc))
    return fd


def send_frame(fd, opcode, index, payload):
    header = struct.pack("<HHH", opcode, index, len(payload))
    frame = header + payload
    buf = ctypes.create_string_buffer(frame, len(frame))
    libc.send.restype = ctypes.c_ssize_t
    n = libc.send(ctypes.c_int(fd), buf, ctypes.c_size_t(len(frame)), 0)
    if n < 0:
        err = ctypes.get_errno()
        raise OSError(err, os.strerror(err))
    print("  tx %s -> %d bytes" % (frame.hex(), n))


def recv_frame(fd, timeout):
    ready, _, _ = select.select([fd], [], [], timeout)
    if not ready:
        return None
    buf = ctypes.create_string_buffer(512)
    libc.recv.restype = ctypes.c_ssize_t
    n = libc.recv(ctypes.c_int(fd), buf, ctypes.c_size_t(512), 0)
    if n <= 0:
        return None
    return buf.raw[:n]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    addr_text = sys.argv[1]
    addr_type = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    index = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    type_name = {0: "BDADDR_BREDR (kernel will ignore LE params!)",
                 1: "BDADDR_LE_PUBLIC",
                 2: "BDADDR_LE_RANDOM"}.get(addr_type, "unknown")

    if os.geteuid() != 0:
        print("ERROR: must run as root (MGMT CONTROL channel)")
        return 2

    octets = bytes(int(x, 16) for x in reversed(addr_text.split(":")))
    entry = struct.pack("<6sB4H", octets, addr_type, 24, 40, 0, 600)
    payload = struct.pack("<H", 1) + entry
    print("peer=%s type=%d (%s) index=%d payload=%d bytes: %s"
          % (addr_text, addr_type, type_name, index, len(payload),
             payload.hex()))
    if addr_type not in (1, 2):
        print("WARNING: mgmt LOAD_CONN_PARAM only accepts "
              "BDADDR_LE_PUBLIC(1)/BDADDR_LE_RANDOM(2); other values are "
              "silently skipped by the kernel while still returning Success.")

    for attempt, with_sndmtu in (("plain", False), ("sndmtu", True)):
        print("[%s]" % attempt)
        fd = None
        try:
            fd = open_mgmt(index, with_sndmtu)

            # 1) READ_VERSION on index 0xFFFF proves socket + framing work
            send_frame(fd, MGMT_OP_READ_VERSION, HCI_DEV_NONE, b"")
            resp = recv_frame(fd, 3)
            if resp:
                print("  READ_VERSION reply: %s" % resp.hex())
            else:
                print("  READ_VERSION reply: none (rc may still work)")

            # 2) LOAD_CONN_PARAM
            send_frame(fd, MGMT_OP_LOAD_CONN_PARAM, index, payload)
            resp = recv_frame(fd, 3)
            if not resp:
                print("  LOAD_CONN_PARAM reply: TIMEOUT")
                return 1
            print("  LOAD_CONN_PARAM reply: %s" % resp.hex())
            # 回复是 MGMT Command Complete 事件:
            #   {opcode=0x0001, index, len=3, cmd(2), status(1)}
            event = struct.unpack("<H", resp[0:2])[0]
            echo_index = struct.unpack("<H", resp[2:4])[0]
            status = 255
            if event == 0x0001 and len(resp) >= 9:
                cmd = struct.unpack("<H", resp[6:8])[0]
                status = resp[8]
                print("  Command Complete cmd=0x%04x index=%d status=%d (%s)"
                      % (cmd, echo_index, status,
                         MGMT_STATUS.get(status, "?")))
            elif event == 0x0002 and len(resp) >= 7:
                status = resp[6]
                print("  Command Status index=%d status=%d (%s)"
                      % (echo_index, status, MGMT_STATUS.get(status, "?")))
            else:
                print("  unexpected reply layout")
            if status == 0:
                print("RESULT: OK — peer params loaded "
                      "(24-40 units / latency 0 / supervision 600)")
                return 0
            print("RESULT: FAIL status=%d" % status)
            return 1
        except OSError as exc:
            print("  %s" % exc)
        finally:
            if fd is not None:
                libc.close(ctypes.c_int(fd))

    print("RESULT: FAIL — all send/parse variants rejected")
    return 1


if __name__ == "__main__":
    sys.exit(main())
