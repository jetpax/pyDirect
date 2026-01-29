# Board API Documentation

The `board` module provides read-only access to board hardware definitions.

## Overview

The board module is a singleton that automatically loads board configuration from `/settings/board.json`. It provides a clean, attribute-based API for accessing hardware information.

```python
from lib import board

# Identity
print(board.id.name)  # "Scripto P4+C6"
print(board.id.chip)  # "ESP32-P4"

# Capabilities
if board.has("ethernet"):
    print("Board has Ethernet")

# Pins
led_pin = board.pin("status_led")

# Buses
can_bus = board.can("twai")
print(f"CAN TX: GPIO{can_bus.tx}")

# Devices
eth = board.device("ethernet")
print(f"Ethernet PHY: {eth.phy}")
```

## Identity

Access board identification information via `board.id`:

```python
board.id.id          # "scripto_p4_c6"
board.id.name        # "Scripto P4+C6"
board.id.vendor      # "scripto"
board.id.chip        # "ESP32-P4"
board.id.revision    # "1.4"
```

## Capabilities

Check if board has specific hardware features:

```python
board.has("ethernet")     # True/False
board.has("can")          # True/False
board.has("sdcard")       # True/False
board.has("uart.primary") # True/False (nested check)
```

Returns `True` if the capability exists, `False` otherwise.

## Pins

Get GPIO pin numbers for named pins:

```python
led_pin = board.pin("status_led")  # 1
boot_pin = board.pin("boot")       # 35
```

Raises `KeyError` if pin not found.

## Buses

### UART

```python
uart = board.uart("primary")
tx = uart.tx    # 37
rx = uart.rx    # 38
```

### I2C

```python
i2c = board.i2c("i2c0")
sda = i2c.sda   # 7
scl = i2c.scl   # 8
```

### CAN (TWAI)

```python
can_bus = board.can("twai")
tx = can_bus.tx         # 20
rx = can_bus.rx         # 21
standby = getattr(can_bus, 'standby', None)  # Optional
```

### SPI

```python
spi = board.spi("spi1")
miso = spi.miso  # 13
mosi = spi.mosi  # 11
sclk = spi.sclk  # 12
```

### SDMMC

```python
sd = board.sdmmc("sdcard")
cmd = sd.cmd   # 44
clk = sd.clk   # 43
d0 = sd.d0     # 39
d1 = sd.d1     # 40 (optional for 4-bit mode)
d2 = sd.d2     # 41 (optional for 4-bit mode)
d3 = sd.d3     # 42 (optional for 4-bit mode)
```

All bus methods raise `KeyError` if the bus is not found.

## Devices

Get device-specific configuration:

```python
eth = board.device("ethernet")
phy = eth.phy            # "IP101"
phy_addr = eth.phy_addr  # 1
pins = eth.pins          # {"mdc": 31, "mdio": 52, "reset": 51}

led = board.device("status_led")
led_type = led.type           # "neopixel"
pixel_order = led.pixel_order # "RGB"

codec = board.device("audio_codec")
codec_type = codec.type       # "ES8311"
i2c_bus = codec.i2c_bus       # "i2c0"
i2c_addr = codec.i2c_address  # "0x18"
```

Raises `KeyError` if device not found.

## Error Handling

All methods raise `KeyError` (or `AttributeError` for attributes) when requested hardware is not found:

```python
try:
    can_bus = board.can("twai")
except KeyError:
    print("Board does not have CAN bus")

# Better: Use has() first
if board.has("can"):
    can_bus = board.can("twai")
```

## Usage Patterns

### Check before use

```python
if board.has("ethernet"):
    eth = board.device("ethernet")
    init_ethernet(phy=eth.phy, addr=eth.phy_addr)
```

### Safe attribute access

```python
# Use getattr() for optional attributes
can_bus = board.can("twai")
standby_pin = getattr(can_bus, 'standby', None)
if standby_pin:
    setup_standby(standby_pin)
```

### Initialization helper

```python
def init_can():
    """Initialize CAN bus using board config."""
    if not board.has("can"):
        return None
    
    try:
        can_bus = board.can("twai")
        return CAN.init(tx=can_bus.tx, rx=can_bus.rx)
    except Exception as e:
        print(f"CAN init failed: {e}")
        return None
```

## Configuration File Location

The board module loads from `/settings/board.json`. This file is typically uploaded by ScriptoStudio when a board is detected or manually uploaded.

If the file is not found, the module creates a generic fallback board:

```python
{
  "identity": {
    "id": "generic",
    "name": "Generic Board",
    "vendor": "generic",
    "chip": "ESP32",
    "revision": "1.0"
  },
  "capabilities": {},
  "resources": {"pins": {}},
  "devices": {}
}
```

## Implementation Details

- **Singleton pattern**: First import loads and caches configuration
- **Lazy loading**: Configuration loads on first attribute access
- **Immutable**: Board configuration is read-only
- **View objects**: Returns lightweight view objects for nested data

## See Also

- `lib/settings.py` - Runtime configuration (user-modifiable)
- `Boards/README.md` - Board JSON format specification
