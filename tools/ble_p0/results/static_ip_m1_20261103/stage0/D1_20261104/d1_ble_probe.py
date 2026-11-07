#!/usr/bin/env python3
import asyncio, json, sys
from pathlib import Path
sys.path.insert(0, str(Path('tools/ble_p0').resolve()))
from p0_acceptance import Session, sanitize, scan_filtered, choose_target, clear_cached_iot_targets
from bleak import BleakClient

ADDR = 'B0:A7:32:28:C8:E6'

async def main():
    clear_cached_iot_targets()
    rows = await scan_filtered(10)
    target = choose_target(rows, ADDR)
    if target is None:
        raise SystemExit('target not found')
    cli = BleakClient(target['address'], timeout=25)
    await cli.connect()
    s = Session(cli, address=target['address'], username='admin', password='123456')
    await cli.start_notify('6e400005-b5a3-f393-e0a9-e50e24dcca9e', s.on_notify)
    await asyncio.sleep(0.2)
    login = await s.login()
    out = {'address': target['address'], 'name': target.get('name'), 'login_ok': login.obj.get('ok') is True}
    for name, method, args in [
        ('device_info','device.info',{}),
        ('device_status','device.status',{}),
        ('config_get','config.get',{}),
        ('network_status','network.status',{}),
    ]:
        r = await s.v1(method, args, timeout=20, id_prefix='d1-'+name)
        out[name] = sanitize(r.obj)
    print(json.dumps(out, ensure_ascii=False, indent=2))
    await cli.disconnect()

asyncio.run(main())
