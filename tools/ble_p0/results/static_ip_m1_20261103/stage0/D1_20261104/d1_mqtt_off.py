#!/usr/bin/env python3
import json, os, sys, threading, time, secrets
from pathlib import Path
import paho.mqtt.client as mqtt

HOST=os.environ.get('IOTHUB_MQTT_HOST','139.9.93.245'); PORT=int(os.environ.get('IOTHUB_MQTT_PORT','21883'))
OUT=Path(sys.argv[1])
token=os.environ.get('IOTHUB_MQTT_TOKEN')
if not token:
    raise SystemExit('missing IOTHUB_MQTT_TOKEN')
out={'host':HOST,'port':PORT,'responses':{}}
connected=threading.Event()
msgs={}
lock=threading.Lock()

def on_connect(c,u,f,rc,props=None):
    out['connack']=rc
    if rc==0:
        c.subscribe('v1/devices/me/rpc/response/+',qos=1)
        c.subscribe('iothub/rpc/resp',qos=1)
        connected.set()

def on_message(c,u,msg):
    try:
        obj=json.loads(msg.payload.decode())
    except Exception:
        return
    rid=obj.get('id')
    with lock:
        msgs[str(rid)]=obj

c=mqtt.Client(client_id='d1-mqtt-'+secrets.token_hex(4), protocol=mqtt.MQTTv311)
c.username_pw_set(token)
c.on_connect=on_connect; c.on_message=on_message
c.connect(HOST,PORT,keepalive=20); c.loop_start()
if not connected.wait(10):
    print(json.dumps({'error':'mqtt connect timeout','connack':out.get('connack')}))
    sys.exit(2)

def req(name,cmd,args,timeout=8):
    rid='d1-'+name+'-'+secrets.token_hex(3)
    payload=json.dumps({'id':rid,'version':'1.0','cmd':cmd,'args':args},separators=(',',':'))
    c.publish('v1/devices/me/rpc/request/'+rid,payload,qos=1)
    c.publish('iothub/rpc',payload,qos=1)
    end=time.time()+timeout
    while time.time()<end:
        with lock:
            if rid in msgs:
                out['responses'][name]=msgs[rid]
                return msgs[rid]
        time.sleep(0.05)
    out['responses'][name]={'timeout':True}
    return None

req('status','network.status',{})
req('tryconnect','network.tryconnect',{'ssid':'CMCC-502','password':'x','idempotency_key':'d1-off-'+secrets.token_hex(3)})
req('apply','network.apply',{'operation_id':'op-D1NONEXISTENT'})
req('cancel','network.cancel',{'operation_id':'op-D1NONEXISTENT'})
OUT.write_text(json.dumps(out,ensure_ascii=False,indent=2))
c.loop_stop(); c.disconnect()
print(json.dumps({'connack':out.get('connack'),'names':list(out['responses'])}))
