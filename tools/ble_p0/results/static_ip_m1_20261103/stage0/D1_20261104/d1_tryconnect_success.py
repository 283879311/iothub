#!/usr/bin/env python3
import asyncio, json, os, sys
from pathlib import Path
sys.path.insert(0, str(Path('tools/ble_p0').resolve()))
from p0_acceptance import Session, sanitize, scan_filtered, choose_target, clear_cached_iot_targets, CMD_TX
from bleak import BleakClient
ADDR='B0:A7:32:28:C8:E6'
async def main():
    pw=os.environ.get('IOTHUB_WIFI_PASSWORD')
    if not pw: raise SystemExit('missing IOTHUB_WIFI_PASSWORD')
    out={}
    clear_cached_iot_targets(); rows=await scan_filtered(8); t=choose_target(rows,ADDR)
    if not t or 'C8E4' not in (t.get('name') or ''): raise SystemExit('C8E4 not found')
    cli=BleakClient(t['address'],timeout=25); await cli.connect()
    s=Session(cli,address=t['address'],username='admin',password='123456')
    await cli.start_notify(CMD_TX,s.on_notify); await asyncio.sleep(0.2); await s.login()
    st=await s.v1('network.status',{},timeout=15,id_prefix='d1-tc-st'); out['before']=sanitize(st.obj)
    ssid=(st.obj.get('data') or {}).get('sta_ssid') or 'CMCC-502'
    r=await s.v1('network.tryconnect',{'ssid':ssid,'password':pw,'idempotency_key':'d1-tc-'+os.urandom(4).hex()},timeout=20,id_prefix='d1-tc')
    out['tryconnect']=sanitize(r.obj)
    op=(r.obj.get('data') or {}).get('operation_id')
    states=[]
    if op:
        for i in range(30):
            rr=await s.v1('operation.get',{'operation_id':op},timeout=10,id_prefix='d1-tc-op')
            d=(rr.obj.get('data') or {})
            states.append({'t':i*0.5,'state':d.get('state'),'error':d.get('error')})
            if d.get('state') in ('got_ip','failed','cancelled','expired'):
                break
            await asyncio.sleep(0.5)
        out['operation_states']=states
        cr=await s.v1('network.cancel',{'operation_id':op},timeout=15,id_prefix='d1-tc-cancel')
        out['cancel']=sanitize(cr.obj)
    out['after']=sanitize((await s.v1('network.status',{},timeout=15,id_prefix='d1-tc-after')).obj)
    Path(sys.argv[1]).write_text(json.dumps(out,ensure_ascii=False,indent=2))
    await cli.disconnect()
asyncio.run(main())
