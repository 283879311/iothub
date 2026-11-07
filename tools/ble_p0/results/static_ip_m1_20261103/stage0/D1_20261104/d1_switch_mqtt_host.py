#!/usr/bin/env python3
import asyncio, json, sys
from pathlib import Path
sys.path.insert(0, str(Path('tools/ble_p0').resolve()))
from p0_acceptance import Session, sanitize, scan_filtered, choose_target, clear_cached_iot_targets, CMD_TX
from bleak import BleakClient
ADDR='B0:A7:32:28:C8:E6'; NEW_HOST='139.9.93.245'; NEW_PORT=21883
async def main():
    out={}
    clear_cached_iot_targets(); rows=await scan_filtered(8); t=choose_target(rows,ADDR)
    if not t or 'C8E4' not in (t.get('name') or ''): raise SystemExit('C8E4 not found')
    cli=BleakClient(t['address'],timeout=25); await cli.connect()
    s=Session(cli,address=t['address'],username='admin',password='123456')
    await cli.start_notify(CMD_TX,s.on_notify); await asyncio.sleep(0.2); await s.login()
    cfg=await s.v1('config.get',{},timeout=20,id_prefix='d1-sw-cfg-before'); out['before_config']=sanitize(cfg.obj)
    r=await s.v0('config.set',{'mqtt_host':NEW_HOST,'mqtt_port':NEW_PORT},req_id=301,timeout=20)
    out['config_set']=sanitize(r.obj)
    polls=[]
    for i in range(20):
        st=await s.v1('network.status',{},timeout=15,id_prefix='d1-sw-st')
        d=st.obj.get('data') or {}
        polls.append({'t':i,'mqtt_connected':d.get('mqtt_connected'),'mqtt_last_error':d.get('mqtt_last_error'),'ip':d.get('sta_ip')})
        if d.get('mqtt_connected') is True: break
        await asyncio.sleep(1.0)
    out['status_polls']=polls
    cfg2=await s.v1('config.get',{},timeout=20,id_prefix='d1-sw-cfg-after'); out['after_config']=sanitize(cfg2.obj)
    Path(sys.argv[1]).write_text(json.dumps(out,ensure_ascii=False,indent=2))
    await cli.disconnect()
asyncio.run(main())
