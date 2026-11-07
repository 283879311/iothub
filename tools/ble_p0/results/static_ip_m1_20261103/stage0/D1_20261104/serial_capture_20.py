import serial, time, sys
s=serial.Serial('/dev/ttyUSB1',115200,timeout=0.2)
end=time.time()+25
while time.time()<end:
 d=s.read(4096)
 if d: sys.stdout.buffer.write(d); sys.stdout.buffer.flush()
s.close()
