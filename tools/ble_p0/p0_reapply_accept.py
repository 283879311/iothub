#!/usr/bin/env python3
"""P0 静态 re-apply 验收 runner（2026-11-05）。

用途：验证 `network.status` 新增的 cfg_state / persistent_* 字段，以及
`network.reconnect` 多次重连后静态 IP 是否仍被保留（P0 方向 3/4 设备验收）。

模式：
  base     —— device.info（免 token）+ login + 一次 network.status 采样；
  reapply  —— 连续 N 次 network.reconnect，每轮轮询 network.status 直到
              重连完成，记录 IP/cfg_state/persistent_* 变化。

约束：
  * 口令只从 IOTHUB_ADMIN_PASSWORD 读取（缺省用工具统一的测试默认值），
    不写日志、不写结果文件；结果经 sanitize() 掩码。
  * 只发送 network.reconnect 与只读查询，不写配置、不改 NVS。
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
import time
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

from p0_acceptance import sanitize  # noqa: E402
from success_path import close_session, connect_once, data_of  # noqa: E402

DEFAULT_ADDRESS = "B0:A7:32:28:C8:E6"   # C8E4 板的 BLE 地址（Wi-Fi MAC 尾字节为 E4）
EXPECT_DEVICE = "IotHub-C8E4"


def build_args(args: argparse.Namespace) -> argparse.Namespace:
    """connect_once() 需要 argparse 形状的参数。"""
    return argparse.Namespace(
        address=args.address,
        user=args.user,
        user_password=args.password,
        max_write=20,
    )


async def sample_status(sess: Any) -> dict[str, Any]:
    res = await sess.v1("network.status", {}, timeout=15, id_prefix="p0-st")
    data = sanitize(data_of(res.obj))
    return {
        "ok": bool(res.obj.get("ok")) if isinstance(res.obj, dict) else False,
        "error": (res.obj or {}).get("error") if isinstance(res.obj, dict) else None,
        "data": data,
    }


def pick(data: dict[str, Any]) -> dict[str, Any]:
    keys = (
        "sta_connected",
        "sta_has_ip",
        "sta_ip",
        "connected_ssid",
        "sta_ssid",
        "persistent_use_static",
        "persistent_static_ip",
        "cfg_state",
        "mqtt_connected",
        "mqtt_last_error",
        "password_set",
    )
    return {k: data.get(k) for k in keys}


async def wait_reconnected(
    sess: Any, baseline_ip: str | None, timeout: float = 60.0
) -> tuple[dict[str, Any], float]:
    """轮询直到 sta_connected 且有 IP；返回最后一次采样与耗时。"""
    start = time.monotonic()
    last: dict[str, Any] = {}
    while time.monotonic() - start < timeout:
        last = await sample_status(sess)
        data = last.get("data", {})
        if data.get("sta_connected") and data.get("sta_has_ip"):
            return last, time.monotonic() - start
        await asyncio.sleep(2.0)
    return last, time.monotonic() - start


async def run(args: argparse.Namespace) -> int:
    conn_args = build_args(args)
    cli, sess, target = await connect_once(conn_args)
    out: dict[str, Any] = {
        "address": target.get("address"),
        "mode": args.mode,
        "started": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "rounds": [],
    }
    try:
        info = await sess.v1("device.info", {}, timeout=10, token=False, id_prefix="p0-info")
        out["device_info"] = sanitize(data_of(info.obj))

        device_id = out["device_info"].get("device_id")
        if args.expect_device and device_id != args.expect_device:
            raise RuntimeError(
                f"target mismatch: connected to {device_id!r}, expected {args.expect_device!r}; "
                "aborting before any state-changing command"
            )

        baseline = await sample_status(sess)
        out["baseline"] = pick(baseline["data"])
        baseline_ip = baseline["data"].get("sta_ip")

        if args.mode == "reapply":
            for index in range(1, args.rounds + 1):
                t0 = time.monotonic()
                res = await sess.v1(
                    "network.reconnect", {}, timeout=20, id_prefix=f"p0-rc{index}"
                )
                accepted = bool(res.obj.get("ok")) if isinstance(res.obj, dict) else False
                err = res.obj.get("error") if isinstance(res.obj, dict) else None
                status, elapsed = await wait_reconnected(sess, baseline_ip, args.timeout)
                data = status.get("data", {})
                entry = {
                    "round": index,
                    "accepted": accepted,
                    "error": err,
                    "elapsed_s": round(elapsed, 1),
                    "wifi_stop_to_connected_s": round(time.monotonic() - t0, 1),
                    "status": pick(data),
                    "ip_matches_baseline": data.get("sta_ip") == baseline_ip,
                }
                out["rounds"].append(entry)
                print(json.dumps(entry, ensure_ascii=False))
                if args.gap > 0 and index < args.rounds:
                    await asyncio.sleep(args.gap)
    finally:
        await close_session(cli, sess)

    out["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S")
    text = json.dumps(out, ensure_ascii=False, indent=2)
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(text + "\n", encoding="utf-8")
    print(f"\n[written] {args.out}")
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="P0 static re-apply acceptance")
    parser.add_argument("--mode", choices=("base", "reapply"), default="base")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--gap", type=float, default=20.0, help="rounds 间隔秒")
    parser.add_argument("--timeout", type=float, default=60.0, help="单轮重连等待秒")
    parser.add_argument("--address", default=os.environ.get("IOTHUB_BLE_ADDR", DEFAULT_ADDRESS))
    parser.add_argument(
        "--expect-device",
        default=EXPECT_DEVICE,
        help="device_id 白名单；不匹配直接中止（防止误连 65D8）",
    )
    parser.add_argument("--user", default="admin")
    parser.add_argument(
        "--password",
        default=os.environ.get("IOTHUB_ADMIN_PASSWORD", "123456"),
        help="只从环境变量读取；缺省为工具统一测试默认值",
    )
    parser.add_argument("--out", required=True)
    return parser.parse_args(argv)


if __name__ == "__main__":
    raise SystemExit(asyncio.run(run(parse_args(sys.argv[1:]))))
