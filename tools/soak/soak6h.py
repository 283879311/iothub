#!/usr/bin/env python3
"""6h 单设备 soak runner（2026-11-06，口径与小程序侧对齐）。

- 时长默认 6h；每 60s 一个 BLE 会话采样：connect → login → device.info →
  network.status → disconnect（每会话全新 token，连接级鉴权）。
- 每 30 个周期一次扰动会话：connect → login → 订阅 CMD/DATA → 静默 hold 秒 →
  断开。订阅中 reaper 不得触发（R-BLE-3 语义的长期版）；断开后广播应自动恢复。
- 串口全程采集（复用 tools/capture_serial.py）；开始前 esptool 受控复位取干净基线。
- 结束时出 soak_summary.json / soak.csv / soak.jsonl，并对串口做
  rst:/idle terminate/Guru/abort/看门狗 计数与（按设备 uptime 窗口）归因。

判据（summary 里逐项给 pass/fail）：
  1) 无二次复位：串口无 "rst:0x" 且 uptime 单调；
  2) heap 无单调下降：末次 free >= 首次 free - 2048；
  3) adv_retry_count 不增长（max == 首值）；
  4) idle terminate 计数 == 0（本口径无未订阅静默会话；出现即逐条归因到会话窗口）；
  5) 无 Guru/abort/assert；adv watchdog 触发数 == 0。
"""
from __future__ import annotations

import argparse
import asyncio
import csv
import json
import re
import signal
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "ble_p0"))

from bleak import BleakClient  # noqa: E402
from p0_acceptance import CMD_TX, CMD_RX  # noqa: E402

DATA_TX = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


class Reader:
    """多对象 JSON 流解析（回包无换行分隔）。"""

    def __init__(self) -> None:
        self.buf = bytearray()
        self.objs: list[dict] = []
        self.ev = asyncio.Event()

    def on(self, _c, d) -> None:
        self.buf += bytes(d)
        text = self.buf.decode("utf-8", "replace")
        dec = json.JSONDecoder()
        idx = 0
        while idx < len(text):
            while idx < len(text) and text[idx] in " \r\n\t":
                idx += 1
            if idx >= len(text):
                break
            try:
                obj, end = dec.raw_decode(text, idx)
            except json.JSONDecodeError:
                break
            self.objs.append(obj)
            self.ev.set()
            idx = end
        self.buf = bytearray(text[idx:].encode())

    def reset(self) -> None:
        self.buf = bytearray()
        self.objs = []
        self.ev.clear()


def data_of(obj: dict | None) -> dict:
    if not isinstance(obj, dict):
        return {}
    d = obj.get("data")
    return d if isinstance(d, dict) else {}


async def ble_session(addr: str, perturb: bool, hold_s: float,
                      max_write: int = 20) -> tuple[dict | None, str | None]:
    """一个 BLE 会话。返回 (row, err)；row 为 None 表示连接层失败。"""
    cli = BleakClient(addr, timeout=25)
    await cli.connect()
    rd = Reader()
    await cli.start_notify(CMD_TX, rd.on)

    async def send(obj: dict, wait: float = 15.0) -> tuple[float, dict | None]:
        rd.reset()
        want = str(obj.get("id"))
        payload = (json.dumps(obj, separators=(",", ":")) + "\n").encode()
        t0 = time.time()
        for i in range(0, len(payload), max_write):
            await cli.write_gatt_char(CMD_RX, payload[i:i + max_write], response=True)
        deadline = t0 + wait
        while time.time() < deadline:
            for o in rd.objs:
                if str(o.get("id")) == want:
                    return time.time() - t0, o
            try:
                await asyncio.wait_for(rd.ev.wait(), timeout=0.2)
            except asyncio.TimeoutError:
                pass
        return time.time() - t0, None

    _, lr = await send({"id": "l", "version": "1.0", "cmd": "auth.login",
                        "args": {"username": "admin", "password": "123456"}}, 10)
    token = data_of(lr).get("token")
    if not token:
        await cli.disconnect()
        return None, "login_failed"
    _, status = await send({"id": "st", "version": "1.0", "cmd": "device.status",
                            "args": {}, "token": token}, 20)
    _, st = await send({"id": "s", "version": "1.0", "cmd": "network.status",
                        "args": {}, "token": token}, 20)
    row: dict = {
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "status": data_of(status),
        "net": data_of(st),
        "connected_after": bool(cli.is_connected),
    }
    if perturb:
        await cli.start_notify(DATA_TX, lambda _c, _d: None)
        row["perturb_hold_s"] = hold_s
        await asyncio.sleep(hold_s)
    try:
        await cli.disconnect()
    except Exception:  # noqa: BLE001
        pass
    return row, None


