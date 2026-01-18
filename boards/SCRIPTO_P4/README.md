# Scripto P4 Board

ESP32-P4 development board with Ethernet, CAN, and NeoPixel status LED.

## Specifications

- **Chip:** ESP32-P4
- **Flash:** 16MB
- **Features:** Ethernet (IP101), CAN, SD card, Audio (ES8311), NeoPixel LED

## Quick Start

### 1. Build Firmware

```bash
cd /path/to/pyDirect
./build.sh SCRIPTO_P4 all --flash
```

### 2. Upload Device Scripts

```bash
./upload-device-scripts.sh /dev/ttyUSB0 SCRIPTO_P4
```

### 3. WiFi Onboarding

On first boot, device starts in captive portal mode:

1. Connect to WiFi network: **`pyDirect-XXXX`** (where XXXX = last 4 chars of MAC)
2. Visit: `http://192.168.4.1/setup`
3. Select your WiFi network and enter password
4. Device connects to WiFi and is available at: **`http://pyDirect-XXXX.local`**

## Hardware Configuration

See `manifest.json` for complete hardware definitions.

### Pin Assignments

- **Status LED:** GPIO 1 (NeoPixel, RGB order)
- **Boot Button:** GPIO 35
- **CAN:** TX=GPIO 20, RX=GPIO 21
- **Ethernet:** IP101 PHY via RMII
- **SD Card:** SDMMC interface

### Network

- **Hostname:** `pyDirect-XXXX.local` (mDNS)
- **AP SSID:** `pyDirect-XXXX` (onboarding mode)
- **Default Services:** HTTP (port 80), WebREPL

## Device Scripts

Located in `device-scripts/`:

- **`boot.py`** - Boot configuration
- **`main.py`** - Main orchestrator with WiFi onboarding
- **`lib/board_config.py`** - Board configuration (auto-generated from manifest.json)
- **`lib/wifi_manager.py`** - WiFi credential storage and connection management
- **`lib/wifi_onboarding.py`** - Captive portal implementation

## WiFi Management

### Reset WiFi Credentials

Connect via WebREPL and run:

```python
from lib.wifi_manager import WiFiManager
wifi_mgr = WiFiManager()
wifi_mgr.clear_credentials()
import machine
machine.reset()
```

Device will reboot into captive portal mode.

### Manual WiFi Configuration

```python
from lib.wifi_manager import WiFiManager
wifi_mgr = WiFiManager()
wifi_mgr.save_credentials("YOUR_SSID", "YOUR_PASSWORD")
import machine
machine.reset()
```

## Scripto Studio Integration

For full Scripto Studio IDE features:

```bash
./install-scripto-studio.sh /dev/ttyUSB0
```

This replaces the minimal device scripts with the full Scripto Studio orchestrator.

## Build Configuration

- **`mpconfigboard.cmake`** - CMake board configuration
- **`mpconfigboard.h`** - C preprocessor definitions
- **`sdkconfig.board`** - ESP-IDF configuration
- **`partitions-16mb.csv`** - Partition table (16MB flash)

## Troubleshooting

**Can't connect to captive portal:**
- Ensure device is powered on and booted
- Look for WiFi network `pyDirect-XXXX`
- Try visiting `http://192.168.4.1/setup` directly

**mDNS not working:**
- Some networks block mDNS traffic
- Use IP address instead (shown in serial console)
- On Windows, install Bonjour service

**WiFi won't connect:**
- Check SSID and password are correct
- Ensure 2.4GHz WiFi (ESP32 doesn't support 5GHz)
- Check router allows new devices

## See Also

- [pyDirect README](../../README.md)
- [Build Guide](../../docs/BUILD_GUIDE.md)
- [Board Manifest](manifest.json)
