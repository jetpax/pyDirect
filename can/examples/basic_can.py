"""
CAN Bus Basic Example

Demonstrates:
- Initializing CAN bus
- Sending CAN frames
- Receiving CAN frames
- Using loopback mode for testing
"""

import CAN
import time

# Initialize CAN in loopback mode (for testing without hardware)
# For real hardware, use mode=CAN.NORMAL
can = CAN(0, mode=CAN.LOOPBACK, bitrate=500000, tx=21, rx=22)

print("CAN bus initialized in loopback mode")
print("Bitrate: 500 kbps")
print("")

# Send some test frames
print("Sending frames...")
for i in range(5):
    msg_id = 0x100 + i
    data = bytes([i, i+1, i+2, i+3])
    can.send(msg_id, data)
    print(f"Sent: ID=0x{msg_id:03x}, Data={data.hex()}")
    time.sleep_ms(100)

print("")
print("Receiving frames...")

# Receive frames (in loopback mode, we'll receive what we sent)
for i in range(5):
    msg_id, data = can.recv(timeout=1000)
    if msg_id is not None:
        print(f"Received: ID=0x{msg_id:03x}, Data={data.hex()}")
    else:
        print("Timeout - no message received")

print("")
print("Done! For real hardware:")
print("  can = CAN(0, mode=CAN.NORMAL, bitrate=500000, tx=21, rx=22)")
