"""
USB Modem Example

Demonstrates:
- Initializing USB modem
- Connecting to cellular network
- Making HTTP request over cellular
"""

import usbmodem
import time
import urequests

# Initialize modem
print("Initializing USB modem...")
modem = usbmodem.Modem()

# Configure APN for your carrier
# Common APNs:
#   AT&T: "broadband"
#   T-Mobile: "fast.t-mobile.com"
#   Verizon: "vzwinternet"
#   Hologram: "hologram"
APN = "internet"  # Change this for your carrier!

modem.set_apn(APN)
print(f"APN set to: {APN}")

# Optional: Set SIM PIN if required
# modem.set_pin("1234")

# Connect to network
print("Connecting to cellular network...")
modem.connect()

# Wait for connection
while not modem.is_connected():
    print("Waiting for connection...")
    time.sleep(1)

print("Connected!")
print(f"IP Address: {modem.get_ip()}")

# Check signal quality
signal = modem.at("AT+CSQ")
print(f"Signal quality: {signal}")

# Make HTTP request
print("\nTesting internet connectivity...")
try:
    response = urequests.get("http://httpbin.org/ip")
    print(f"Response: {response.text}")
    response.close()
except Exception as e:
    print(f"Error: {e}")

print("\nDone! Modem is ready for use.")
print("Network interface is now available for all network operations.")

# Keep connection alive
try:
    while True:
        time.sleep(10)
        if not modem.is_connected():
            print("Connection lost! Reconnecting...")
            modem.connect()
except KeyboardInterrupt:
    print("\nDisconnecting...")
    modem.disconnect()
