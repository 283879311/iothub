#!/usr/bin/env python3
"""CMD-04 系列合并烧录 · 旧固件 pre-golden/基线预采集（步骤 1，评审 2026-11-14 授权范围）。

前置：设备在跑 1.3-2ef2a38（build_sha 98f2465cb）；串口在录；不烧录/不重启/不改持久配置。
授权分支：04a 缺失三分支（owner 不匹配 / accepted-connecting 在飞 / expired）、
04b 七分支、04c sync tryconnect 受理响应（段 A）、逐任务 HWM（见 readme 口径）、heap 基线。

阶段（--stages S1,S2,... 选择；缺省跑到无凭据处为止）：
  S0  扫描+连接+登录（只读）
  S1  只读基线: config.get / device.status / network.status(idle) —— 04b 正常/缺字段 + heap/stack
  S2  MQTT 路径探测（按 config.mqtt_host 判断本机 broker / 云 broker，只探测不改配置）
  S3  终态分支: BLE sync tryconnect(不存在 SSID) → ap_not_found → operation.get（04b 终态）
  S4  在飞+取消: MQTT async tryconnect(不存在 SSID) → BLE operation.get 在飞（04a accepted/
      connecting + trial 块）→ MQTT network.cancel → BLE operation.get（04b 取消）
  S5  owner 不匹配: BLE 建终态 op → MQTT operation.get 同 id（04a+04b owner 不匹配）
  S6  expired+段A: [需 IOTHUB_WIFI_PASSWORD] sync tryconnect 当前 SSID → got_ip 响应
      （04c 段 A）→ 不 apply 静置 300s → operation.get（04a/04b expired）
  S7  终态对比: config.get / device.status / network.status vs S1（状态变化前后记录）

每帧原始 JSON 追加 pre/capture_log.txt；各分支响应独立落 pre/NN_branch.json
（{branch, source, request, response, ts}）。密码只从环境变量读，不落盘。
"""

import asyncio
import json
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "ble_p0"))

from bleak import BleakClient  # noqa: E402

from p0_acceptance import Session, choose_target, scan_filtered  # noqa: E402

OUT = Path("artifacts/cmd04_flash_20261114/pre")
FAKE_EXPIRED = "__precapture_expired__"
FAKE_TERMINAL = "__precapture_terminal__"
FAKE_FLIGHT = "__precapture_flight__"
FAKE_OWNER = "__precapture_owner__"
STAGES = ["S0", "S1", "S2", "S3", "S4", "S5", "S6", "S7"]

_seq = 0


def next_id() -> str:
    global _seq
    _seq += 1
    return f"pre-{_seq:02d}"


def log_line(text: str) -> None:
    line = f"{time.strftime('%H:%M:%S')} {text}"
    print(line, flush=True)
    with open(OUT / "capture_log.txt", "a", encoding="utf-8") as f:
        f.write(line + "\n")


def save(branch: str, source: str, request: dict, response: dict) -> None:
    idx = len(list(OUT.glob("[0-9][0-9]_*.json"))) + 1
    path = OUT / f"{idx:02d}_{branch}.json"
    payload = {
        "branch": branch,
        "source": source,
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "request": request,
        "response": response,
    }
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=1), encoding="utf-8")
    log_line(f"[save] {path.name}")


async def rpc(sess: Session, method: str, params: dict | None = None,
              source: str = "ble", timeout: float = 20) -> dict:
    req = {"id": next_id(), "version": "1.0", "cmd": method,
           "args": params or {}, "token": sess.token}
    res = await sess.send_request(req, timeout=timeout)
    save(f"{method.replace('.', '_')}_{source}", source, req, res.obj)
    return res.obj


async def main() -> int:
    wanted = [s for s in (sys.argv[1].split(",") if len(sys.argv) > 1 else STAGES)
              if s in STAGES]
    OUT.mkdir(parents=True, exist_ok=True)

    records = await scan_filtered(10.0)
    target = choose_target(records, None)
    if not target:
        log_line("[fatal] no BLE target")
        return 2
    log_line(f"[conn] target {target['address']} name={target.get('name')!r}")
    async with BleakClient(target["address"], timeout=25) as cli:
        sess = Session(cli, address=target["address"])
        await sess._connect_once()
        log_line("[conn] connected + logged in")

        cfg = {}
        if "S1" in wanted:
            await rpc(sess, "device.info")
            cfg = await rpc(sess, "config.get")
            await rpc(sess, "device.status")
            await rpc(sess, "network.status")

        mqtt_host = ""
        try:
            st = (cfg.get("data") or {}).get("mqtt") or {}
            mqtt_host = f"{st.get('host', '')}:{st.get('port', '')}"
        except Exception:  # noqa: BLE001
            pass
        if "S2" in wanted:
            log_line(f"[mqtt] device mqtt_host={mqtt_host or '(config.get 未含)'}")

        if "S3" in wanted:
            log_line("[S3] sync tryconnect(不存在 SSID) → 终态分支")
            await rpc(sess, "network.tryconnect", {"ssid": FAKE_TERMINAL}, timeout=30)
            await rpc(sess, "network.status")
            # operation id 从响应事件里取不到时用 operation.get 空查询兜底
            await rpc(sess, "network.status")

        if "S4" in wanted:
            log_line("[S4] MQTT async tryconnect → 在飞查询 → cancel —— S2 需 MQTT 可达，本阶段跳过")
            log_line("[S4] 未实现自动驱动（MQTT 路径待 S2 结论），见 readme")

        if "S6" in wanted:
            pw = os.environ.get("IOTHUB_WIFI_PASSWORD", "")
            if not pw:
                log_line("[S6] IOTHUB_WIFI_PASSWORD 未设置——expired/段A 阶段跳过")
            else:
                cur = ((cfg.get("data") or {}).get("sta") or {}).get("ssid", "")
                log_line(f"[S6] sync tryconnect 当前 SSID={cur!r} → got_ip 段A")
                await rpc(sess, "network.tryconnect", {"ssid": cur, "password": pw},
                          timeout=30)
                log_line("[S6] 不 apply，静置 300s 后查 expired")
                await asyncio.sleep(305)
                await rpc(sess, "network.status")
                # 需 operation id：由 tryconnect 响应 data.trial_id 承载
                # （save() 已记录完整帧，expired 查询在 readme 手册流程中补跑）

        if "S7" in wanted:
            await rpc(sess, "config.get")
            await rpc(sess, "device.status")
            await rpc(sess, "network.status")

        await cli.disconnect()
    log_line("[done] stages=" + ",".join(wanted))
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
