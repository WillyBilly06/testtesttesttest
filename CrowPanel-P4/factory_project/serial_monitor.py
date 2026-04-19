import serial
import time
import sys

port = 'COM13'
baud = 115200

try:
    s = serial.Serial(port, baud, timeout=1)
    # Toggle DTR/RTS to reset the board
    s.dtr = False
    s.rts = True
    time.sleep(0.1)
    s.rts = False
    time.sleep(0.1)
    
    # Read boot output for ~15 seconds
    start = time.time()
    while time.time() - start < 15:
        line = s.readline()
        if line:
            try:
                text = line.decode('utf-8', 'ignore').rstrip()
                print(text)
                sys.stdout.flush()
            except:
                pass
    s.close()
except Exception as e:
    print(f"Error: {e}")
