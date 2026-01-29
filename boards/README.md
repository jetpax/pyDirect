# pyDirect Board Definitions

This directory contains board definitions for building pyDirect firmware across different ESP32 chip families and memory configurations.

## Supported Chips

- **ESP32-S3** - Primary target (Xtensa LX7, USB-OTG, WiFi, BLE)
- **ESP32-P4** - High-performance target (Ethernet via C6 coprocessor)

> Note: Vanilla ESP32 (Xtensa LX6) is not supported due to atomic operation compatibility issues with WebRTC libraries.

## Architecture

### Directory Structure

```
boards/
├── ESP32_S3/           # ESP32-S3 8MB flash
├── ESP32_S3_16MB/      # ESP32-S3 16MB flash
├── ESP32_P4/           # ESP32-P4 (high-perf, C6 WiFi)
└── manifests/          # Runtime board manifests
    ├── generic_esp32s3.json    # Generic S3 dev boards
    ├── generic_esp32p4.json    # Generic P4 dev boards
    └── retrovms_mini.json      # RetroVMS Mini product
```

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
    },
    ...
  ]
}
```

**To add a new product:**
1. Create `boards/manifests/{product_name}.json` with pin assignments and capabilities
2. Add entry to `.github/build-matrix.json`

### Build-time vs Runtime Configuration

| Concern | What varies | Source |
|---------|-------------|--------|
| **Build-time** | Flash size, PSRAM, chip SDK config | `boards/<CHIP>/sdkconfig.board` |
| **Runtime** | Pin assignments, capabilities, devices | `manifests/*.json` → `/settings/board.json` on device |

### Building Locally

```bash
# Build for ESP32-S3 with 8MB flash
BOARD=ESP32_S3 MANIFEST=generic_esp32s3 ./build.sh

# Build for ESP32-S3 with 16MB flash
BOARD=ESP32_S3_16MB MANIFEST=generic_esp32s3 ./build.sh

# Build for specific product with custom manifest
BOARD=ESP32_S3_16MB MANIFEST=retrovms_mini ./build.sh

# Build for ESP32-P4
BOARD=ESP32_P4 MANIFEST=generic_esp32p4 ./build.sh
```

### Adding a New Product

1. **Create the manifest** in `boards/manifests/{product}.json`:
```json
{
  "identity": {
    "id": "my_product",
    "name": "My Product",
    "chip": "ESP32-S3",
    "description": "..."
  },
  "capabilities": { ... },
  "resources": {
    "pins": { ... },
    "can": { ... },
    "spi": { ... }
  },
  "devices": { ... }
}
```

2. **Add to build matrix** in `.github/build-matrix.json`:
```json
{
  "board": "ESP32_S3_16MB",
  "target": "esp32s3",
  "manifest": "my_product",
  "artifact_name": "My_Product",
  "description": "My awesome product"
}
```

3. **Push to master** - GHA will build and deploy firmware automatically

### Adding a New Chip Family

1. Create `boards/<CHIP>/` with:
   - `mpconfigboard.h` - MicroPython board config
   - `mpconfigboard.cmake` - CMake includes
   - `sdkconfig.board` - Chip-specific Kconfig
   - `partitions-*.csv` - Partition tables

2. Create `boards/manifests/generic_<chip>.json`

3. Add entry to `.github/build-matrix.json`

### Web Flasher

The web flasher at `https://jetpax.github.io/pyDirect/` dynamically discovers available firmware from GitHub Releases and populates a dropdown based on detected chip family.
