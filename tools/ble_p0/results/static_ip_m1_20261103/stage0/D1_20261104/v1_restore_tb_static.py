#!/usr/bin/env python3
import asyncio, json, sys
from pathlib import Path
sys.path.insert(0, str(Path('tools/ble_p0').resolve()))
from p0_acceptance import Session, sanitize, scan_filtered, choose_target, clear_cached_iot_targets, CMD_TX
from bleak import BleakClient
ADDR='B0:A7:32:28:C8:E6'; OUT=Path(sys.argv[1])
def data_of(o):
 d=o.get('data') if isinstance(o,dict) else None
 return d if isinstance(d,dict) else {}
async def conn():
 clear_cached_iot_targets(); rows=await scan_filtered(10); t=choose_target(rows,ADDR)
 if not t: raise RuntimeError('C8E4 not found')
 cli=BleakClient(t['address'],timeout=25); await cli.connect(); s=Session(cli,address=t['address'],username='admin',password='123456')
 await cli.start_notify(CMD_TX,s.on_notify); await asyncio.sleep(.2); await s.login(); return cli,s
async def main():
 out={}; cli,s=await conn()
 patch={'mqtt_host':'139.9.93.245','mqtt_port':21883,'sta_use_static':True,'sta_static_ip':'192.168.1.50','sta_static_gateway':'192.168.1.1','sta_static_netmask':'255.255.255.0','sta_static_dns1':'223.5.5.5','sta_static_dns2':'8.8.8.8'}
 r=await s.v1('config.set',{'patch':patch},timeout=20,id_prefix='restore-set'); out['set']=sanitize(r.obj)
 await s.close(); await cli.disconnect(); await asyncio.sleep(12)
 cli,s=await conn()
 for i in range(20):
  st=await s.v1('network.status',{},timeout=15,id_prefix='restore-st'); out['status']=sanitize(st.obj); d=data_of(st.obj)
  if d.get('mqtt_connected') is True: break
  await asyncio.sleep(1)
 cfg=await s.v1('config.get',{},timeout=20,id_prefix='restore-cfg'); out['config']=sanitize(cfg.obj)
 OUT.write_text(json.dumps(out,ensure_ascii=False,indent=2)); await s.close(); await cli.disconnect()
asyncio.run(main())
