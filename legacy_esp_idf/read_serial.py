import serial
import time
import sys

port = "/dev/cu.usbmodem202201"
baud = 115200

print(f"Opening {port} at {baud}...")
try:
    ser = serial.Serial(port, baud, timeout=0.1)
    print("Listening for 5 seconds. Please press RESET on the board now!")
    
    start_time = time.time()
    while time.time() - start_time < 5:
        line = ser.readline()
        if line:
            try:
                print(line.decode('utf-8', errors='replace').strip())
            except:
                print(line)
    ser.close()
except Exception as e:
    print(f"Error: {e}")
