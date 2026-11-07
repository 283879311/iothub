import serial,time,sys
s=serial.Serial('/dev/ttyUSB1',115200,timeout=0.2)
s.setDTR(False); s.setRTS(False); time.sleep(0.2)
s.setRTS(True); time.sleep(0.15); s.setRTS(False)
end=time.time()+45
while time.time()<end:
 d=s.read(4096)
 if d: sys.stdout.buffer.write(d); sys.stdout.buffer.flush()
s.close()
