# Name: ESP32-S3-N8R2
set(IDF_TARGET esp32s3)

set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    boards/sdkconfig.ble
    boards/sdkconfig.spiram_sx
    ${MICROPY_BOARD_DIR}/sdkconfig.board
)

set(MICROPY_PARTITION_TABLE ${MICROPY_BOARD_DIR}/partitions-8mb.csv)
