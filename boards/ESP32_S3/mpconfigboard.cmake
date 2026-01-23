# ESP32-S3 Generic Board Configuration
# Supports multiple flash/PSRAM configurations via MEMORY_PROFILE env var

set(IDF_TARGET esp32s3)

# Base SDK configs
set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    boards/sdkconfig.ble
    boards/sdkconfig.spiram_sx
    ${MICROPY_BOARD_DIR}/sdkconfig.board
)

# Memory profile selection - set via environment variable
# Default: 8MB flash, 2MB Quad PSRAM (N8R2)
# Options: 8MB_2MB (default), 16MB_8MB, 16MB_2MB
if(DEFINED ENV{MEMORY_PROFILE})
    set(MEMORY_PROFILE $ENV{MEMORY_PROFILE})
else()
    set(MEMORY_PROFILE "8MB_2MB")
endif()

# Select partition table based on memory profile
# Note: build.sh copies partition tables from partitions/ to the board dir root
# and the sdkconfig uses boards/ESP32_S3/partitions-*.csv (relative to ports/esp32)
if(MEMORY_PROFILE STREQUAL "16MB_8MB" OR MEMORY_PROFILE STREQUAL "16MB_2MB")
    # Use 16MB partition table - path is relative to ports/esp32
    set(MICROPY_PARTITION_TABLE boards/ESP32_S3/partitions-16mb.csv)
    list(APPEND SDKCONFIG_DEFAULTS ${MICROPY_BOARD_DIR}/sdkconfig.16mb)
else()
    # Use 8MB partition table (default)
    set(MICROPY_PARTITION_TABLE boards/ESP32_S3/partitions-8mb.csv)
endif()

# Add octal PSRAM config for 16MB_8MB profile
if(MEMORY_PROFILE STREQUAL "16MB_8MB")
    list(APPEND SDKCONFIG_DEFAULTS ${MICROPY_BOARD_DIR}/sdkconfig.oct)
endif()
