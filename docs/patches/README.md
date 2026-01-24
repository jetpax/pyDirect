# MicroPython Patches for pyDirect

These patches add the minimal required changes to MicroPython for pyDirect modules.

## Patches

| Patch | Purpose |
|-------|---------|
| `001-esp32-http-server-components.patch` | Adds `esp_http_server` + `esp_https_server` IDF components |
| `002-esp32-managed-components.patch` | Adds `littlefs` + `husarnet` managed components |
| `003-esp-idf-rmt-log-suppression.patch` | Suppresses RMT resolution loss warnings (ESP-IDF) |
| `004-esp32-dupterm-slots.patch` | Increases dupterm slots from 1 to 3 for WebREPL + WebRTC |

## Applying Patches to MicroPython 1.27+

```bash
# Clone fresh MicroPython
git clone https://github.com/micropython/micropython.git
cd micropython
git checkout v1.27.0  # or whatever tag

# Apply MicroPython patches
git apply /path/to/pyDirect/docs/patches/001-esp32-http-server-components.patch
git apply /path/to/pyDirect/docs/patches/002-esp32-managed-components.patch
git apply /path/to/pyDirect/docs/patches/004-esp32-dupterm-slots.patch

# Initialize submodules
make -C mpy-cross
git submodule update --init --recursive
```

## Applying ESP-IDF Patch (Optional)

The RMT log suppression patch is **optional** but recommended to reduce console noise:

```bash
cd $ESP_IDF_PATH
git apply /path/to/pyDirect/docs/patches/003-esp-idf-rmt-log-suppression.patch
```

This changes RMT "channel resolution loss" messages from WARNING to DEBUG level.

## If Patches Fail

If the patches don't apply cleanly (due to upstream changes), you can manually add:

### esp32_common.cmake

Find the `list(APPEND IDF_COMPONENTS` section and add after `esp_event`:
```cmake
    esp_http_server  # Required for pyDirect httpserver USER_C_MODULES
    esp_https_server  # Required for pyDirect httpserver USER_C_MODULES (HTTPS support)
```

### idf_component.yml

Add before the `idf:` section:
```yaml
  # Required for pyDirect httpserver USER_C_MODULES (modwebfiles.c uses esp_vfs_littlefs)
  joltwallet/littlefs: "^1.0.0"
  # Required for pyDirect husarnet VPN module
  husarnet/esp_husarnet: "^0.0.15"
```

### RMT Log Suppression (ESP-IDF)

In `components/esp_driver_rmt/src/rmt_common.c`, change line 270:
```c
// From:
ESP_LOGW(TAG, "channel resolution loss, real=%"PRIu32, chan->resolution_hz);

// To:
ESP_LOGD(TAG, "channel resolution loss, real=%"PRIu32, chan->resolution_hz);
```

## Note on modesp32.c

If you need modules under `esp32.*` namespace (to avoid collision with built-in `webrepl`), you'll need to manually add extern declarations and module references in `ports/esp32/modesp32.c`. This is more invasive and may need adjustment per MicroPython version.
