# ESP32-S3 Generic Board Configuration (8MB Flash)

set(IDF_TARGET esp32s3)

# Base SDK configs
set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    boards/sdkconfig.ble
    boards/sdkconfig.spiram_sx
    ${MICROPY_BOARD_DIR}/sdkconfig.board
)

# 8MB partition table
set(MICROPY_PARTITION_TABLE boards/ESP32_S3/partitions-8mb.csv)
