# pyDirect: Fast path acceleration for Modules for Embedded Python 

**pyDirect** is a suite of high-performance C modules for embedded Python environments, with initial focus on MicroPython ESP32 platforms. These modules provide production-ready functionality for networking, communication protocols, and device connectivity.

## 🎯 Project Goals

- **Platform-Agnostic**: "Embedded Python" focused, not tied to MicroPython specifics
- **Production-Ready**: Battle-tested modules for real-world applications  
- **Modular Design**: Include only what you need
- **High Performance**: Critical paths implemented in C
- **Open Source**: MIT licensed, ready for commercial use

## 🚀 Quick Start

### Pre-Built Firmware (Easiest)

Download pre-built firmware for your board from [Releases](https://github.com/jetpax/pyDirect/releases):

```bash
# Flash firmware (example for SCRIPTO_P4)
esptool.py --chip esp32p4 --port /dev/ttyUSB0 write_flash -z 0x0 pyDirect-SCRIPTO_P4-v1.27.0.bin
```

### Build from Source

```bash
# Clone pyDirect
git clone https://github.com/jetpax/pyDirect.git
cd pyDirect

# Build for your board (requires ESP-IDF v5.5.1)
./build.sh SCRIPTO_P4 all --flash
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

### Using build.sh (Recommended)

```bash
# Build for specific board with all modules
./build.sh SCRIPTO_P4 all

# Build with specific modules only
./build.sh SCRIPTO_S3 httpserver can webrtc

# Exclude specific modules
./build.sh SCRIPTO_P4 all -usbmodem

# Build and flash
./build.sh SCRIPTO_P4 all --flash

# Clean build
./build.sh SCRIPTO_P4 all --clean
```

### Supported Boards

- **SCRIPTO_P4** - ESP32-P4 based board
- **SCRIPTO_S3** - ESP32-S3 based board
- **RETROVMS_MINI** - Compact ESP32-S3 variant

Custom boards can be added in `boards/` directory.

### Module Configuration

Modules are enabled via CMake options:

```cmake
-DMODULE_PYDIRECT_HTTPSERVER=ON   # HTTP/HTTPS server
-DMODULE_PYDIRECT_WEBREPL=ON      # WebREPL (requires httpserver or webrtc)
-DMODULE_PYDIRECT_WEBRTC=ON       # WebRTC DataChannel
-DMODULE_PYDIRECT_CAN=ON          # CAN bus
-DMODULE_PYDIRECT_GVRET=ON        # GVRET (requires can)
-DMODULE_PYDIRECT_HUSARNET=ON     # Husarnet P2P VPN
-DMODULE_PYDIRECT_USBMODEM=ON     # USB modem
-DMODULE_PYDIRECT_PLC=ON          # PLC/V2G protocol
```

## 📖 Usage Examples

### HTTP Server with WebSocket

```python
import httpserver
import wsserver

# Start HTTP server on port 80
httpserver.start(80)

# Register WebSocket handler
def on_message(client_id, message):
    print(f"Received: {message}")
    wsserver.send(client_id, f"Echo: {message}")

wsserver.register_handler("/ws", on_message)

# Main loop
while True:
    httpserver.process_queue()
```

### CAN Bus Communication

```python
import CAN

# Initialize CAN bus
can = CAN(0, mode=CAN.NORMAL, bitrate=500000, tx=21, rx=22)

# Send frame
can.send(0x123, b'\x01\x02\x03\x04')

# Receive frame
msg_id, data = can.recv(timeout=1000)
print(f"ID: 0x{msg_id:03x}, Data: {data.hex()}")
```

### WebRTC DataChannel

```python
import webrtc

# WebRTC server starts automatically with httpserver
# Connect from browser at: https://device-ip/

# Register data handler
def on_data(peer_id, data):
    print(f"Received from {peer_id}: {data}")
    webrtc.send(peer_id, b"Response")

webrtc.on_data(on_data)
```

See module-specific READMEs for comprehensive API documentation.

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

All network modules support TLS/SSL:

```python
# Enable HTTPS in main.py
HTTPS_ENABLED = True
HTTPS_CERT_FILE = '/certs/servercert.pem'
HTTPS_KEY_FILE = '/certs/prvtkey.pem'
```

Generate certificates with the included script:

```bash
./generate-device-cert.sh 2b88 192.168.1.32
```

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
- Inspired by the need for production-ready embedded Python modules

## 📞 Support

- **Issues**: [GitHub Issues](https://github.com/jetpax/pyDirect/issues)
- **Discussions**: [GitHub Discussions](https://github.com/jetpax/pyDirect/discussions)
- **Documentation**: [docs/](docs/)
