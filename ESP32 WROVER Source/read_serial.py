import serial, time, sys
s = serial.Serial('COM4', 115200, timeout=1)
s.setDTR(False)
s.setRTS(False)
time.sleep(0.1)
s.setDTR(True)
s.setRTS(True)
time.sleep(0.1)
s.setDTR(False)
s.setRTS(False)
print("COM4 opened, monitoring...", flush=True)
try:
    for _ in range(500):
        line = s.readline().decode('utf-8', 'replace')
        if line:
            print(line, end='', flush=True)
except KeyboardInterrupt:
    pass
finally:
    s.close()
