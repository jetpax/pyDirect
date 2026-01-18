# Custom Board Definitions

This directory contains custom MicroPython board definitions for pyDirect with integrated WiFi onboarding.

## Board Structure

Each board directory contains both **build configuration** and **runtime device scripts**:

```
boards/BOARD_NAME/
├── manifest.json              # Board metadata (hardware, network config)
├── mpconfigboard.cmake        # CMake configuration
├── mpconfigboard.h            # C preprocessor definitions
├── sdkconfig.board            # ESP-IDF Kconfig options
├── partitions-*.csv           # Flash partition table
├── manifest.py                # Frozen modules (optional)
├── device-scripts/            # Runtime Python scripts
│   ├── boot.py                # Boot configuration
│   ├── main.py                # Main orchestrator with WiFi onboarding
│   └── lib/
│       ├── board_config.py    # Hardware definitions (auto-generated)
│       ├── wifi_manager.py    # WiFi credential storage (NVS)
│       └── wifi_onboarding.py # Captive portal implementation
└── README.md                  # Board documentation
```

## Available Boards

### SCRIPTO_P4
ESP32-P4 development board with Ethernet, CAN, and advanced features.
- 16MB Flash, Ethernet (IP101), CAN, SD card, Audio
- See [SCRIPTO_P4/README.md](SCRIPTO_P4/README.md)

### ESP32_S3_N8R2
Generic ESP32-S3 with 8MB flash, 2MB PSRAM, and RGB LED.
- 8MB Flash, 2MB PSRAM (octal), RGB LED on GPIO48
- See [ESP32_S3_N8R2/README.md](ESP32_S3_N8R2/README.md)

## Quick Start

### 1. Build Firmware

```bash
./build.sh BOARD_NAME all --flash
```

### 2. Upload Device Scripts with WiFi Configuration

```bash
./upload-device-scripts.sh /dev/ttyUSB0 BOARD_NAME

# Interactive WiFi configuration:
Configure WiFi now? (y/n): y
# Select network and enter password
```

### 3. Access Device

Device is available at: **`http://pyDirect-XXXX.local`** (where XXXX = last 4 chars of MAC)

## WiFi Onboarding

All boards include captive portal WiFi onboarding:

**Option 1: Pre-configure during upload** (recommended)
- Upload script scans networks and prompts for credentials
- Saves to NVS, device connects automatically on boot

**Option 2: Captive portal** (fallback)
- Device starts AP: `pyDirect-XXXX`
- Connect to AP, visit `http://192.168.4.1/setup`
- Configure WiFi via web interface

## Creating a New Board

### 1. Copy Existing Board

```bash
cp -r boards/ESP32_S3_N8R2 boards/MY_BOARD
```

### 2. Update Configuration

**manifest.json:**
- Update identity (id, name, chip)
- Configure network settings (hostname pattern, mDNS)
- Define hardware resources (pins, devices)

**mpconfigboard.cmake:**
- Set `IDF_TARGET` (esp32, esp32s3, esp32c3, etc.)
- Configure sdkconfig defaults

**mpconfigboard.h:**
- Set board name and MCU name
- Define hardware pins (I2C, SPI, etc.)

**sdkconfig.board:**
- Flash and PSRAM configuration
- Partition table path
- Logging levels

**device-scripts/lib/board_config.py:**
- Update from manifest.json (or regenerate)
- Verify pin definitions match hardware

### 3. Build and Test

```bash
./build.sh MY_BOARD all --flash
./upload-device-scripts.sh /dev/ttyUSB0 MY_BOARD
```

## Key Configuration Files

### manifest.json (NEW)

Board metadata with hardware definitions:
- **identity**: Board ID, name, vendor, chip
- **network**: Hostname pattern, mDNS, AP settings
- **capabilities**: WiFi, Ethernet, CAN, etc.
- **resources**: Pin assignments, I2C, SPI, UART
- **devices**: Ethernet PHY, status LED, peripherals
- **memory**: Flash and PSRAM configuration

### mpconfigboard.cmake

CMake configuration:
- `IDF_TARGET` - ESP32 variant
- `SDKCONFIG_DEFAULTS` - List of sdkconfig files
- `MICROPY_FROZEN_MANIFEST` - Frozen modules

### mpconfigboard.h

C preprocessor definitions:
- `MICROPY_HW_BOARD_NAME` - Board name
- `MICROPY_HW_MCU_NAME` - MCU name
- Hardware pin definitions

### sdkconfig.board

ESP-IDF Kconfig options:
- Flash configuration (size, mode, frequency)
- PSRAM configuration
- Partition table path
- Logging levels
- USB configuration

### partitions-*.csv

Flash partition table:
- NVS storage (WiFi credentials)
- PHY init data
- Application partition
- Filesystem (VFS) partition

## Device Scripts

### boot.py

Minimal boot configuration:
- Disable debug output
- Disable WiFi (main.py handles it)
- Optional CPU frequency setting

### main.py

Main orchestrator with WiFi onboarding:
- Check for WiFi credentials in NVS
- Start captive portal if no credentials
- Connect to WiFi if credentials exist
- Start HTTP server + WebREPL
- Start mDNS responder

### lib/board_config.py

Board-specific configuration:
- Hardware pin definitions
- Hostname/SSID generation
- mDNS service definitions
- Board capabilities

### lib/wifi_manager.py

WiFi credential management:
- Save/load credentials from NVS
- WiFi network scanning
- Connection management
- Credential reset

### lib/wifi_onboarding.py

Captive portal implementation:
- AP mode with DNS server
- WiFi network scanner
- Setup web interface
- Success page with mDNS URL

## Using MicroPython Built-in Boards

To use a board from MicroPython's built-in collection:

```bash
./build.sh ESP32_GENERIC_S3 all
```

The build script falls back to MicroPython's boards if no matching directory exists in `pyDirect/boards/`.

**Note:** Built-in boards won't have WiFi onboarding device scripts. Use `device-scripts/` (minimal) instead.

## See Also

- [Build Guide](../docs/BUILD_GUIDE.md)
- [Board-specific READMEs](.)
- [WiFi Onboarding Walkthrough](../.gemini/antigravity/brain/55c489aa-aeaa-4ff3-87dc-bbde5c5d0c2d/walkthrough.md)
