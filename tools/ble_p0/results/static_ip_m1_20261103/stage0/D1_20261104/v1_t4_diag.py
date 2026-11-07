#!/usr/bin/env python3
import asyncio, json, time, sys
from pathlib import Path
sys.path.insert(0, str(Path('tools/ble_p0').resolve()))
from p0_acceptance import Session, sanitize, scan_filtered, choose_target, clear_cached_iot_targets, CMD_TX
from bleak import BleakClient
ADDR='B0:A7:32:28:C8:E6'; OUT=Path(sys.argv[1])
def data_of(o):
 d=o.get('data') if isinstance(o,dict) else None
 return d if isinstance(d,dict) else {}
async def connect():
 clear_cached_iot_targets(); rows=await scan_filtered(10); t=choose_target(rows,ADDR)
 if not t or 'C8E4' not in (t.get('name') or ''): return None,None,None,rows
 cli=BleakClient(t['address'],timeout=25); await cli.connect()
 s=Session(cli,address=t['address'],username='admin',password='123456')
 await cli.start_notify(CMD_TX,s.on_notify); await asyncio.sleep(0.2); await s.login()
 return cli,s,t,rows
async def main():
 out={'rounds':[],'scans':[]}
 cli,s,t,rows=await connect()
 if not cli: raise SystemExit('C8E4 not found')
 out['initial_scan_hits']=[{'name':x.get('name'),'address':x.get('address')} for x in rows]
 st=await s.v1('device.status',{},timeout=20,id_prefix='t4diag-st'); out['device_status']=sanitize(st.obj)
 for i in range(10):
  t0=time.time()
  try:
   rr=await s.v1('network.scan',{},timeout=25,id_prefix=f't4diag-scan-{i}')
   d=data_of(rr.obj); out['scans'].append({'i':i,'ok':rr.obj.get('ok') is True,'ms':round((time.time()-t0)*1000),'count':d.get('count'),'err':rr.obj.get('error')})
  except Exception as e:
   out['scans'].append({'i':i,'ok':False,'ms':round((time.time()-t0)*1000),'error':repr(e)})
  await asyncio.sleep(0.3)
 await s.close(); await cli.disconnect()
 # 3 reconnect rounds: scan after 1.2s, connect/login
 for rnd in range(1,4):
  await asyncio.sleep(1.2); t0=time.time()
  try:
   rows=await scan_filtered(10); found=choose_target(rows,ADDR)
   rec={'round':rnd,'scan_ms':round((time.time()-t0)*1000),'hits':[{'name':x.get('name'),'address':x.get('address')} for x in rows],'found':bool(found)}
   if found:
    cli=BleakClient(found['address'],timeout=25); await cli.connect()
    s=Session(cli,address=found['address'],username='admin',password='123456'); await cli.start_notify(CMD_TX,s.on_notify); await asyncio.sleep(0.2); await s.login()
    di=await s.v1('device.info',{},timeout=15,id_prefix=f't4diag-r{rnd}-info'); rec['info_ok']=di.obj.get('ok') is True
    await s.close(); await cli.disconnect()
  except Exception as e:
   rec={'round':rnd,'scan_ms':round((time.time()-t0)*1000),'error':repr(e)}
  out['rounds'].append(rec)
 OUT.write_text(json.dumps(out,ensure_ascii=False,indent=2))
asyncio.run(main())
