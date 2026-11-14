#!/usr/bin/env python3
"""CMD-04 预采集 · MQTT 阶段（dc254d5 基线，评审 2026-11-14 裁决后恢复）。

裁决口径：dc254d5 为 pre-golden 基线；目标单元=设备-1（adv C8E4）；
状态变更（mqtt_host 临时指向→采集→恢复）按"前后状态记录"边界执行。

阶段：
  P0  BLE 连接 + 采集 mqtt 配置前置状态（改指向前的证据）
  P1  启动 mini_broker(127.0.0.1:21883) + BLE config.set 临时改指向 PC(192.168.1.19)
  P2  等设备接入 mini_broker（≤70s 轮询）
  P3  S3 终态分支: MQTT async tryconnect(不存在 SSID) → op_id → 飞行结束后
      BLE operation.get（04b 终态）+ network.status
  P4  S4 在飞+取消: MQTT async tryconnect → op_id → BLE operation.get 在飞
      （04a accepted/connecting + trial 块）→ BLE network.cancel（不依赖 Wi-Fi）
      → BLE operation.get（04b 取消）+ network.status
  P5  S5 owner 探针: BLE 查 MQTT op（特权放行）/ MQTT 查 MQTT op（匹配）/
      MQTT 查不存在 id（not_found）——owner-gate 错配分支在 dc254d5 上不可构造
      （无 HTTP 传输、全 op 归 MQTT 所有），如实记录
  P6  BLE config.set 恢复云端指向 → 轮询确认 mqtt_connected=true
  P7  终态对比: config.get / device.status / network.status

每帧落 artifacts/cmd04_flash_20261114/pre/（NN_branch.json + capture_log.txt）。
"""

import asyncio
import json
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "ble_p0"))

import bleak  # noqa: F401
from bleak import BleakClient  # noqa: E402

from mqtt_v1_r41 import MqttV1  # noqa: E402
from p0_acceptance import Session, choose_target, scan_filtered  # noqa: E402

OUT = Path("artifacts/cmd04_flash_20261114/pre")
TARGET_ADDR = "B0:A7:32:28:C8:E6"          # 设备-1（adv C8E4）
PC_IP = "192.168.1.19"
CLOUD_HOST, CLOUD_PORT = "139.9.93.245", 21883
LOCAL_PORT = 21883
FAKE_TERMINAL = "__precapture_terminal__"
FAKE_FLIGHT = "__precapture_flight__"

_log_f = None


def log(text: str) -> None:
    line = f"{time.strftime('%H:%M:%S')} {text}"
    print(line, flush=True)
    _log_f.write(line + "\n")
    _log_f.flush()


def save(branch: str, source: str, request: dict, response: dict) -> None:
    idx = len(list(OUT.glob("[0-9][0-9]_*.json"))) + 1
    path = OUT / f"{idx:02d}_{branch}.json"
    path.write_text(json.dumps({
        "branch": branch, "source": source,
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "request": request, "response": response,
    }, ensure_ascii=False, indent=1), encoding="utf-8")
    log(f"[save] {path.name}")


async def ble(sess: Session, cmd: str, args: dict | None = None,
              timeout: float = 20) -> dict:
    req = {"id": f"ble-{sess.next_seq()}", "version": "1.0", "cmd": cmd,
           "args": args or {}, "token": sess.token}
    res = await sess.send_request(req, timeout=timeout)
    save(f"{cmd.replace('.', '_')}_ble", "ble", req, res.obj)
    return res.obj


def mqtt_send(m: MqttV1, cmd: str, args: dict | None = None, wait: float = 8.0):
    # 唯一 id：设备回包 body id=请求 id；共享 id 会与 outbox 重投旧帧串台
    # （cycle-1 教训：11/34 号帧被重投的 accept 帧污染）
    req = {"id": f"mq-{m._next_rid()}", "version": "1.0", "cmd": cmd,
           "args": args or {}}
    resp, rid = m.send(req, wait=wait)
    save(f"{cmd.replace('.', '_')}_mqtt", "mqtt", req, resp or {"_note": "no response"})
    return resp


