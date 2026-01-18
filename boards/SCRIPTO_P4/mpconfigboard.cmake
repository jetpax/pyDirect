set(IDF_TARGET esp32p4)

set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    boards/sdkconfig.p4
    boards/sdkconfig.p4_wifi_common
    boards/sdkconfig.p4_wifi_c6
    ${MICROPY_BOARD_DIR}/sdkconfig.board
)

# Enable WiFi and BLE via ESP32-C6 companion chip
list(APPEND MICROPY_DEF_BOARD
    MICROPY_PY_NETWORK_WLAN=1
    MICROPY_PY_BLUETOOTH=1
)

# Use custom partition table for 16MB flash
set(MICROPY_PARTITION_TABLE ${MICROPY_BOARD_DIR}/partitions-16mb.csv)
