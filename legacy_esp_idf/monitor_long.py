import serial
import time
import sys

port = "/dev/cu.usbmodem202201"
baud = 115200

print(f"Opening {port} at {baud}...")
print("Monitoring for 90 seconds. Please observe the LED behavior.")

try:
    ser = serial.Serial(port, baud, timeout=0.1)
    
    start_time = time.time()
    while time.time() - start_time < 90:
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
