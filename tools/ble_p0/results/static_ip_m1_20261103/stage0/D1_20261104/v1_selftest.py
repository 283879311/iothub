#!/usr/bin/env python3
import asyncio, json, secrets, sys
from pathlib import Path
sys.path.insert(0, str(Path('tools/ble_p0').resolve()))
from p0_acceptance import Session, sanitize, scan_filtered, choose_target, clear_cached_iot_targets, CMD_TX
from bleak import BleakClient
ADDR='B0:A7:32:28:C8:E6'; OUT=Path(sys.argv[1])
def data_of(o):
 d=o.get('data') if isinstance(o,dict) else None
 return d if isinstance(d,dict) else {}
async def main():
 r={'tests':{}}
 clear_cached_iot_targets(); rows=await scan_filtered(10); t=choose_target(rows,ADDR)
 if not t or 'C8E4' not in (t.get('name') or ''): raise SystemExit('C8E4 not found')
 r['name']=t.get('name')
 cli=BleakClient(t['address'],timeout=25); await cli.connect()
 s=Session(cli,address=t['address'],username='admin',password='123456')
 await cli.start_notify(CMD_TX,s.on_notify); await asyncio.sleep(0.2); await s.login()
 async def v1(name,cmd,args=None,timeout=20):
  rr=await s.v1(cmd,args or {},timeout=timeout,id_prefix='v1self-'+name)
  r['tests'][name]=sanitize(rr.obj); return rr
 await v1('device_info','device.info',{})
 await v1('device_status','device.status',{})
 await v1('gpio_status','gpio.status',{})
 await v1('uart_runtime','uart.runtime',{})
 await v1('config_get','config.get',{})
 await v1('io_get','io.get',{})
 await v1('network_status','network.status',{})
 await v1('profiles','network.profile.list',{})
 await v1('config_set_probe','config.set',{'patch':{'__v1_probe':1}})
 await v1('config_set_net_noop','config.set',{'patch':{'net_mode':'sta'}})
 await v1('fake_apply','network.apply',{'operation_id':'op-NOTFOUND'})
 await v1('fake_cancel','network.cancel',{'operation_id':'op-NOTFOUND'})
 # v1 validation: strict ip_mode / static pin
 await v1('bad_ip_mode','network.tryconnect',{'ssid':'P0-STATIC-PIN','ip_mode':'static'})
 await v1('bad_leave_static','network.tryconnect',{'ssid':'P0-STATIC-PIN','allow_leave_static':True})
 r['tests']['static_pin']=sanitize((await s.v1('network.tryconnect',{'ssid':'P0-STATIC-PIN-'+secrets.token_hex(2)},timeout=20,id_prefix='v1self-staticpin')).obj)
 # current SSID, wrong password: transaction created then cancel
 st=data_of((await s.v1('network.status',{},timeout=15,id_prefix='v1self-st2')).obj)
 ssid=st.get('sta_ssid') or 'CMCC-502'
 tr=await s.v1('network.tryconnect',{'ssid':ssid,'password':'v1-wrong-'+secrets.token_hex(4),'idempotency_key':'v1self-'+secrets.token_hex(3)},timeout=20,id_prefix='v1self-try')
 r['tests']['tryconnect_active']=sanitize(tr.obj); op=data_of(tr.obj).get('operation_id')
 if op:
  rr=await s.v1('operation.get',{'operation_id':op},timeout=10,id_prefix='v1self-op'); r['tests']['operation']=sanitize(rr.obj)
  cr=await s.v1('network.cancel',{'operation_id':op},timeout=15,id_prefix='v1self-cancel'); r['tests']['cancel']=sanitize(cr.obj)
 # v1 scan (may be long)
 try:
  sc=await s.v1('network.scan',{},timeout=25,id_prefix='v1self-scan'); r['tests']['scan']=sanitize(sc.obj)
 except Exception as e: r['tests']['scan']={'error':repr(e)}
 # v0 must be rejected
 for name,method,params in [('v0_status','device.status',{}),('v0_config','config.set',{'__v1_probe':1})]:
  try:
   rr=await s.v0(method,params,req_id=701,timeout=10); r['tests'][name]=sanitize(rr.obj)
  except Exception as e: r['tests'][name]={'error':repr(e)}
 OUT.write_text(json.dumps(r,ensure_ascii=False,indent=2))
 await cli.disconnect()
asyncio.run(main())
