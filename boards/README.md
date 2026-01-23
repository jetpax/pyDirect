# pyDirect Board Definitions

This directory contains board definitions for building pyDirect firmware across different ESP32 chip families and memory configurations.

## Supported Chips

- **ESP32-S3** - Primary target (Xtensa LX7, USB-OTG, WiFi, BLE)
- **ESP32-P4** - High-performance target (Ethernet, no WiFi)

> Note: Vanilla ESP32 (Xtensa LX6) is not supported due to atomic operation compatibility issues with WebRTC libraries.

## Architecture

### Directory Structure

```
boards/
├── ESP32_S3/           # ESP32-S3 (Xtensa LX7, USB-OTG)
├── ESP32_P4/           # ESP32-P4 (high-perf, Ethernet)
├── memory_profiles/    # Shared flash/PSRAM configurations
│   ├── esp32s3_8mb_2mb_quad.kconfig
│   ├── esp32s3_16mb_8mb_oct.kconfig
│   └── esp32s3_16mb_2mb_quad.kconfig
└── manifests/          # Runtime board manifests (synced from registry)
    ├── generic_esp32s3.json
    ├── retrovms_mini.json
    └── scripto_p4_c6.json
```

### Build-time vs Runtime Configuration

| Concern | What varies | Source |
|---------|-------------|--------|
| **Build-time** | Flash size, PSRAM, chip-specific SDK config | `boards/<CHIP>/sdkconfig.board` + memory profiles |
| **Runtime** | Pin assignments, capabilities, devices | `manifests/*.json` → copied to device `/lib/board.json` |

### Memory Profiles

Instead of creating separate board directories for each flash/PSRAM variant (N8R2, N16R8, N16R2, etc.), we use **memory profiles** that can be combined with a base chip configuration:

| Profile | Flash | PSRAM | Chips |
|---------|-------|-------|-------|
| `8mb_2mb_quad` | 8MB | 2MB Quad | ESP32-S3 (default) |
| `16mb_8mb_oct` | 16MB | 8MB Octal | ESP32-S3 |
| `16mb_2mb_quad` | 16MB | 2MB Quad | ESP32-S3 (RetroVMS) |

### Building

```bash
# Build for ESP32-S3 with 8MB flash (default profile)
BOARD=ESP32_S3 ./build.sh

# Build for ESP32-S3 with 16MB flash/8MB PSRAM
BOARD=ESP32_S3 MEMORY_PROFILE=16MB_8MB ./build.sh

# Build for specific product with manifest
BOARD=ESP32_S3 MANIFEST=retrovms_mini ./build.sh
```

### Adding a New Chip Family

1. Create `boards/<CHIP>/` with:
   - `mpconfigboard.h` - MicroPython board config
   - `mpconfigboard.cmake` - CMake includes
   - `sdkconfig.board` - Chip-specific Kconfig
   - `partitions/` - Partition tables for different flash sizes

2. Add memory profile(s) if new configurations needed

3. Add manifest(s) to `scripto-studio-registry/Boards/`

### Legacy Board Directories

The following directories are maintained for backward compatibility but are deprecated:
- `ESP32_S3_N8R2/` → Use `ESP32_S3` with default profile
- `SCRIPTO_S3/` → Use `ESP32_S3` with 16MB profile
- `RETROVMS_MINI/` → Use `ESP32_S3` with 16MB profile + manifest
- `SCRIPTO_P4/` → Use `ESP32_P4`
