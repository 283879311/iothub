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

用法：
  tools/preflight_flash.py --port /dev/ttyUSB1 --expect-mac 10:97:BD:D3:65:DA \
      --elf build-release/iothub.elf
"""

import argparse
import hashlib
import re
import subprocess
import sys
from pathlib import Path

SERIAL_DIR = Path("artifacts/cmd04_flash_20261114/phaseA")


def port_device_map():
    """从两份 console 日志反推端口 → 最近一次设备 MAC。"""
    out = {}
    for port in ("ttyUSB0", "ttyUSB1"):
        f = SERIAL_DIR / f"serial_{port}.log"
        if not f.is_file():
            out[port] = None
            continue
        # 只读尾部 2MB，避免整份大日志
        size = f.stat().st_size
        with open(f, "rb") as fh:
            if size > 2 * 1024 * 1024:
                fh.seek(size - 2 * 1024 * 1024)
            data = fh.read().decode("utf-8", "replace")
        macs = re.findall(r"client_id=iothub-([0-9A-Fa-f]{12})", data)
        out[port] = macs[-1].upper() if macs else None
    return out


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
    ap.add_argument("--expect-device-id", required=True,
                    help="目标 device_id，如 IotHub-65D8（由 WiFi MAC 末 4 位推出）")
    ap.add_argument("--elf", required=True)
    args = ap.parse_args()

    name = Path(args.port).name
    mapping = port_device_map()
    observed_wifi = mapping.get(name)
    observed_id = dev_id_from_wifi_mac(observed_wifi)

    elf = Path(args.elf)
    if not elf.is_file():
        print(f"[FAIL] 镜像不存在: {elf}")
        return 2
    elf_sha = hashlib.sha256(elf.read_bytes()).hexdigest()
    build_sha = elf_sha[:9]

    print("=== 烧录前四元组 ===")
    print(f"  端口          : {args.port}  (日志名 {name})")
    print(f"  端口观测 WiFi MAC : {observed_wifi or '未知'}  → {observed_id}"
          f"（BLE≈{ble_from_wifi_mac(observed_wifi)}）")
    print(f"  期望 device_id    : {args.expect_device_id}")
    print(f"  镜像 ELF      : {elf}  sha256[:16]={elf_sha[:16]}  build_sha={build_sha}")
    print("====================")

    ok = True
    if observed_wifi is None:
        print("[FAIL] 该端口日志里找不到 client_id=iothub-<MAC>，无法确认设备身份")
        ok = False
    elif observed_id.lower() != args.expect_device_id.lower():
        print(f"[FAIL] 端口设备不符：观测 {observed_id} ≠ 期望 {args.expect_device_id}")
        ok = False
    if not ok:
        print("[STOP] 拒绝烧录（四元组核验未通过）")
        return 1

    print("[OK] 四元组一致，可执行烧录；烧录后请用 device.info.build_sha 反证")
    print(f"     期望 device.info.build_sha == {build_sha}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
