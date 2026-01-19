# CMake configuration for pyDirect USB Modem module
# 
# Wraps esp-iot-solution's iot_usbh_modem component for:
# - AT command communication with USB modems
# - PPP data connection
# - GPS location services
# - Network connectivity checks (ping)
#
# V2.0 MIGRATION: Now uses ESP Component Registry instead of local fork
# Dependencies are declared in ports/esp32/main/idf_component.yml

set(MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

# Create the usermod interface library
add_library(usermod_usbmodem INTERFACE)

# Source files - just our MicroPython wrapper
target_sources(usermod_usbmodem INTERFACE
    ${MODULE_DIR}/modusbmodem.c
)

# Include directories from managed components for V2.0 API
# The managed_components are in the MicroPython ports/esp32 build tree
# We need to find them relative to CMAKE_BINARY_DIR (the build directory)
# Build dir is like: /path/to/micropython/ports/esp32/build-BOARDNAME
# Managed components are in: /path/to/micropython/ports/esp32/managed_components
get_filename_component(MP_ESP32_DIR "${CMAKE_BINARY_DIR}/.." ABSOLUTE)
set(managed_comp_dir "${MP_ESP32_DIR}/managed_components")

if(EXISTS "${managed_comp_dir}")
    message(STATUS "pyDirect usbmodem: Found managed_components at ${managed_comp_dir}")
    
    # Add include paths from managed components (simple pattern - no version hashes)
    if(EXISTS "${managed_comp_dir}/espressif__iot_usbh_modem/include")
        target_include_directories(usermod_usbmodem INTERFACE 
            "${managed_comp_dir}/espressif__iot_usbh_modem/include")
        message(STATUS "pyDirect usbmodem: Added iot_usbh_modem includes")
    endif()
    
    if(EXISTS "${managed_comp_dir}/espressif__iot_usbh_cdc/include")
        target_include_directories(usermod_usbmodem INTERFACE 
            "${managed_comp_dir}/espressif__iot_usbh_cdc/include")
        message(STATUS "pyDirect usbmodem: Added iot_usbh_cdc includes")
    endif()
    
    if(EXISTS "${managed_comp_dir}/espressif__modem_at/include")
        target_include_directories(usermod_usbmodem INTERFACE 
            "${managed_comp_dir}/espressif__modem_at/include")
        message(STATUS "pyDirect usbmodem: Added modem_at includes")
    endif()
else()
    message(STATUS "pyDirect usbmodem: managed_components not found at ${managed_comp_dir} (will resolve during build)")
endif()

target_include_directories(usermod_usbmodem INTERFACE
    ${MODULE_DIR}
)

# Configuration defines (normally from Kconfig)
target_compile_definitions(usermod_usbmodem INTERFACE
    # Task configuration
    CONFIG_USBH_TASK_CORE_ID=0
    CONFIG_USBH_TASK_BASE_PRIORITY=5
    
    # Buffer sizes
    CONFIG_CONTROL_TRANSFER_BUFFER_SIZE=256
    CONFIG_IN_TRANSFER_BUFFER_SIZE=1024
    CONFIG_OUT_TRANSFER_BUFFER_SIZE=1024
    CONFIG_DEVICE_ADDRESS_LIST_NUM=8
    
    # Modem configuration - SIM7600 with dual interface for AT + PPP
    CONFIG_MODEM_TARGET_USER=1
    CONFIG_MODEM_TARGET_NAME="SIM7600"
    CONFIG_MODEM_SUPPORT_SECONDARY_AT_PORT=1
    CONFIG_MODEM_USB_ITF=0x03
    CONFIG_MODEM_USB_ITF2=0x02
    
    # Modem timeouts
    CONFIG_MODEM_COMMAND_TIMEOUT_DEFAULT=2000
    CONFIG_MODEM_COMMAND_TIMEOUT_OPERATOR=6000
    CONFIG_MODEM_COMMAND_TIMEOUT_RESET=6000
    CONFIG_MODEM_COMMAND_TIMEOUT_MODE_CHANGE=10000
    CONFIG_MODEM_COMMAND_TIMEOUT_POWEROFF=1000
    
    # GPIO configuration
    CONFIG_MODEM_POWER_GPIO=0
    CONFIG_MODEM_RESET_GPIO=0
    CONFIG_MODEM_POWER_GPIO_INACTIVE_LEVEL=1
    CONFIG_MODEM_RESET_GPIO_INACTIVE_LEVEL=1
    
    # APN defaults
    CONFIG_MODEM_PPP_APN="internet"
    CONFIG_MODEM_SIM_PIN_PWD=""
    CONFIG_USB_OTG_SUPPORTED=1
)

# Compiler options
target_compile_options(usermod_usbmodem INTERFACE
    -Wno-error=implicit-function-declaration
    -Wno-error=unused-variable
    -Wno-error=unused-function
)

# Link IDF components from ESP Component Registry
if(TARGET idf::iot_usbh_modem)
    target_link_libraries(usermod_usbmodem INTERFACE idf::iot_usbh_modem)
    message(STATUS "pyDirect usbmodem: Linked idf::iot_usbh_modem")
endif()

if(TARGET idf::iot_usbh_cdc)
    target_link_libraries(usermod_usbmodem INTERFACE idf::iot_usbh_cdc)
    message(STATUS "pyDirect usbmodem: Linked idf::iot_usbh_cdc")
endif()

if(TARGET idf::modem_at)
    target_link_libraries(usermod_usbmodem INTERFACE idf::modem_at)
    message(STATUS "pyDirect usbmodem: Linked idf::modem_at")
endif()

# Link the usermod to MicroPython
target_link_libraries(usermod INTERFACE usermod_usbmodem)
