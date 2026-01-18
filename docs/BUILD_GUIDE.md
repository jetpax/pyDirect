# Building pyDirect Firmware

This guide covers building MicroPython firmware with pyDirect modules from source.

## Prerequisites

### Required Software

1. **ESP-IDF v5.5.1**
   ```bash
   # Install ESP-IDF
   mkdir -p ~/esp
   cd ~/esp
   git clone --recursive https://github.com/espressif/esp-idf.git
   cd esp-idf
   git checkout v5.5.1
   git submodule update --init --recursive
   ./install.sh esp32,esp32s3,esp32p4
   ```

2. **MicroPython v1.27.0+**
   ```bash
   git clone https://github.com/micropython/micropython.git
   cd micropython
   git checkout v1.27.0  # or latest
   git submodule update --init --recursive
   make -C mpy-cross
   ```

3. **pyDirect**
   ```bash
   git clone https://github.com/jetpax/pyDirect.git
   ```

### Apply MicroPython Patches

pyDirect requires minimal patches to MicroPython for ESP-IDF component integration:

```bash
cd micropython
git apply /path/to/pyDirect/docs/patches/001-esp32-http-server-components.patch
git apply /path/to/pyDirect/docs/patches/002-esp32-managed-components.patch
```

If patches fail (due to upstream changes), see [docs/patches/README.md](patches/README.md) for manual application instructions.

## Quick Build

### Using build.sh (Recommended)

The easiest way to build is using the included `build.sh` script:

```bash
cd /path/to/pyDirect

# Build for SCRIPTO_P4 with all modules
./build.sh SCRIPTO_P4 all

# Build and flash
./build.sh SCRIPTO_P4 all --flash

# Build with specific modules
./build.sh SCRIPTO_S3 httpserver can webrtc

# Exclude specific modules
./build.sh SCRIPTO_P4 all -usbmodem

# Clean build
./build.sh SCRIPTO_P4 all --clean --flash
```

The script will:
1. Source ESP-IDF environment
2. Configure CMake with selected modules
3. Build firmware
4. Optionally flash to device

### Environment Variables

Configure paths in your shell (or let build.sh use defaults):

```bash
export MPY_DIR=/path/to/micropython
export ESP_IDF_PATH=/path/to/esp-idf-v5.5.1
export PYDIRECT_DIR=/path/to/pyDirect
```

## Manual Build

### Step 1: Source ESP-IDF

```bash
source ~/esp/esp-idf-v5.5.1/export.sh
```

### Step 2: Navigate to MicroPython ESP32 Port

```bash
cd /path/to/micropython/ports/esp32
```

### Step 3: Configure Build

```bash
# Configure for SCRIPTO_P4 with all modules
cmake \
  -DMICROPY_BOARD=SCRIPTO_P4 \
  -DMICROPY_BOARD_DIR=/path/to/pyDirect/boards/SCRIPTO_P4 \
  -DUSER_C_MODULES=/path/to/pyDirect/micropython.cmake \
  -DMODULE_PYDIRECT_HTTPSERVER=ON \
  -DMODULE_PYDIRECT_WEBREPL=ON \
  -DMODULE_PYDIRECT_WEBRTC=ON \
  -DMODULE_PYDIRECT_CAN=ON \
  -DMODULE_PYDIRECT_GVRET=ON \
  -DMODULE_PYDIRECT_HUSARNET=ON \
  -DMODULE_PYDIRECT_USBMODEM=ON \
  -DMODULE_PYDIRECT_PLC=ON \
  -S . -B build-SCRIPTO_P4
```

### Step 4: Build

```bash
make -C build-SCRIPTO_P4 -j$(nproc)
```

### Step 5: Flash

```bash
make -C build-SCRIPTO_P4 flash BAUD=921600
```

## Board Configuration

### Supported Boards

pyDirect includes custom board definitions in `boards/`:

- **SCRIPTO_P4** - ESP32-P4 with 16MB flash, Ethernet, WiFi
- **SCRIPTO_S3** - ESP32-S3 with 16MB flash, WiFi
- **RETROVMS_MINI** - Compact ESP32-S3 variant

### Using Custom Boards

```bash
# Build for custom board
./build.sh SCRIPTO_P4 all

# Or manually specify board directory
cmake \
  -DMICROPY_BOARD=SCRIPTO_P4 \
  -DMICROPY_BOARD_DIR=/path/to/pyDirect/boards/SCRIPTO_P4 \
  ...
```

### Using MicroPython Built-in Boards

```bash
# Build for generic ESP32-S3
cmake \
  -DMICROPY_BOARD=GENERIC_S3 \
  -DUSER_C_MODULES=/path/to/pyDirect/micropython.cmake \
  ...
```

## Module Selection

### Enable All Modules

```bash
./build.sh BOARD all
```

