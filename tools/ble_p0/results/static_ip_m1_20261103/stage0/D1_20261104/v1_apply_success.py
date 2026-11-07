#!/usr/bin/env python3
import asyncio, json, re, sys
from pathlib import Path
sys.path.insert(0, str(Path('tools/ble_p0').resolve()))
from p0_acceptance import Session, sanitize, scan_filtered, choose_target, clear_cached_iot_targets, CMD_TX
from bleak import BleakClient
ADDR='B0:A7:32:28:C8:E6'; OUT=Path(sys.argv[1])
def data_of(o):
 d=o.get('data') if isinstance(o,dict) else None
 return d if isinstance(d,dict) else {}
def wifi_pw():
 s=Path('README.md').read_text()
 m=re.search(r'WIFI SSID：(\S+)\s+密码：(\S+)', s)
 if not m: raise SystemExit('wifi credentials not found in README')
 return m.group(1), m.group(2)
async def main():
 ssid,pw=wifi_pw(); out={'ssid':ssid}
 clear_cached_iot_targets(); rows=await scan_filtered(10); t=choose_target(rows,ADDR)
 if not t or 'C8E4' not in (t.get('name') or ''): raise SystemExit('C8E4 not found')
 cli=BleakClient(t['address'],timeout=25); await cli.connect()
 s=Session(cli,address=t['address'],username='admin',password='123456')
 await cli.start_notify(CMD_TX,s.on_notify); await asyncio.sleep(0.2); await s.login()
 st=await s.v1('network.status',{},timeout=15,id_prefix='apply-st'); out['before']=sanitize(st.obj)
 tr=await s.v1('network.tryconnect',{'ssid':ssid,'password':pw,'idempotency_key':'v1-apply-'+__import__('secrets').token_hex(3)},timeout=20,id_prefix='apply-try')
 out['tryconnect']=sanitize(tr.obj); op=data_of(tr.obj).get('operation_id')
 if not op: raise SystemExit('no operation_id')
 states=[]
 for i in range(40):
  rr=await s.v1('operation.get',{'operation_id':op},timeout=10,id_prefix='apply-op')
  d=data_of(rr.obj); states.append({'t':i*0.5,'state':d.get('state'),'error':d.get('error')})
  if d.get('state') in ('got_ip','failed','cancelled','expired'): break
  await asyncio.sleep(0.5)
 out['operation_states']=states
 if states and states[-1].get('state')=='got_ip':
  ap=await s.v1('network.apply',{'operation_id':op},timeout=20,id_prefix='apply-commit')
  out['apply']=sanitize(ap.obj)
  op2=await s.v1('operation.get',{'operation_id':op},timeout=10,id_prefix='apply-after')
  out['after_operation']=sanitize(op2.obj)
  # wait for Wi-Fi/MQTT to settle
  for i in range(15):
   time_st=await s.v1('network.status',{},timeout=15,id_prefix='apply-settle')
   d=data_of(time_st.obj)
   if d.get('mqtt_connected') is True: break
   await asyncio.sleep(1)
  out['after_status']=sanitize(time_st.obj)
  cfg=await s.v1('config.get',{},timeout=20,id_prefix='apply-cfg'); out['after_config']=sanitize(cfg.obj)
 OUT.write_text(json.dumps(out,ensure_ascii=False,indent=2))
 await s.close(); await cli.disconnect()
asyncio.run(main())
