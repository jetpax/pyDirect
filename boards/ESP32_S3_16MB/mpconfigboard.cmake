set(IDF_TARGET esp32s3)

# ESP32-S3 with 16MB flash configuration
set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    boards/sdkconfig.ble
    boards/sdkconfig.spiram_sx
    ${MICROPY_BOARD_DIR}/sdkconfig.board
)

# Use 16MB partition table
# Path is relative to ports/esp32 (build.sh copies partition file there)
set(MICROPY_PARTITION_TABLE boards/ESP32_S3_16MB/partitions-16mb.csv)
