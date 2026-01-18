#ifndef MICROPY_HW_BOARD_NAME
#define MICROPY_HW_BOARD_NAME               "ESP32-S3-N8R2 with pyDirect"
#endif
#define MICROPY_HW_MCU_NAME                 "ESP32-S3"

// Enable UART REPL (required for serial console)
#define MICROPY_HW_ENABLE_UART_REPL         (1)

// Disable USB device mode - USB-OTG is used for host mode (USB modem support)
#define MICROPY_HW_ENABLE_USBDEV            (0)
#define MICROPY_HW_USB_CDC                  (0)

#define MICROPY_PY_NETWORK_HOSTNAME_DEFAULT "pyDirect-S3"

#define MICROPY_HW_I2C0_SCL                 (9)
#define MICROPY_HW_I2C0_SDA                 (8)

#define MICROPY_HW_SPI1_MOSI                (11)
#define MICROPY_HW_SPI1_MISO                (13)
#define MICROPY_HW_SPI1_SCK                 (12)
