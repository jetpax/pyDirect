"""
PLC/V2G Basic Example

Demonstrates:
- Initializing PLC module
- EXI encoding/decoding for V2G messages
- Basic EVSE (charging station) mode
"""

import plc

# Initialize PLC module
print("Initializing PLC module...")
plc.init()

print("PLC module ready for V2G communication")
print("")
print("This is a basic example. Full V2G implementation requires:")
print("  1. Physical PLC hardware (SLAC, HomePlug)")
print("  2. ISO 15118 message handling")
print("  3. EXI codec for message encoding/decoding")
print("")
print("See plc/README.md for detailed implementation guide")

# Example: EXI encoding (when implemented)
# message = plc.encode_session_setup(...)
# plc.send(message)

# Example: EXI decoding (when implemented)
# data = plc.receive()
# message = plc.decode(data)
