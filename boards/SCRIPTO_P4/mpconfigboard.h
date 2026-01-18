// Scripto ESP32-P4 board configuration
// Based on Waveshare ESP32-P4-Module with ESP32-C6 WiFi companion

#ifndef MICROPY_HW_BOARD_NAME
#define MICROPY_HW_BOARD_NAME               "Scripto P4+C6"
#endif

#ifndef MICROPY_HW_MCU_NAME
#define MICROPY_HW_MCU_NAME                 "ESP32P4"
#endif

// ESP-NOW not supported on P4 (uses ESP-Hosted for WiFi via C6)
#define MICROPY_PY_ESPNOW                   (0)

// Enable SD card support
#define MICROPY_HW_ENABLE_SDCARD            (1)

// USB Serial/JTAG packet size
#ifndef USB_SERIAL_JTAG_PACKET_SZ_BYTES
#define USB_SERIAL_JTAG_PACKET_SZ_BYTES     (64)
#endif

// REPL via UART (USB reserved for host mode)
#define MICROPY_HW_ENABLE_UART_REPL         (1)

// Enable I2S
#define MICROPY_PY_MACHINE_I2S              (1)

// WiFi/BT enabled via ESP32-C6 companion chip (set in cmake)

// Enable Ethernet LAN support (P4 has internal EMAC)
#define MICROPY_PY_NETWORK_LAN              (1)

// I2C pins (adjust for your Waveshare module pinout)
#define MICROPY_HW_I2C0_SCL                 (8)
#define MICROPY_HW_I2C0_SDA                 (7)
