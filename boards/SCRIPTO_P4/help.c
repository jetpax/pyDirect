/*
 * Board-specific help text for Scripto ESP32-P4 board
 * Based on Waveshare ESP32-P4-Module with ESP32-C6 WiFi companion
 */

#include "py/builtin.h"

// Override the default ESP32 help text by providing our own esp32_help_text variable
// This works because we use --allow-multiple-definition and our version is linked first
// Override the default ESP32 help text by providing our own esp32_help_text variable
// This works because we use --allow-multiple-definition and our version is linked first
const char esp32_help_text[] =
    "Welcome to MicroPython with pyDirect on the Scripto P4+C6!\n"
    "\n"
    "Board: Scripto ESP32-P4 with ESP32-C6 WiFi companion\n"
    "MCU: ESP32-P4 (RISC-V dual-core)\n"
    "WiFi/BLE: ESP32-C6 companion chip\n"
    "\n"
    "For online docs please visit http://pydirect.com/\n"
    "\n"
    "Helper Functions:\n"
    "getSysInfo()            - Get comprehensive system info (SYS-INFO command)\n"
    "getNetworksInfo()       - Get all network interfaces info (NETWORKS-INFO command)\n"
    "neofetch()              - Display neofetch-style system banner with ANSI art\n"
    "getDeviceIP()           - Get device IP address, returns string\n"
    "getDeviceURL(path)      - Get device URL with correct protocol (http/https), returns string\n"
    "status_led()            - StatusLED instance for controlling onboard LED (or None if not available)\n"
    "\n"
    "Board Module:\n"
    "board.id.name           - Scripto P4+C6\n"
    "board.id.chip           - ESP32-P4\n"
    "board.has(ethernet)     - True/False\n"
    "board.pin(status_led)   - 1\n"
    "board.can(\"twai\")       - True/False\n"
    "board.device(ethernet)  - True/False\n"
    "board.sdmmc(sdcard)     - True/False\n"
    "board.audio(audio)      - True/False\n"
    "board.neopixel(neopixel)- True/False\n"
    "\n"
    "Background Tasks:\n"
    "bg_tasks.start(\"my_task\", my_task)\n"
    "bg_tasks.list_tasks()  # -> {\"my_task\": {\"state\": \"running\", \"system\": False}}\n"
    "bg_tasks.stop(\"my_task\")\n"
    "bg_tasks.stop_user_tasks()  # Stop all non-system tasks\n"
    "\n"
    "For further help on a specific object, type help(obj)\n"
    "For a list of available modules, type help('modules')\n"
;