async def wait_broker_link(m: MqttV1, seconds: float = 75) -> bool:
    deadline = time.time() + seconds
    while time.time() < deadline:
        resp, _ = m.send({"id": "link-probe", "version": "1.0", "cmd": "device.info",
                          "args": {}}, wait=3.0)
        if resp is not None:
            return True
        await asyncio.sleep(5)
    return False


async def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    global _log_f
    _log_f = open(OUT / "capture_log.txt", "a", encoding="utf-8")

    records = await scan_filtered(10.0)
    target = choose_target(records, TARGET_ADDR)
    if not target:
        log("[fatal] 设备-1(adv C8E4) 不在广播中")
        return 2
    log(f"[conn] {target['address']} name={target.get('name')!r}")

    broker = subprocess.Popen(
        [sys.executable, "tools/broker_local/mini_broker.py", "--port", str(LOCAL_PORT)],
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

    async with BleakClient(target["address"], timeout=25) as cli:
        sess = Session(cli, address=target["address"])
        await sess._connect_once()
        log("[conn] BLE 已连接并登录")

        cfg0 = (await ble(sess, "config.get"))["data"]
        save("p0_mqtt_config_before", "ble",
             {"_note": "repoint 前状态记录"},
             {"mqtt": cfg0.get("mqtt"), "network_sta_ip": cfg0["network"].get("sta_ip")})

        m = MqttV1(f"127.0.0.1:{LOCAL_PORT}")

        # P1 临时改指向（SUBSYS effect，非整机重启）
        await ble(sess, "config.set", {"patch": {"mqtt_host": PC_IP,
                                                 "mqtt_port": LOCAL_PORT}})
        if not await wait_broker_link(m):
            log("[fatal] 设备未接入 mini_broker——恢复指向后退出")
            await ble(sess, "config.set", {"patch": {"mqtt_host": CLOUD_HOST,
                                                     "mqtt_port": CLOUD_PORT}})
            broker.terminate()
            return 3
        log("[P2] 设备已接入 mini_broker")

        # P3 终态分支
        r = mqtt_send(m, "network.tryconnect", {"ssid": FAKE_TERMINAL}, wait=10)
        op1 = ((r or {}).get("data") or {}).get("operation_id")
        log(f"[P3] async 受理 op={op1}，等待飞行+恢复(~55s)")
        await asyncio.sleep(55)
        if op1:
            await ble(sess, "operation.get", {"operation_id": op1})
            await ble(sess, "network.status")

        # P4 在飞 + 取消
        r = mqtt_send(m, "network.tryconnect", {"ssid": FAKE_FLIGHT}, wait=10)
        op2 = ((r or {}).get("data") or {}).get("operation_id")
        log(f"[P4] async 受理 op={op2}，立即在飞查询")
        if op2:
            await ble(sess, "operation.get", {"operation_id": op2})
            await ble(sess, "network.status")
            cr = await ble(sess, "network.cancel", {}, timeout=25)
            log(f"[P4] cancel 响应 ok={(cr or {}).get('ok')}")
            await asyncio.sleep(2)
            await ble(sess, "operation.get", {"operation_id": op2})
            await ble(sess, "network.status")

        # P5 owner 探针（owner-gate 错配在 dc254d5 不可构造，探针记录）
        if op1:
            await ble(sess, "operation.get", {"operation_id": op1})
            mqtt_send(m, "operation.get", {"operation_id": op1})
            mqtt_send(m, "operation.get", {"operation_id": "op-00000000CDEF1234"})

        # P6 恢复云端指向
        await ble(sess, "config.set", {"patch": {"mqtt_host": CLOUD_HOST,
                                                 "mqtt_port": CLOUD_PORT}})
        log("[P6] 已恢复云端指向，等待 cloud 重连")
        cloud_ok = False
        for _ in range(10):
            await asyncio.sleep(8)
            st = await ble(sess, "network.status")
            if ((st.get("data") or {}).get("mqtt_connected") is True):
                cloud_ok = True
                break
        log(f"[P6] cloud 重连={'ok' if cloud_ok else 'FAIL（需人工核对）'}")

        # P7 终态对比
        await ble(sess, "config.get")
        await ble(sess, "device.status")
        await ble(sess, "network.status")

        m.cli.loop_stop()
        broker.terminate()
    log("[done] MQTT 阶段完成")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
