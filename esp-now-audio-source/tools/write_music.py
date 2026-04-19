"""Write MP3 to flash partition with 4-byte size header.
Usage: python write_music.py <port> <mp3_file>
"""
import sys, struct, subprocess, os, tempfile

if len(sys.argv) < 3:
    print(f"Usage: {sys.argv[0]} <port> <mp3_file>")
    sys.exit(1)

port = sys.argv[1]
mp3_path = sys.argv[2]

with open(mp3_path, 'rb') as f:
    mp3_data = f.read()

size = len(mp3_data)
print(f"MP3 file: {mp3_path} ({size} bytes)")

# Create temp file: 4-byte LE size + MP3 data
tmp = tempfile.NamedTemporaryFile(delete=False, suffix='.bin')
tmp.write(struct.pack('<I', size))
tmp.write(mp3_data)
tmp.close()

print(f"Partition image: {tmp.name} ({size + 4} bytes)")
print(f"Flashing to {port} at 0x400000...")

# First erase the partition, then write
ret = subprocess.run([
    sys.executable, '-m', 'esptool',
    '--chip', 'esp32', '-p', port, '-b', '460800',
    '--before', 'default_reset', '--after', 'hard_reset',
    'write_flash', '0x400000', tmp.name
], check=False)

os.unlink(tmp.name)

if ret.returncode == 0:
    print("Done!")
else:
    print("Flash failed!")
    sys.exit(1)
