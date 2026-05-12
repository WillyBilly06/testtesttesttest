"""Read COM port for a fixed duration, print lines, and exit. Used to capture
serial output non-interactively from the agent."""
import sys
import time
import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM7"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
duration_s = float(sys.argv[3]) if len(sys.argv) > 3 else 60.0

ser = serial.Serial(port, baud, timeout=0.2)
deadline = time.time() + duration_s
buf = b""
done_markers = (
    b"*** C6 UPGRADED",
    b"already at v",
    b"[FAIL]",
    b"OTA not needed",
)
while time.time() < deadline:
    chunk = ser.read(4096)
    if not chunk:
        continue
    buf += chunk
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        try:
            print(line.decode("utf-8", errors="replace").rstrip("\r"))
        except Exception:
            print(repr(line))
    sys.stdout.flush()
    if any(m in buf for m in done_markers):
        # Read a little more then stop
        deadline = min(deadline, time.time() + 3.0)

ser.close()
