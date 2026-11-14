#!/usr/bin/env python3
"""CMD-04 预采集 · MQTT cycle-2（在飞/取消分支，吸取 cycle-1 教训）。

cycle-1 教训：op1 终态后 job 粘滞 ~61s（rescue 链完成后才 clear_job），
P4 的 tryconnect 走了 existing-op 分支、op2 未创建、cancel 无的放矢。
cycle-2：受理响应立即校验 op_id != op1（job 空闲确证）→ BLE op.get 在飞 →
BLE cancel（job active 时取消 active op）→ op.get 终态。
"""

import asyncio
import json
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "ble_p0"))

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cmd04_precapture_mqtt as c1  # noqa: E402

from bleak import BleakClient  # noqa: E402
from mqtt_v1_r41 import MqttV1  # noqa: E402
from p0_acceptance import Session, choose_target, scan_filtered  # noqa: E402

OP1 = "op-DC04749FC55E348C"
FAKE2 = "__precapture_flight2__"


async def main() -> int:
    OUT = c1.OUT
    OUT.mkdir(parents=True, exist_ok=True)
    c1._log_f = open(OUT / "capture_log.txt", "a", encoding="utf-8")

    records = await scan_filtered(10.0)
    target = choose_target(records, c1.TARGET_ADDR)
    if not target:
        c1.log("[fatal] 设备-1 不在广播中")
        return 2
    broker = subprocess.Popen(
        [sys.executable, "tools/broker_local/mini_broker.py", "--port", "21883"],
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

    async with BleakClient(target["address"], timeout=25) as cli:
        sess = Session(cli, address=target["address"])
        await sess._connect_once()
        c1.log("[c2] BLE 已连接")

        await c1.ble(sess, "config.get")
        m = MqttV1("127.0.0.1:21883")
        await c1.ble(sess, "config.set",
                     {"patch": {"mqtt_host": c1.PC_IP, "mqtt_port": 21883}})
        if not await c1.wait_broker_link(m):
            c1.log("[fatal] 未接入 mini_broker，恢复指向退出")
            await c1.ble(sess, "config.set",
                         {"patch": {"mqtt_host": c1.CLOUD_HOST,
                                    "mqtt_port": c1.CLOUD_PORT}})
            broker.terminate()
            return 3
        c1.log("[c2] 已接入 mini_broker，探测 job 空闲")

        r = c1.mqtt_send(m, "network.tryconnect", {"ssid": FAKE2}, wait=10)
        op2 = ((r or {}).get("data") or {}).get("operation_id")
        state = ((r or {}).get("data") or {}).get("state")
        if op2 is None or op2 == OP1:
            c1.log(f"[c2] job 仍粘滞（resp op={op2} state={state}）——本轮不继续")
            await c1.ble(sess, "config.set",
                         {"patch": {"mqtt_host": c1.CLOUD_HOST,
                                    "mqtt_port": c1.CLOUD_PORT}})
            broker.terminate()
            return 4
        c1.log(f"[c2] 新 op={op2}（job 空闲确证），立即在飞采集")
        await c1.ble(sess, "operation.get", {"operation_id": op2})
        await c1.ble(sess, "network.status")
        cr = await c1.ble(sess, "network.cancel", {}, timeout=25)
        c1.log(f"[c2] cancel ok={(cr or {}).get('ok')}")
        await asyncio.sleep(3)
        await c1.ble(sess, "operation.get", {"operation_id": op2})
        await c1.ble(sess, "network.status")

        c1.log("[c2] 等 rescue 链 + job clear（90s）")
        await asyncio.sleep(90)
        c1.mqtt_send(m, "operation.get", {"operation_id": op2})

        await c1.ble(sess, "config.set",
                     {"patch": {"mqtt_host": c1.CLOUD_HOST,
                                "mqtt_port": c1.CLOUD_PORT}})
        cloud_ok = False
        for _ in range(10):
            await asyncio.sleep(8)
            st = await c1.ble(sess, "network.status")
            if ((st.get("data") or {}).get("mqtt_connected") is True):
                cloud_ok = True
                break
        c1.log(f"[c2] cloud 重连={'ok' if cloud_ok else 'FAIL'}")
        await c1.ble(sess, "config.get")
        await c1.ble(sess, "device.status")
        await c1.ble(sess, "network.status")
        m.cli.loop_stop()
        broker.terminate()
    c1.log("[c2 done]")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
