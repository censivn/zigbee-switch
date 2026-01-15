import serial
import time
import sys

port = "/dev/cu.usbmodem202201"
baud = 115200

print(f"Opening {port} at {baud}...")
print("Monitoring for 30 seconds. Please perform your test actions (Start -> Wait -> Long Press)...")

try:
    ser = serial.Serial(port, baud, timeout=0.1)
    
    start_time = time.time()
    while time.time() - start_time < 60:
        line = ser.readline()
        if line:
            try:
                print(line.decode('utf-8', errors='replace').strip())
            except:
                print(line)
    ser.close()
    print("Monitoring finished.")
except Exception as e:
    print(f"Error: {e}")
