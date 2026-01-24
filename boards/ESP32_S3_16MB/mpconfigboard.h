// ESP32-S3 with 16MB flash board configuration
// For modules like N16R2, N16R8

#ifndef MICROPY_HW_BOARD_NAME
#define MICROPY_HW_BOARD_NAME               "ESP32-S3 16MB with pyDirect"
#endif

#ifndef MICROPY_HW_MCU_NAME
#define MICROPY_HW_MCU_NAME                 "ESP32S3"
#endif

// Disable USB device mode - USB-OTG is used for host mode (USB modem support)
#define MICROPY_HW_ENABLE_USBDEV            (0)
#define MICROPY_HW_USB_CDC                  (0)

// Feature flags
#define MICROPY_HW_ENABLE_SDCARD            (1)

// Disable UART REPL - using WebREPL instead to avoid duplicate output
#define MICROPY_HW_ENABLE_UART_REPL         (1)

