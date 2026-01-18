"""
Husarnet P2P VPN Example

Demonstrates:
- Joining Husarnet network
- Getting device IPv6 address
- Creating P2P TCP server
"""

import husarnet
import socket
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

print(f"WiFi connected: {wlan.ifconfig()[0]}")

# Join Husarnet network
# Get your join code from: https://app.husarnet.com/
JOIN_CODE = "fc94:xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:xxxx/xxxxxxxxxxxxxxxxxxxxxxxxx"

print("Joining Husarnet network...")
husarnet.join(JOIN_CODE)

# Wait for connection
while not husarnet.status()['connected']:
    print("Waiting for Husarnet connection...")
    time.sleep(1)

ipv6 = husarnet.get_ipv6()
print(f"Husarnet connected!")
print(f"IPv6: {ipv6}")
print("")

# Create TCP server on Husarnet IPv6
server = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
server.bind((ipv6, 8080))
server.listen(1)

print(f"Server listening on [{ipv6}]:8080")
print("Connect from another device:")
print(f"  telnet {ipv6} 8080")

# Accept connections
try:
    while True:
        print("Waiting for connection...")
        client, addr = server.accept()
        print(f"Client connected from {addr}")
        
        client.send(b"Hello from Husarnet!\n")
        client.close()
        
except KeyboardInterrupt:
    print("\nStopping server...")
    server.close()
    husarnet.leave()
