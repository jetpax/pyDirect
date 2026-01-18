"""
WebREPL Example

Demonstrates:
- Starting WebREPL over WebSocket
- Password protection
- Remote Python shell access
"""

import webrepl_binary
import httpserver
import time

# Set WebREPL password
PASSWORD = "mypassword"  # Change this!

# Start HTTP server (required for WebSocket transport)
httpserver.start(80)

# Start WebREPL
print("Starting WebREPL...")
webrepl_binary.set_password(PASSWORD)
webrepl_binary.start()

print(f"WebREPL running on wss://device-ip/webrepl")
print(f"Password: {PASSWORD}")
print("")
print("Connect from browser:")
print("  1. Visit https://micropython.org/webrepl/")
print("  2. Enter: wss://device-ip/webrepl")
print(f"  3. Password: {PASSWORD}")

# Main loop
try:
    while True:
        httpserver.process_queue()
        webrepl_binary.process_queue()
        time.sleep_ms(10)
except KeyboardInterrupt:
    print("\nStopping WebREPL...")
    webrepl_binary.stop()
    httpserver.stop()
