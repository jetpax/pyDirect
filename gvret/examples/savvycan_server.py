"""
GVRET Server Example

Demonstrates:
- Starting GVRET server for SavvyCAN
- CAN bus initialization
- Remote CAN analysis over TCP
"""

import gvret
import CAN
import network
import time

# Connect to WiFi first
wlan = network.WLAN(network.STA_IF)
wlan.active(True)

if not wlan.isconnected():
    print("Connecting to WiFi...")
    wlan.connect('YOUR_SSID', 'YOUR_PASSWORD')  # Change these!
    
    while not wlan.isconnected():
        time.sleep(0.5)

ip = wlan.ifconfig()[0]
print(f"WiFi connected: {ip}")

# Initialize CAN bus
can = CAN(0, mode=CAN.NORMAL, bitrate=500000, tx=21, rx=22)
print("CAN bus initialized (500 kbps)")

# Start GVRET server
gvret.start(port=23)
print(f"GVRET server running on {ip}:23")
print("")
print("Connect from SavvyCAN:")
print("  1. Connection → Open Connection")
print("  2. Select 'Network' → 'GVRET'")
print(f"  3. Host: {ip}, Port: 23")
print("  4. Click 'Connect'")

# Main loop
try:
    while True:
        time.sleep(1)
except KeyboardInterrupt:
    print("\nStopping GVRET server...")
    gvret.stop()
