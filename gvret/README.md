# GVRET Module

GVRET protocol implementation for CAN bus communication over TCP, compatible with SavvyCAN and other GVRET-compatible tools.

## Overview

The GVRET module provides a TCP server that implements the GVRET protocol, allowing remote CAN bus analysis and interaction using tools like [SavvyCAN](https://www.savvycan.com/). It acts as a bridge between the ESP32's CAN controller and network-based CAN analysis tools.

## Features

- **GVRET Protocol** - Full implementation of the GVRET serial protocol over TCP
- **SavvyCAN Compatible** - Works with SavvyCAN and other GVRET-compatible tools
- **Dual CAN Support** - Supports both CAN0 and CAN1 interfaces
- **Bidirectional** - Send and receive CAN frames over the network
- **Filtering** - Hardware CAN filtering support
- **Thread-Safe** - Uses FreeRTOS synchronization primitives

## Dependencies

- **CAN Module** - Requires the `can` module for CAN bus access
- **Network** - Requires WiFi or Ethernet connectivity

## Python API

```python
import gvret

# Start GVRET server on port 23 (default)
gvret.start(port=23)

# Stop GVRET server
gvret.stop()

# Check if server is running
is_running = gvret.is_running()
```

## Usage Example

```python
import gvret
import network

# Connect to WiFi
wlan = network.WLAN(network.STA_IF)
wlan.active(True)
wlan.connect('SSID', 'password')

# Start GVRET server
gvret.start(port=23)
print(f"GVRET server running on {wlan.ifconfig()[0]}:23")

# Connect from SavvyCAN:
# Connection → Open Connection → Network → GVRET
# Host: <device-ip>, Port: 23
```

## SavvyCAN Configuration

1. Open SavvyCAN
2. Connection → Open Connection
3. Select "Network" → "GVRET"
4. Enter device IP address and port (default: 23)
5. Click "Connect"

## Protocol Details

The GVRET protocol is a simple binary protocol for CAN frame transmission:

- **Frame Format**: Binary packed CAN frames
- **Commands**: Start/stop capture, set filters, configure bitrate
- **Transport**: TCP socket (default port 23)

## Build Configuration

Enable in `build.sh`:
```bash
./build.sh BOARD gvret
```

Or in CMake:
```cmake
-DMODULE_PYDIRECT_GVRET=ON
```

## Implementation Notes

- Uses CAN manager API from `can` module
- Maintains separate RX queue for network transmission
- Thread-safe frame buffering
- Automatic reconnection handling

## See Also

- [CAN Module](../can/README.md) - CAN bus interface
- [SavvyCAN Documentation](https://www.savvycan.com/)
