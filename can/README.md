# TWAI/CAN Module

ESP32 TWAI (Two-Wire Automotive Interface) / CAN bus support for MicroPython.

This module exposes ESP-IDF's TWAI functionality, allowing MicroPython applications to communicate over CAN bus networks.

## Attribution

This module is based on work from:
- [micropython-esp32-twai](https://github.com/vostraga/micropython-esp32-twai) by vostraga
- [MicroPython PR #12331](https://github.com/micropython/micropython/pull/12331)

**Original Contributors:**
- Copyright (c) 2019 Musumeci Salvatore
- Copyright (c) 2021 Ihor Nehrutsa  
- Copyright (c) 2022 Yuriy Makarov

**License:** MIT License - See `mod_can.c` header for full license text.

## Features

- Support for all ESP32 variants (ESP32, ESP32-C3, ESP32-S2, ESP32-S3)
- Universal timing configuration for compatibility across variants
- Async/await support
- Loopback mode for testing
- Configurable bitrates: 1k, 5k, 10k, 12.5k, 16k, 20k, 25k, 50k, 100k, 125k, 250k, 500k, 800k, 1000k bps

## Usage

```python
import CAN

# Create CAN device
# Parameters: controller_id, extframe, tx_pin, rx_pin, mode, bitrate, auto_restart
dev = CAN(0, extframe=False, tx=5, rx=4, mode=CAN.LOOPBACK, bitrate=50000, auto_restart=False)

# Send message
msg_id = 0x123
msg_data = [0x01, 0x02, 0x03, 0x04]
dev.send(msg_data, msg_id)

# Receive message (blocking)
if dev.any():
    data = dev.recv()
    can_id = data[0]
    extended = data[1]
    rtr = data[2]
    payload = data[3]
    print(f"Received: id={hex(can_id)}, data={payload}")
```

## Async Example

```python
import asyncio
import CAN

dev = CAN(0, extframe=False, tx=5, rx=4, mode=CAN.LOOPBACK, bitrate=50000, auto_restart=False)

async def reader():
    while True:
        if dev.any():
            data = dev.recv()
            print(f"RECEIVED: id:{hex(data[0])}, data:{data[3]}")
        await asyncio.sleep(0.01)

async def sender():
    counter = 0
    while True:
        dev.send([counter & 0xFF, (counter >> 8) & 0xFF], 0x123)
        print(f"SENT: id:0x123, counter={counter}")
        counter += 1
        await asyncio.sleep(1)

async def main():
    await asyncio.gather(reader(), sender())

asyncio.run(main())
```

## Modes

- `CAN.NORMAL` - Normal operation
- `CAN.LOOPBACK` - Loopback mode (tx and rx pins internally connected)
- `CAN.NO_ACK` - No acknowledgment mode

## Testing

For loopback testing, you can connect tx and rx pins together:

```python
# Hardware loopback: connect pin 5 to pin 4
dev = CAN(0, tx=5, rx=4, mode=CAN.NORMAL, bitrate=500000)
```

Or use software loopback:

```python
dev = CAN(0, tx=5, rx=4, mode=CAN.LOOPBACK, bitrate=500000)
```

## API Reference

### CAN(controller_id, extframe=False, tx=5, rx=4, mode=CAN.NORMAL, bitrate=500000, auto_restart=False)

Create a CAN device instance.

**Parameters:**
- `controller_id`: CAN controller ID (0 or 1)
- `extframe`: Enable extended frame format (29-bit IDs)
- `tx`: GPIO pin for transmit
- `rx`: GPIO pin for receive
- `mode`: CAN mode (NORMAL, LOOPBACK, NO_ACK)
- `bitrate`: CAN bus bitrate in bits per second
- `auto_restart`: Automatically restart on bus-off condition

**Returns:** CAN device object

### dev.send(data, can_id)

Send a CAN message.

**Parameters:**
- `data`: List of bytes (0-8 bytes)
- `can_id`: CAN message identifier

**Returns:** None

### dev.recv()

Receive a CAN message (blocking).

**Returns:** Tuple of (can_id, extended, rtr, data)
- `can_id`: CAN message identifier
- `extended`: True if extended frame format
- `rtr`: True if RTR (Remote Transmission Request) frame
- `data`: Bytes object with message payload

### dev.any()

Check if any messages are available.

**Returns:** True if messages available, False otherwise

## Technical Notes

### ESP32 Variant Compatibility

This module uses a universal timing configuration approach to support all ESP32 variants. The original ESP32 has a different `twai_timing_config_t` structure (5 fields) compared to newer variants (7 fields with additional clock source parameters).

- **ESP32**: Manual timing calculations based on 40MHz APB clock
- **ESP32-C3/S2/S3**: Uses ESP-IDF timing macros with conditional compilation

### Timing Formula

```
Bitrate = APB_CLK_FREQ / (BRP * (1 + tseg_1 + tseg_2))
```

Where:
- APB_CLK_FREQ = 40MHz for ESP32
- BRP = Baud Rate Prescaler
- tseg_1/tseg_2 = Time segments

## Requirements

- ESP-IDF v5.0 or later
- MicroPython v1.19 or later
- ESP32 family device

