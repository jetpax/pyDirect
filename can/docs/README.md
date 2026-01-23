# CAN Module (TWAI)

The CAN module provides CAN bus (TWAI) support for ESP32 platforms.

## Features

- CAN 2.0A/2.0B support (standard and extended frames)
- Hardware filtering
- Loopback mode for testing
- Configurable bitrates (1k-1000k bps)

## Quick Example

```python
import CAN

# Initialize CAN in loopback mode (for testing without external hardware)
can = CAN(0, tx=5, rx=4, mode=CAN.LOOPBACK, bitrate=500000)

# Send a frame
can.send([0x01, 0x02, 0x03], 0x123)  # data, id

# Receive (returns: id, extended, rtr, data)
if can.any():
    msg = can.recv()
    print(f"ID: 0x{msg[0]:03x}, Data: {msg[3]}")
```

## API Reference

### Constructor

```python
CAN(bus, tx=pin, rx=pin, mode=CAN.NORMAL, bitrate=500000, extframe=False)
```

- `bus`: CAN bus number (0 or 1)
- `tx`/`rx`: GPIO pins for CAN TX/RX
- `mode`: `CAN.NORMAL`, `CAN.LOOPBACK`, `CAN.SILENT`, `CAN.SILENT_LOOPBACK`
- `bitrate`: 1000 to 1000000 bps
- `extframe`: True for 29-bit IDs, False for 11-bit

### Methods

- `send(data, id)` - Send a CAN frame
- `recv()` - Receive a frame (id, extended, rtr, data)
- `any()` - Check if frames are available
- `deinit()` - Shutdown CAN interface

## Supported Bitrates

1k, 5k, 10k, 12.5k, 16k, 20k, 25k, 50k, 100k, 125k, 250k, 500k, 800k, 1000k bps

## Credits

Based on [micropython-esp32-twai](https://github.com/vostraga/micropython-esp32-twai) (MIT License).