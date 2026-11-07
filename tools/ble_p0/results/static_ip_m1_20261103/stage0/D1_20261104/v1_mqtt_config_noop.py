#!/usr/bin/env python3
import asyncio, json, sys
from pathlib import Path
sys.path.insert(0, str(Path('tools/ble_p0').resolve()))
from p0_acceptance import Session, sanitize, scan_filtered, choose_target, clear_cached_iot_targets, CMD_TX
from bleak import BleakClient
ADDR='B0:A7:32:28:C8:E6'
async def main():
 clear_cached_iot_targets(); rows=await scan_filtered(10); t=choose_target(rows,ADDR)
 if not t or 'C8E4' not in (t.get('name') or ''): raise SystemExit('C8E4 not found')
 cli=BleakClient(t['address'],timeout=25); await cli.connect(); s=Session(cli,address=t['address'],username='admin',password='123456')
 await cli.start_notify(CMD_TX,s.on_notify); await asyncio.sleep(0.2); await s.login()
 cfg=await s.v1('config.get',{},timeout=20,id_prefix='mqttnoop-before'); out={'before':sanitize(cfg.obj)}
 port=(cfg.obj.get('data') or {}).get('mqtt',{}).get('mqtt_port')
 # v1 requires args.patch; same port -> no change/reboot
 r=await s.v1('config.set',{'patch':{'mqtt_port':port}},timeout=20,id_prefix='mqttnoop-set')
 out['set_patch']=sanitize(r.obj)
 # wrong shape (what the mini program currently sends)
 w=await s.v1('config.set',{'mqtt_port':port},timeout=20,id_prefix='mqttnoop-wrongshape')
 out['set_wrong_shape']=sanitize(w.obj)
 Path(sys.argv[1]).write_text(json.dumps(out,ensure_ascii=False,indent=2))
 await s.close(); await cli.disconnect()
asyncio.run(main())
