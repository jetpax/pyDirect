// ESP32-S3 with 16MB flash board configuration
// For modules like N16R2, N16R8

#ifndef MICROPY_HW_BOARD_NAME
#define MICROPY_HW_BOARD_NAME               "ESP32-S3 16MB with pyDirect"
#endif

#ifndef MICROPY_HW_MCU_NAME
#define MICROPY_HW_MCU_NAME                 "ESP32S3"
#endif

// Enable USB Serial JTAG
#define MICROPY_HW_USB_CDC                  (1)

// Feature flags
#define MICROPY_HW_ENABLE_SDCARD            (1)

// USB Serial/JTAG packet size
#ifndef USB_SERIAL_JTAG_PACKET_SZ_BYTES
#define USB_SERIAL_JTAG_PACKET_SZ_BYTES     (64)
#endif
