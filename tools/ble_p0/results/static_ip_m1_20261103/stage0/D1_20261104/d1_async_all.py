#!/usr/bin/env python3
import asyncio, json, sys
from pathlib import Path
sys.path.insert(0, str(Path('tools/ble_p0').resolve()))
from p0_acceptance import Session, sanitize, scan_filtered, choose_target, clear_cached_iot_targets, CMD_TX
from bleak import BleakClient
ADDR='B0:A7:32:28:C8:E6'
async def main():
    out={}
    clear_cached_iot_targets()
    rows=await scan_filtered(8); t=choose_target(rows, ADDR)
    if not t or 'C8E4' not in (t.get('name') or ''): raise SystemExit('C8E4 not found')
    cli=BleakClient(t['address'], timeout=25); await cli.connect()
    s=Session(cli,address=t['address'],username='admin',password='123456')
    await cli.start_notify(CMD_TX,s.on_notify); await asyncio.sleep(0.2); await s.login()
    for i,(name,method) in enumerate([('tryconnect','network.tryconnect.async'),('apply','network.apply.async'),('cancel','network.cancel.async')], start=201):
        r=await s.v0(method, {'ssid':'CMCC-502','password':'x','operation_id':'x','trial_id':'x'}, req_id=i, timeout=15)
        out[name]=sanitize(r.obj)
    Path(sys.argv[1]).write_text(json.dumps(out,ensure_ascii=False,indent=2))
    await cli.disconnect()
asyncio.run(main())
