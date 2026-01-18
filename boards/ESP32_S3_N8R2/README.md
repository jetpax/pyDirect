# ESP32-S3-N8R2 Board

Generic ESP32-S3 board with 8MB flash, 2MB PSRAM (octal), and RGB LED on GPIO48.

## Specifications

- **Chip:** ESP32-S3
- **Flash:** 8MB (QIO mode, 80MHz)
- **PSRAM:** 2MB (Octal mode)
- **Features:** WiFi, Bluetooth, USB OTG, RGB LED

## Quick Start

### 1. Build Firmware

```bash
cd /path/to/pyDirect
./build.sh ESP32_S3_N8R2 all --flash
```

### 2. Upload Device Scripts

```bash
./upload-device-scripts.sh /dev/ttyUSB0 ESP32_S3_N8R2

# Interactive WiFi configuration:
Configure WiFi now? (y/n): y
```

### 3. Access Device

After WiFi configuration, device is available at: **`http://pyDirect-XXXX.local`**

## Hardware Configuration

See `manifest.json` for complete hardware definitions.

### Pin Assignments

- **Status LED:** GPIO 48 (RGB LED, WS2812)
- **Boot Button:** GPIO 0
- **USB:** GPIO 19 (D-), GPIO 20 (D+)
- **I2C:** SDA=GPIO 8, SCL=GPIO 9
- **SPI:** MOSI=GPIO 11, MISO=GPIO 13, SCK=GPIO 12, CS=GPIO 10

### Network

- **Hostname:** `pyDirect-XXXX.local` (mDNS)
- **AP SSID:** `pyDirect-XXXX` (onboarding mode)
- **Default Services:** HTTP (port 80), WebREPL

## Device Scripts

Located in `device-scripts/`:

- **`boot.py`** - Boot configuration
- **`main.py`** - Main orchestrator with WiFi onboarding
- **`lib/board_config.py`** - Board configuration
- **`lib/wifi_manager.py`** - WiFi credential storage (NVS)
- **`lib/wifi_onboarding.py`** - Captive portal implementation

## WiFi Onboarding

### First Boot (No WiFi)

1. Device starts AP: `pyDirect-XXXX`
2. Connect to AP
3. Visit: `http://192.168.4.1/setup`
4. Select WiFi network + enter password
5. Device connects and is available at: `http://pyDirect-XXXX.local`

### Pre-Configure During Upload

```bash
./upload-device-scripts.sh /dev/ttyUSB0 ESP32_S3_N8R2

Configure WiFi now? (y/n): y
# Select network and enter password
# Captive portal skipped on first boot
```

### Reset WiFi Credentials

```python
from lib.wifi_manager import WiFiManager
wifi_mgr = WiFiManager()
wifi_mgr.clear_credentials()
import machine
machine.reset()
```

## Build Configuration

- **`mpconfigboard.cmake`** - CMake board configuration
- **`mpconfigboard.h`** - C preprocessor definitions
- **`sdkconfig.board`** - ESP-IDF configuration
- **`partitions-8mb.csv`** - Partition table (8MB flash)

## Partition Layout

| Partition | Type | Offset | Size | Description |
|-----------|------|--------|------|-------------|
| nvs | data | 0x9000 | 24KB | Non-volatile storage |
| phy_init | data | 0xf000 | 4KB | PHY init data |
| factory | app | 0x10000 | 3MB | MicroPython firmware |
| vfs | data | 0x310000 | ~5MB | Virtual filesystem |

## Troubleshooting

**RGB LED not working:**
- Verify LED is on GPIO48
- Check if LED is WS2812 compatible
- Try different pixel order (RGB, GRB, etc.)

**USB not detected:**
- Ensure USB cable supports data (not charge-only)
- Try different USB port
- Check GPIO 19/20 are not used elsewhere

**WiFi won't connect:**
- ESP32-S3 only supports 2.4GHz WiFi
- Check SSID and password are correct
- Ensure router allows new devices

## See Also

- [pyDirect README](../../README.md)
- [Build Guide](../../docs/BUILD_GUIDE.md)
- [Board Manifest](manifest.json)
