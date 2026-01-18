# WebRTC Module - Build Instructions

## Prerequisites

1. **ESP-IDF 5.0+** installed and configured
2. **MicroPython ESP32 port** with pyDirect support
3. **ESP32-S3 or ESP32-P4** board (recommended for memory)

## Step 1: Enable esp-webrtc-solution Components

The WebRTC module depends on `esp_peer` from Espressif's WebRTC solution.

**Note**: These dependencies are already configured in MicroPython's `main/idf_component.yml` and will be automatically downloaded by ESP-IDF's component manager during the build.

**Why no `media_lib_sal`?** Our data-channel-only implementation doesn't need audio/video support. The `esp_peer` component includes weak implementations of `media_lib` functions (see `media_lib_weak.c`) which are sufficient for DataChannel-only usage.

### Verify Dependencies (Optional)

If you want to manually verify or pre-download dependencies:

```bash
cd micropython/ports/esp32
idf.py reconfigure
```

This will download all managed components including:
- `espressif/esp_peer` (^1.2.2)

## Step 2: Configure mbedTLS for DTLS

The WebRTC module requires DTLS support in mbedTLS. Add to `sdkconfig`:

```ini
CONFIG_MBEDTLS_SSL_PROTO_DTLS=y
CONFIG_MBEDTLS_SSL_DTLS_SRTP=y
# For ESP-IDF 6.0+:
CONFIG_MBEDTLS_X509_CREATE_C=y
```

Or via menuconfig:

```bash
idf.py menuconfig
# Navigate to: Component config → mbedTLS → TLS Protocol Support
# Enable: DTLS protocol
# Enable: DTLS-SRTP support
```

## Step 3: Increase Memory (Recommended)

WebRTC requires substantial memory. Update `sdkconfig`:

```ini
# Enable PSRAM (if available)
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y

# Increase task stack sizes
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT=4096

# Enable heap tracing (optional, for debugging)
CONFIG_HEAP_TRACING=y
```

## Step 4: Build Firmware

```bash
cd micropython/ports/esp32

# Clean build (recommended for first build)
make clean

# Build with WebRTC enabled
make \
  USER_C_MODULES=/path/to/pyDirect/micropython.cmake \
  CMAKE_ARGS="-DMODULE_PYDIRECT_WEBRTC=ON -DMODULE_PYDIRECT_HTTPSERVER=ON" \
  all

# Or using idf.py directly:
idf.py -DMODULE_PYDIRECT_WEBRTC=ON build
```

## Step 5: Flash and Test

```bash
# Flash firmware
make PORT=/dev/ttyUSB0 erase
make PORT=/dev/ttyUSB0 deploy

# Or using idf.py:
idf.py -p /dev/ttyUSB0 flash monitor
```

## Verify Installation

Connect to REPL and test:

```python
>>> import webrtc
>>> peer = webrtc.Peer()
>>> print("WebRTC module loaded successfully!")
>>> peer.close()
```

## Troubleshooting

### Build Error: "esp_peer.h not found"

**Solution**: IDF Component Manager hasn't pulled dependencies yet.

```bash
# Force component update
idf.py reconfigure
# Or manually:
cd build && rm -rf managed_components && cd ..
idf.py build
```

### Build Error: "MBEDTLS_SSL_PROTO_DTLS not defined"

**Solution**: DTLS not enabled in mbedTLS.

```bash
idf.py menuconfig
# Component config → mbedTLS → Enable DTLS
idf.py build
```

### Runtime Error: "Failed to create peer"

**Solution**: Insufficient memory.

1. Check PSRAM is enabled and detected:
   ```python
   import esp32
   print(esp32.spiram_size())  # Should show non-zero
   ```

2. Reduce buffer sizes in `modwebrtc.c`:
   ```c
   #define WEBRTC_SEND_CACHE_SIZE (50 * 1024)  // Reduce to 50KB
   #define WEBRTC_RECV_CACHE_SIZE (50 * 1024)
   ```

3. Free up memory:
   ```python
   import gc
   gc.collect()
   print(gc.mem_free())  # Should have >100KB free
   ```

### Linker Error: "undefined reference to esp_peer_open"

**Solution**: Static library not linked.

Check `webrtc/micropython.cmake` includes:
```cmake
target_link_libraries(usermod_webrtc INTERFACE idf::esp_peer)
```

## Memory Requirements

Minimum RAM requirements:

| Component | RAM Usage |
|-----------|-----------|
| ESP Peer Stack | ~60 KB |
| Data Channel Buffers | 200 KB (configurable) |
| DTLS Session | ~30 KB |
| **Total** | **~290 KB** |

**Recommendation**: Use ESP32-S3 with PSRAM for comfortable operation.

## Build Examples

### Minimal Build (ESP32-S3, no PSRAM)

```bash
make \
  BOARD=ESP32_GENERIC_S3 \
  USER_C_MODULES=/path/to/pyDirect/micropython.cmake \
  CMAKE_ARGS="-DMODULE_PYDIRECT_WEBRTC=ON" \
  all
```

Reduce buffer sizes to fit:
```c
// In modwebrtc.c:
#define WEBRTC_SEND_CACHE_SIZE (50 * 1024)
#define WEBRTC_RECV_CACHE_SIZE (50 * 1024)
```

### Full Build (ESP32-S3 with PSRAM)

```bash
make \
  BOARD=ESP32_GENERIC_S3 \
  USER_C_MODULES=/path/to/pyDirect/micropython.cmake \
  CMAKE_ARGS="-DMODULE_PYDIRECT_WEBRTC=ON -DMODULE_PYDIRECT_HTTPSERVER=ON -DMODULE_PYDIRECT_TWAI=ON" \
  all
```

### Custom Board Build

```bash
make \
  BOARD=SCRIPTO_S3 \
  USER_C_MODULES=/path/to/pyDirect/micropython.cmake \
  CMAKE_ARGS="-DMODULE_PYDIRECT_WEBRTC=ON -DMODULE_PYDIRECT_HTTPSERVER=ON" \
  all
```

## Next Steps

After successful build:

1. **Generate certificates** for HTTPS signaling:
   ```bash
   cd pyDirect
   ./generate-device-cert.sh
   ```

2. **Upload certificates** to device at `/certs/`

3. **Run example**: `examples/webrtc_echo_server.py`

4. **Test from browser**: See `README.md` for browser client code

## Additional Resources

- **ESP WebRTC Solution**: https://github.com/espressif/esp-webrtc-solution
- **ESP-IDF DTLS Guide**: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/mbedtls.html
- **WebRTC Spec**: https://www.w3.org/TR/webrtc/
- **pyDirect Main Docs**: `../../README.md`

## Support

For issues:
1. Check build logs for specific errors
2. Verify all dependencies are installed
3. Test basic functionality with test script
4. Open issue on GitHub with build output

Happy WebRTC coding! 🚀
