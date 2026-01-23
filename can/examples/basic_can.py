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
    data = [i, i+1, i+2, i+3]
    can.send(data, msg_id)  # Note: data first, then id
    print(f"Sent: ID=0x{msg_id:03x}, Data={data}")
    time.sleep_ms(100)

print("")
print("Receiving frames...")

# Receive frames (in loopback mode, we'll receive what we sent)
for i in range(5):
    if can.any():
        msg = can.recv()  # Returns (id, extended, rtr, data)
        print(f"Received: ID=0x{msg[0]:03x}, Data={msg[3]}")
    else:
        print("No message available")

print("")
print("Done! For real hardware:")
print("  can = CAN(0, mode=CAN.NORMAL, bitrate=500000, tx=21, rx=22)")
