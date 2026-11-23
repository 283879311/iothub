import asyncio
import importlib.util
import json
import os
import random
import string
import sys
from pathlib import Path

MOD_PATH = Path("/tmp/iothub-h1-build/tools/ble_p0/p0_acceptance.py")
spec = importlib.util.spec_from_file_location("p0_acceptance", MOD_PATH)
p0 = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = p0
spec.loader.exec_module(p0)


async def main() -> int:
    user = os.environ["IOTHUB_BLE_USER"]
    password = os.environ["IOTHUB_BLE_PASSWORD"]
    records = await p0.scan_filtered(12)
    target = p0.choose_target(records, None)
    if not target:
        print("NO_TARGET")
        return 2
    address = target["address"]
    wrong = "p0-wrong-" + "".join(random.choices(string.ascii_letters + string.digits, k=12))
    print(f"[focus] target={address} name={target.get('name')!r}")
    async with p0.BleakClient(address, timeout=25) as cli:
        sess = p0.Session(cli, address=address, username=user, password=password)
        await cli.start_notify(p0.CMD_TX, sess.on_notify)
        await asyncio.sleep(0.2)
        info = await sess.v1("device.info", {}, timeout=10, token=False, id_prefix="info")
        print("[info]", json.dumps(p0.sanitize(info.obj), ensure_ascii=False))
        await sess.login(user, password)
        await asyncio.sleep(0.25)
        result = await p0.test_error_password(sess, "CMCC-502", wrong)
        print("[5a]", json.dumps(p0.sanitize(result), ensure_ascii=False, indent=1))
        Path("/tmp/h1-device/focus_5a_result.json").write_text(
            json.dumps(p0.sanitize(result), ensure_ascii=False, indent=1), encoding="utf-8")
        return 0 if result.get("pass") else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
