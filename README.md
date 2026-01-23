# pyDirect: Fast path acceleration for Modules for Embedded Python 

[![Build Firmware](https://github.com/jetpax/pyDirect/actions/workflows/build-firmware.yml/badge.svg)](https://github.com/jetpax/pyDirect/actions/workflows/build-firmware.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5.1-blue)](https://github.com/espressif/esp-idf)
[![MicroPython](https://img.shields.io/badge/MicroPython-v1.27+-green)](https://micropython.org/)

**pyDirect** is a suite of high-performance C components for embedded Python environments, with initial focus on MicroPython ESP32 platforms. These components provide production-ready functionality for networking, communication protocols, and device connectivity.

It is hoped that some or all of these components will be upstreamed to MicroPython eventually, but in the meantime, they are available here for use in your projects, together with a build system that allows you to build them for your specific board and MicroPython version.


## 📦 Included Components

| Component | Description |
|--------|-------------|
| ![httpserver](https://img.shields.io/badge/httpserver-HTTP%2FHTTPS%20Server-6366f1) | Complete HTTP/HTTPS server with WebSocket support |
| ![webrepl](https://img.shields.io/badge/webrepl-Remote%20REPL-8b5cf6) | WebSocket & WebRTC remote Python access |
| ![webrtc](https://img.shields.io/badge/webrtc-DataChannel-a855f7) | Browser-native P2P communication |
| ![can](https://img.shields.io/badge/can-CAN%20Bus-ef4444) | TWAI/CAN 2.0 for automotive applications |
| ![gvret](https://img.shields.io/badge/gvret-SavvyCAN-f97316) | GVRET protocol for CAN analysis |
| ![plc](https://img.shields.io/badge/plc-V2G%20Protocol-22c55e) | DIN 70121 EXI codec for EV charging |
| ![husarnet](https://img.shields.io/badge/husarnet-P2P%20VPN-0ea5e9) | Zero-config global device connectivity |
| ![usbmodem](https://img.shields.io/badge/usbmodem-LTE%2F4G%2F5G-14b8a6) | USB Host cellular modem support |


## 🚀 Quick Start

### One-Click Web Installer (Easiest!)

Flash pyDirect directly from your browser - no tools required:

<p align="center">
  <a href="https://jetpax.github.io/pyDirect/">
    <img src="https://img.shields.io/badge/⚡_Install_pyDirect-Click_Here-6366f1?style=for-the-badge&logoColor=white" alt="Install pyDirect" />
  </a>
</p>

> **Requires:** Chrome, Edge, or Opera on desktop. Automatically detects your ESP32 chip type!

### Pre-Built Firmware

Download pre-built firmware for your board from [Releases](https://github.com/jetpax/pyDirect/releases):

```bash
# Flash firmware (example for ESP32-S3)
esptool.py --chip esp32s3 --port /dev/ttyUSB0 write_flash -z 0x0 pyDirect-ESP32_S3-merged.bin
```

### Build from Source

```bash
# Clone pyDirect and MicroPython
git clone https://github.com/jetpax/pyDirect.git
git clone https://github.com/micropython/micropython.git
cd micropython && git checkout v1.27.0

# Build for your board (requires ESP-IDF v5.5.1)
cd ../pyDirect
BOARD=ESP32_S3 MANIFEST=generic_esp32s3 ./build.sh
```

See [docs/BUILD_GUIDE.md](docs/BUILD_GUIDE.md) for detailed build instructions.

## 📦 Modules

### Core Networking

#### httpserver
Complete HTTP/HTTPS server with WebSocket support, static file serving, and WebDAP debugging protocol.

**Features:**
- HTTP/HTTPS server (dual-stack)
- WebSocket server with ping/pong keep-alive
- Static file serving with gzip compression
- WebDAP (Debug Adapter Protocol) over WebSocket
- CORS support for development

**[Documentation](httpserver/README.md)** | **[Examples](httpserver/examples/)**

#### webrepl
Remote Python REPL access via WebSocket and WebRTC transports.

**Features:**
- WebSocket transport (compatible with standard WebREPL clients)
- WebRTC DataChannel transport (NAT-traversal capable)
- Binary protocol (CBOR encoding)
- File transfer support
- Password protection

**[Documentation](webrepl/README.md)** | **[Examples](webrepl/examples/)**

#### webrtc
WebRTC DataChannel implementation for browser-native peer-to-peer communication.

**Features:**
- WebRTC DataChannel (no media codecs)
- Browser-native connectivity
- NAT traversal (STUN/TURN)
- Low-latency bidirectional data
- Automatic reconnection

**[Documentation](webrtc/README.md)** | **[Examples](webrtc/examples/)**

### Communication Protocols

#### can
CAN bus (TWAI) support for automotive and industrial applications.

**Features:**
- CAN 2.0A/2.0B support
- Standard and extended frames
- Hardware filtering
- Loopback mode for testing
- Configurable bitrates (1k-1000k bps)

**[Documentation](can/README.md)** | **[Examples](can/tests/)**

#### gvret
GVRET protocol implementation for CAN bus analysis with SavvyCAN.

**Features:**
- TCP server with GVRET protocol
- SavvyCAN compatible
- Dual CAN interface support
- Bidirectional frame transmission
- Network-based CAN analysis

**[Documentation](gvret/README.md)** | **[Examples](gvret/)**

#### plc
PLC/V2G (Vehicle-to-Grid) protocol support for EV charging.

**Features:**
- DIN 70121 EXI codec
- EVSE (charging station) mode
- ISO 15118 foundation
- Efficient binary encoding

**[Documentation](plc/README.md)** | **[Examples](plc/)**

### Connectivity

#### husarnet
P2P VPN integration for global device connectivity without port forwarding.

**Features:**
- Zero-config P2P networking
- IPv6 native
- NAT traversal
- End-to-end encryption
- Global device reach

**[Documentation](husarnet/README.md)**

#### usbmodem
USB Host mode support for cellular modems (LTE/4G/5G).

**Features:**
- USB CDC-ACM modem support
- PPP protocol implementation
- AT command interface
- Dual interface (SIM7600 compatible)
- IPv4/IPv6 dual-stack

**[Documentation](usbmodem/README.md)**

## 🔧 Build System

### Using build.sh

```bash
# Build for ESP32-S3 (8MB)
BOARD=ESP32_S3 MANIFEST=generic_esp32s3 ./build.sh

# Build for ESP32-S3 (16MB)
BOARD=ESP32_S3_16MB MANIFEST=generic_esp32s3 ./build.sh

# Build for ESP32-P4
BOARD=ESP32_P4 MANIFEST=generic_esp32p4 ./build.sh

# Build for specific product (with custom board manifest)
BOARD=ESP32_S3_16MB MANIFEST=retrovms_mini ./build.sh
```

### Supported Boards

| Board | Chip | Flash | Description |
|-------|------|-------|-------------|
| `ESP32_S3` | ESP32-S3 | 8MB | Generic dev boards (N8R2) |
| `ESP32_S3_16MB` | ESP32-S3 | 16MB | Extended flash boards |
| `ESP32_P4` | ESP32-P4 | 16MB | With C6 WiFi coprocessor |

### Build Matrix

All automated builds are defined in `.github/build-matrix.json`. To add a new product:

1. Create manifest: `boards/manifests/{product}.json`
2. Add entry to `.github/build-matrix.json`
3. Push to master - GHA builds automatically

See [boards/README.md](boards/README.md) for details.

## 🏗️ Architecture

### Module Structure

Each module follows a consistent structure:

```
module_name/
├── README.md           # Module documentation
├── micropython.cmake   # CMake build configuration
├── modmodule.c         # C implementation
├── modmodule.h         # Header file (if needed)
├── docs/              # Additional documentation
└── examples/          # Usage examples
```

### Dependencies

```
httpserver ──┬─→ webrepl (WebSocket transport)
             └─→ wsserver

webrtc ──────→ webrepl (WebRTC transport)

can ──────────→ gvret

(All modules are independent unless noted)
```

## 📋 Requirements

- **MicroPython**: v1.27.0 or later
- **ESP-IDF**: v5.5.1 (for ESP32 builds)
- **CMake**: 3.16 or later

### MicroPython Patches

pyDirect requires minimal patches to MicroPython for ESP-IDF component integration. Patches are provided in `docs/patches/`:

1. `001-esp32-http-server-components.patch` - Adds HTTP server components
2. `002-esp32-managed-components.patch` - Adds managed components (littlefs, husarnet)

See [docs/patches/README.md](docs/patches/README.md) for application instructions.

## 🤖 Automated Builds

Pre-built binaries are available in [Releases](https://github.com/jetpax/pyDirect/releases).

The workflow:
- Monitors MicroPython releases daily
- Applies patches automatically
- Builds for all boards in parallel
- Publishes firmware artifacts

See [.github/workflows/build-firmware.yml](.github/workflows/build-firmware.yml) for details.

## 📚 Documentation

- **[Build Guide](docs/BUILD_GUIDE.md)** - Detailed build instructions
- **[Production Certificates](docs/PRODUCTION_CERTIFICATES.md)** - HTTPS/WSS setup
- **[Module READMEs](.)** - Individual module documentation
- **[Patches](docs/patches/README.md)** - MicroPython patch guide

## 🔐 Security

### HTTPS/WSS Support

All network modules support TLS/SSL. Certificates are automatically generated during the browser-based provisioning flow.

See [docs/PRODUCTION_CERTIFICATES.md](docs/PRODUCTION_CERTIFICATES.md) for details.

## 📄 License

pyDirect is licensed under the **MIT License** - see [LICENSE](LICENSE) for details.

### Third-Party Components

- **CAN module**: Based on [micropython-esp32-twai](https://github.com/vostraga/micropython-esp32-twai) (MIT)
- **Husarnet**: Uses [esp_husarnet](https://components.espressif.com/components/husarnet/esp_husarnet) managed component
- **WebRTC**: Uses [esp_peer](https://components.espressif.com/components/espressif/esp_peer) managed component

## 🤝 Contributing

Contributions welcome! Please:

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Submit a pull request

See individual module READMEs for module-specific contribution guidelines.

## 🙏 Acknowledgments

- Built on [MicroPython](https://github.com/micropython/micropython)
- Uses [ESP-IDF](https://github.com/espressif/esp-idf)

## 📞 Support

- **Issues**: [GitHub Issues](https://github.com/jetpax/pyDirect/issues)
- **Discussions**: [GitHub Discussions](https://github.com/jetpax/pyDirect/discussions)
- **Documentation**: [docs/](docs/)
