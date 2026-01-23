# pyDirect Agent Instructions

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