Or manually:
```cmake
-DMODULE_PYDIRECT_HTTPSERVER=ON
-DMODULE_PYDIRECT_WEBREPL=ON
-DMODULE_PYDIRECT_WEBRTC=ON
-DMODULE_PYDIRECT_CAN=ON
-DMODULE_PYDIRECT_GVRET=ON
-DMODULE_PYDIRECT_HUSARNET=ON
-DMODULE_PYDIRECT_USBMODEM=ON
-DMODULE_PYDIRECT_PLC=ON
```

### Enable Specific Modules

```bash
# Only HTTP server and CAN
./build.sh SCRIPTO_S3 httpserver can

# Or manually
cmake \
  -DMODULE_PYDIRECT_HTTPSERVER=ON \
  -DMODULE_PYDIRECT_CAN=ON \
  ...
```

### Module Dependencies

Some modules require others:

- **webrepl** requires **httpserver** OR **webrtc**
- **gvret** requires **can**

The build system will warn if dependencies are missing.

## Build Artifacts

After successful build, find artifacts in `build-BOARD/`:

```
build-SCRIPTO_P4/
├── micropython.bin       # Main firmware (flash at 0x0)
├── micropython.elf       # Debug symbols
├── micropython.map       # Memory map
├── bootloader/
│   └── bootloader.bin    # Bootloader (flash at 0x0)
└── partition_table/
    └── partition-table.bin  # Partition table (flash at 0x8000)
```

### Flashing Individual Components

```bash
esptool.py --chip esp32p4 --port /dev/ttyUSB0 write_flash \
  0x0 build-SCRIPTO_P4/bootloader/bootloader.bin \
  0x8000 build-SCRIPTO_P4/partition_table/partition-table.bin \
  0x10000 build-SCRIPTO_P4/micropython.bin
```

Or flash everything at once:
```bash
make -C build-SCRIPTO_P4 flash
```

## Verification

### Check Build Output

Look for module registration messages during build:

```
-- pyDirect: Enabling httpserver module
-- pyDirect: Enabling can module
-- pyDirect: Enabling webrtc module
...
```

### Test in REPL

After flashing, connect via serial:

```bash
# Using miniterm
python -m serial.tools.miniterm /dev/ttyUSB0 115200

# Or using screen
screen /dev/ttyUSB0 115200
```

Test module imports:

```python
>>> import httpserver
>>> import CAN
>>> import webrtc
>>> import webrepl_binary
>>> print("All modules loaded!")
```

## Troubleshooting

### Patches Don't Apply

If MicroPython patches fail:

1. Check MicroPython version compatibility
2. Apply patches manually (see [docs/patches/README.md](patches/README.md))
3. Or use the fallback sed commands in the GitHub Actions workflow

### Build Errors

**Missing ESP-IDF components:**
```
CMake Error: Could not find esp_http_server
```
**Solution:** Ensure patches were applied correctly.

**Module not found:**
```
ModuleNotFoundError: No module named 'httpserver'
```
**Solution:** Verify module was enabled in CMake configuration.

**Out of memory:**
```
region `iram0_0_seg' overflowed
```
**Solution:** Disable unused modules or use a board with more flash/RAM.

### Flash Errors

**Permission denied:**
```bash
sudo chmod 666 /dev/ttyUSB0
# Or add user to dialout group
sudo usermod -a -G dialout $USER
```

**Wrong chip:**
```bash
# Specify correct chip
esptool.py --chip esp32s3 ...
```

**Baud rate issues:**
```bash
# Try lower baud rate
make -C build-BOARD flash BAUD=115200
```

## Advanced Configuration

### Custom Partition Table

Edit `boards/BOARD/partitions-16mb.csv`:

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x6000,
phy_init, data, phy,     0xf000,  0x1000,
factory,  app,  factory, 0x10000, 0x300000,
vfs,      data, fat,     0x310000,0xCF0000,
```

### sdkconfig Customization

Board-specific settings in `boards/BOARD/sdkconfig.board`:

```cmake
# Enable HTTPS
CONFIG_ESP_HTTPS_SERVER_ENABLE=y

# Increase task stack
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192

# Suppress debug logs
CONFIG_LOG_MAXIMUM_LEVEL_INFO=y
```

### Adding Custom Modules

1. Create module directory: `mymodule/`
2. Add `micropython.cmake`
3. Update `micropython.cmake` to include your module
4. Add to `build.sh` module list

See existing modules for examples.

## Continuous Integration

pyDirect includes GitHub Actions workflow for automated builds:

- Monitors MicroPython releases
- Applies patches automatically
- Builds all boards in parallel
- Publishes firmware to Releases

See [.github/workflows/build-firmware.yml](../.github/workflows/build-firmware.yml).

## Next Steps

- **Configure HTTPS**: See [PRODUCTION_CERTIFICATES.md](PRODUCTION_CERTIFICATES.md)
- **Module Documentation**: Check individual module READMEs
- **Example Scripts**: Explore `examples/` directories
- **API Reference**: See module-specific documentation

## See Also

- [README.md](../README.md) - Project overview
- [patches/README.md](patches/README.md) - Patch application guide
- [PRODUCTION_CERTIFICATES.md](PRODUCTION_CERTIFICATES.md) - HTTPS setup
- Module READMEs - Individual module documentation