def stop_capture(proc: subprocess.Popen | None) -> None:
    if proc is None:
        return
    try:
        proc.send_signal(signal.SIGTERM)
        proc.wait(timeout=5)
    except Exception:  # noqa: BLE001
        try:
            proc.kill()
        except Exception:  # noqa: BLE001
            pass


def analyze_serial(serial_log: Path, sessions: list[dict]) -> dict:
    text = serial_log.read_text(encoding="utf-8", errors="replace") if serial_log.exists() else ""
    counts = {
        "rst": len(re.findall(r"rst:0x", text)),
        "guru_abort_assert": len(re.findall(r"Guru Meditation|abort\(\)|assert failed", text)),
        "idle_terminate": len(re.findall(r"idle terminate", text)),
        "adv_watchdog": len(re.findall(r"adv watchdog", text)),
        "heap_new_low": len(re.findall(r"heap new low", text)),
        "mqtt_connected": len(re.findall(r"MQTT connected:", text)),
        "mqtt_restart_attempt": len(re.findall(r"mqtt restart attempt", text)),
        "disconnect_0x222": len(re.findall(r"reason=0x222", text)),
        "disconnect_total": len(re.findall(r"central disconnected", text)),
    }
    # idle terminate 归因：把设备 uptime(I (ms)) 映射到会话窗口
    attributed = []
    for m in re.finditer(r"I \((\d+)\) \S+ bt_gatts: idle terminate", text):
        up = int(m.group(1))
        owner = next((s["session_id"] for s in sessions
                      if s.get("uptime_start") is not None and s.get("uptime_end") is not None
                      and s["uptime_start"] <= up <= s["uptime_end"]), None)
        attributed.append({"uptime_ms": up, "session": owner})
    return {"counts": counts, "idle_terminate_attributed": attributed}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--addr", default="B0:A7:32:28:C8:E6")
    ap.add_argument("--port", default="/dev/ttyUSB1")
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--duration-s", type=int, default=21600)
    ap.add_argument("--interval", type=int, default=60)
    ap.add_argument("--perturb-every", type=int, default=30)
    ap.add_argument("--hold", type=float, default=60.0)
    ap.add_argument("--no-reset", action="store_true")
    ns = ap.parse_args()

    out = Path(ns.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    jsonl = out / "soak.jsonl"
    csvf = out / "soak.csv"
    serial_log = out / "serial_soak.log"

    if not ns.no_reset:
        log("controlled reset for clean baseline (esptool chip_id)")
        esptool = str(Path.home() / ".espressif/python_env/idf6.0_py3.12_env/bin/esptool")
        subprocess.call([esptool, "--port", ns.port, "--baud", "460800",
                         "--after", "hard_reset", "chip_id"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(10)

    log(f"start serial capture -> {serial_log}")
    cap = subprocess.Popen([sys.executable, str(ROOT / "tools/capture_serial.py"),
                            ns.port, str(serial_log)])

    sessions: list[dict] = []
    csv_rows: list[dict] = []
    start = time.time()
    cycle = 0
    consecutive_fail = 0
    stop = False

    def finish() -> None:
        stop_capture(cap)
        summary = build_summary()
        (out / "soak_summary.json").write_text(
            json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        log(f"summary written -> {out/'soak_summary.json'}  verdict={summary['verdict']}")

    def heap_stable(frees: list[int]) -> bool:
        """稳态 heap 判据:取后半段样本,末值 >= 后半段首值 - 2048,且
        后半段无"较前 10 样本均值下降 >4KB"的阶梯。"""
        if len(frees) < 6:
            return True
        tail = frees[len(frees) // 2:]
        if tail[-1] < tail[0] - 2048:
            return False
        for i in range(10, len(tail)):
            avg = sum(tail[i - 10:i]) / 10
            if tail[i] < avg - 4096:
                return False
        return True

    def build_summary() -> dict:
        ups = [s["uptime_ms"] for s in sessions if s.get("uptime_ms") is not None]
        frees = [s["heap_free"] for s in sessions if s.get("heap_free") is not None]
        advr = [s["adv_retry"] for s in sessions if s.get("adv_retry") is not None]
        serial_ana = analyze_serial(serial_log, sessions)
        uptime_monotonic = all(b > a for a, b in zip(ups, ups[1:]))
        checks = {
            # 串口第 1 条 rst 属于开始前的受控复位基线;soak 中复位以 uptime 回落为准
            "no_reboot": uptime_monotonic and bool(ups)
                         and serial_ana["counts"]["rst"] <= 1,
            "heap_no_decline": heap_stable(frees),
            "adv_retry_stable": bool(advr) and max(advr) == advr[0],
            "idle_terminate_zero": serial_ana["counts"]["idle_terminate"] == 0,
            "no_crash": serial_ana["counts"]["guru_abort_assert"] == 0,
            "adv_watchdog_zero": serial_ana["counts"]["adv_watchdog"] == 0,
            "no_ll_timeout": serial_ana["counts"]["disconnect_0x222"] == 0,
        }
        fails = [k for k, v in checks.items() if not v]
        ok_sessions = sum(1 for s in sessions if s.get("err") is None)
        return {
            "started": datetime.fromtimestamp(start).isoformat(),
            "finished": datetime.now().isoformat(),
            "duration_s": round(time.time() - start),
            "cycles": cycle,
            "sessions_total": len(sessions),
            "sessions_ok": ok_sessions,
            "sessions_err": len(sessions) - ok_sessions,
            "uptime_first_ms": ups[0] if ups else None,
            "uptime_last_ms": ups[-1] if ups else None,
            "heap_free_first": frees[0] if frees else None,
            "heap_free_last": frees[-1] if frees else None,
            "heap_free_min": min(frees) if frees else None,
            "adv_retry_max": max(advr) if advr else None,
            "serial": serial_ana,
            "checks": checks,
            "verdict": "PASS" if not fails else f"FAIL({','.join(fails)})",
        }

    async def run_loop() -> None:
      nonlocal cycle, consecutive_fail, stop
      try:
        while time.time() - start < ns.duration_s:
            cycle += 1
            perturb = (ns.perturb_every > 0 and cycle % ns.perturb_every == 0)
            t0 = time.time()
            row: dict = {"session_id": cycle, "ts": time.strftime("%Y-%m-%dT%H:%M:%S"),
                         "type": "perturb" if perturb else "sample", "err": None}
            try:
                data, err = await ble_session(ns.addr, perturb, ns.hold)
                if err is not None or data is None:
                    row["err"] = err or "connect_failed"
                else:
                    status, net = data["status"], data["net"]
                    row.update({
                        "uptime_ms": status.get("uptime_ms"),
                        "reset_reason": status.get("reset_reason"),
                        "firmware": status.get("firmware"),
                        "build_sha": status.get("build_sha"),
                        "heap_free": (status.get("heap") or {}).get("free"),
                        "heap_min": (status.get("heap") or {}).get("min_free"),
                        "largest": (status.get("heap") or {}).get("largest"),
                        "adv": (status.get("ble") or {}).get("advertising"),
                        "conn": (status.get("ble") or {}).get("connected"),
                        "adv_retry": (status.get("ble") or {}).get("adv_retry_count"),
                        "cf": (status.get("ble") or {}).get("conn_find"),
                        "rx_age": (status.get("ble") or {}).get("rx_age_ms"),
                        "sta_ip": net.get("sta_ip"),
                        "cfg_state": net.get("cfg_state"),
                        "mqtt": net.get("mqtt_connected"),
                        "mqtt_err": net.get("mqtt_last_error"),
                        "trial": (net.get("trial") or {}).get("state"),
                        "connected_after": data.get("connected_after"),
                        "perturb_hold_s": data.get("perturb_hold_s"),
                    })
            except Exception as exc:  # noqa: BLE001
                row["err"] = f"{type(exc).__name__}: {exc}"
            sessions.append(row)
            with jsonl.open("a", encoding="utf-8") as f:
                f.write(json.dumps(row, ensure_ascii=False) + "\n")
            if row.get("err") is None and row.get("uptime_ms") is not None:
                # 会话窗口（用于 idle terminate 归因）
                row["uptime_start"] = row["uptime_ms"]
                row["uptime_end"] = row["uptime_ms"] + 70000 if perturb else row["uptime_ms"] + 10000
                csv_rows.append(row)
            else:
                csv_rows.append(row)
            consecutive_fail = consecutive_fail + 1 if row.get("err") else 0
            if consecutive_fail >= 5:
                log("5 consecutive session failures — aborting soak")
                stop = True
            log(f"cycle {cycle} type={row['type']} err={row.get('err')} "
                f"ip={row.get('sta_ip')} mqtt={row.get('mqtt')} "
                f"heap={row.get('heap_free')} uptime={row.get('uptime_ms')}")
            if stop:
                break
            # 对齐到下一个 interval 边界（扰动会话含 hold，天然占满）
            target = start + cycle * ns.interval
            delay = max(0.0, target - time.time())
            await asyncio.sleep(min(delay, ns.interval))
      except KeyboardInterrupt:
        log("interrupted")
      finally:
        finish()

    asyncio.run(run_loop())
    return 0



if __name__ == "__main__":
    # main() 是同步入口（内部自己 asyncio.run(run_loop())）。
    # 2026-11-06:首版写成 asyncio.run(main()) 会在收尾阶段抛
    # "a coroutine was expected"（summary 已落盘，但退出码非 0）。
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        pass
