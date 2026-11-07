#!/usr/bin/env python3
import asyncio, json, sys, time
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
 if not t or 'C8E4' not in (t.get('name') or ''): raise RuntimeError('C8E4 not found')
 cli=BleakClient(t['address'],timeout=25); await cli.connect()
 s=Session(cli,address=t['address'],username='admin',password='123456')
 await cli.start_notify(CMD_TX,s.on_notify); await asyncio.sleep(0.2); await s.login()
 return cli,s
async def read_state(s):
 cfg=await s.v1('config.get',{},timeout=20,id_prefix='staticw-cfg'); net=(data_of(cfg.obj).get('network') or {})
 st=await s.v1('network.status',{},timeout=15,id_prefix='staticw-st'); sn=data_of(st.obj)
 return {'config_network':sanitize(net),'status':sanitize(sn)}
async def main():
 out={}
 cli,s=await connect()
 out['before']=await read_state(s)
 patch={'sta_use_static':True,'sta_static_ip':'192.168.1.51','sta_static_gateway':'192.168.1.1','sta_static_netmask':'255.255.255.0','sta_static_dns1':'223.5.5.5','sta_static_dns2':'8.8.8.8'}
 r=await s.v1('config.set',{'patch':patch},timeout=20,id_prefix='staticw-set')
 out['set']=sanitize(r.obj)
 await s.close(); await cli.disconnect()
 await asyncio.sleep(12)
 cli,s=await connect(); out['after_set']=await read_state(s)
 # restore .50
 r=await s.v1('config.set',{'patch':{'sta_static_ip':'192.168.1.50'}},timeout=20,id_prefix='staticw-restore')
 out['restore_cmd']=sanitize(r.obj)
 await s.close(); await cli.disconnect()
 await asyncio.sleep(12)
 cli,s=await connect()
 for i in range(20):
  st=await read_state(s)
  if st['status'].get('mqtt_connected') is True: break
  await asyncio.sleep(1)
 out['after_restore']=st
 await s.close(); await cli.disconnect()
 OUT.write_text(json.dumps(out,ensure_ascii=False,indent=2))
 print(json.dumps({'set_ok':r.obj.get('ok') if False else out.get('set',{}).get('ok'),'after_set_ip':out.get('after_set',{}).get('status',{}).get('sta_ip'),'after_restore_ip':out.get('after_restore',{}).get('status',{}).get('sta_ip'),'after_restore_mqtt':out.get('after_restore',{}).get('status',{}).get('mqtt_connected')},ensure_ascii=False))
asyncio.run(main())
