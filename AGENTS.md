# pyDirect Agent Instructions

> ⚠️ **CRITICAL**: If you cannot access a file (e.g., PDF, binary) or cannot understand something the user has referenced, **STOP and ask for clarification immediately**. Do NOT proceed with best-effort guesses or assumptions. This saves time and prevents wasted effort on incorrect interpretations.

**pyDirect** is a suite of C-based accelerator modules for MicroPython on ESP32. These modules implement high-performance functionality directly in C, achieving near-native performance for networking, communication, and I/O operations.

## Project Overview

pyDirect provides custom MicroPython modules for:
- **httpserver** - HTTP/HTTPS server with WebSocket support
- **webrepl** - WebREPL over WebSocket and WebRTC
- **webrtc** - WebRTC DataChannel for browser P2P
- **twai** - CAN/TWAI bus support
- **gvret** - GVRET protocol for SavvyCAN
- **husarnet** - P2P VPN networking
- **usbmodem** - USB modem (SIM7600) support
- **plc** - PLC/V2G protocol (experimental)

## WebREPL Binary Protocol (WBP) Architecture

> **Single Protocol, Multiple Transports** - WBP is one unified protocol with two transport options:
> - **WebRTC** (primary) - P2P via browser DataChannel
> - **WebSocket** (fallback) - Direct wss:// connection
>
> Both transports share the **same message handler** in `webrepl/modwebrepl_binary.c`. Protocol features (authentication, completion, execution, file transfer) are transport-agnostic. Never duplicate protocol logic per transport.

## Repository Structure

```
pyDirect/
├── httpserver/       # HTTP/HTTPS/WebSocket server
├── webrepl/          # WebREPL implementation
├── webrtc/           # WebRTC DataChannel
├── twai/             # CAN bus support
├── gvret/            # GVRET protocol
├── husarnet/         # P2P VPN
├── usbmodem/         # USB modem
├── plc/              # PLC/V2G
├── boards/           # Board definitions
│   ├── ESP32_S3/     # ESP32-S3 8MB
│   ├── ESP32_S3_16MB/# ESP32-S3 16MB
│   ├── ESP32_P4/     # ESP32-P4
│   └── manifests/    # Runtime board configs
├── device-scripts/   # Python scripts baked into VFS
├── docs/             # Documentation and web flasher
├── tools/            # Build and CI tools
└── .github/          # Workflows and build matrix
```

## Build System

### Build Matrix

All builds are defined in **`.github/build-matrix.json`**:

```json
{
  "builds": [
    {
      "board": "ESP32_S3",
      "target": "esp32s3",
      "manifest": "generic_esp32s3",
      "artifact_name": "ESP32_S3",
      "description": "Generic ESP32-S3 (8MB Flash)"
    }
  ]
}
```

### Building Locally

```bash
# Clone dependencies
git clone https://github.com/micropython/micropython.git
cd micropython && git checkout v1.27.0

# Build firmware
BOARD=ESP32_S3 MANIFEST=generic_esp32s3 ./build.sh
```

### Adding a New Product

1. Create manifest: `boards/manifests/{product}.json`
2. Add to build matrix: `.github/build-matrix.json`
3. Push to master - GHA builds automatically

## Coding Standards

- **C99** with two-space indentation, no tabs
- **snake_case** for variables/functions
- **UPPER_CASE** for macros and constants
- Defer ISR work to task context
- No dynamic allocation where possible
- Match existing patterns in files you modify

## Board Manifests

Runtime configuration is defined in JSON manifests:

```json
{
  "identity": {
    "id": "my_product",
    "name": "My Product",
    "chip": "ESP32-S3"
  },
  "capabilities": { "can": true, "wifi": true },
  "resources": {
    "pins": { "status_led": 48 },
    "can": { "can0": { "tx": 4, "rx": 5 } }
  }
}
```

Available at runtime: `/lib/board.json`

## Web Flasher

The web flasher at `https://jetpax.github.io/pyDirect/`:
- Detects chip family via Web Serial
- Dynamically discovers firmware from GitHub Releases
- Downloads from `docs/firmware/` (same origin)

## Key Files

| File | Purpose |
|------|---------|
| `build.sh` | Main build script |
| `.github/build-matrix.json` | Build configurations |
| `tools/ci_update_dependencies.py` | Patches MicroPython for GHA |
| `docs/index.html` | Web flasher |
| `boards/manifests/*.json` | Runtime board configs |

## Supported Chips

- **ESP32-S3** - Primary target (Xtensa LX7, USB-OTG, WiFi, BLE)
- **ESP32-P4** - High-performance (via C6 WiFi coprocessor)

> Note: Vanilla ESP32 (Xtensa LX6) is not supported due to atomic operation issues with WebRTC libraries.

## ScriptOs Format

ScriptOs are MicroPython scripts run on-device via Scripto Studio. They use a special config block pattern:

```python
# === START_CONFIG_PARAMETERS ===

dict(
    timeout = 5,  # Seconds before showing interrupt button (0 = never)
    
    info = dict(
        name        = 'My ScriptO',
        version     = [1, 0, 0],
        category    = 'Test',
        description = 'What this script does',
        author      = 'YourName',
    ),
    
    args = dict(
        my_param = dict(
            label = 'Select a value:',
            type  = int,      # str, int, float, bool, list (GPIO), dict (dropdown)
            value = 42        # Optional default
        ),
    )
)

# === END_CONFIG_PARAMETERS ===

# Your code runs at MODULE LEVEL - no main() needed!
print(f"User selected: {args.my_param}")
```

### Key Rules

1. **Module-level execution** - Code runs immediately, no `def main()` pattern
2. **Access config via `args`** - The config block becomes `class args` at runtime
3. **Use `type=list`** for GPIO pin selection
4. **Use `type=dict` with `items=dict(...)` for dropdown menus**

## Hardware Access

**Always use `lib.sys.board`** to access hardware. Never hardcode pins!

```python
from lib.sys import board, settings

# Check capabilities
if not board.has("can"):
    raise RuntimeError("Board does not have CAN capability")

# Get bus configuration
can_bus = board.can("twai")  # or "can0" for multi-CAN boards
tx_pin = can_bus.tx
rx_pin = can_bus.rx

# Get I2C bus
i2c_bus = board.i2c("sensors")  # Returns scl, sda

# Get device info
display = board.device("display")
driver = display.driver

# Get individual pins
led = board.pin("status_led")

# Check resources
if board.has("i2c.sensors"):
    # Board has sensors I2C bus
```

### Board API Reference

| Method | Returns | Example |
|--------|---------|---------|
| `board.id.name` | str | `"Waveshare ESP32-S3 Touch LCD 1.46"` |
| `board.id.chip` | str | `"ESP32-S3"` |
| `board.has(cap)` | bool | `board.has("display")` |
| `board.pin(name)` | int | `board.pin("boot")` → `0` |
| `board.i2c(name)` | I2C | `board.i2c("sensors").scl` |
| `board.spi(name)` | SPI | `board.spi("sdcard").mosi` |
| `board.can(name)` | CAN | `board.can("twai").tx` |
| `board.uart(name)` | UART | `board.uart("primary").tx` |
| `board.device(name)` | Device | `board.device("imu").type` |