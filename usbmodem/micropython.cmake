# CMake configuration for pyDirect USB Modem module
# 
# Wraps esp-iot-solution's iot_usbh_modem component for:
# - AT command communication with USB modems
# - PPP data connection
# - GPS location services
# - Network connectivity checks (ping)
#
# DEPENDENCY: This module requires esp-iot-solution

set(MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

# Find esp-iot-solution
if(DEFINED ENV{ESP_IOT_SOLUTION_DIR})
    set(IOT_SOLUTION_DIR $ENV{ESP_IOT_SOLUTION_DIR})
    message(STATUS "pyDirect usbmodem: Using ESP_IOT_SOLUTION_DIR=$ENV{ESP_IOT_SOLUTION_DIR}")
else()
    set(POSSIBLE_PATHS
        "${CMAKE_CURRENT_LIST_DIR}/../../esp-iot-solution"
        "${CMAKE_CURRENT_LIST_DIR}/../../../esp-iot-solution"
        "${CMAKE_SOURCE_DIR}/../../esp-iot-solution"
    )
    
    foreach(PATH ${POSSIBLE_PATHS})
        get_filename_component(ABS_PATH "${PATH}" ABSOLUTE)
        if(EXISTS "${ABS_PATH}/components/usb/iot_usbh_modem")
            set(IOT_SOLUTION_DIR ${ABS_PATH})
            message(STATUS "pyDirect usbmodem: Found esp-iot-solution at ${ABS_PATH}")
            break()
        endif()
    endforeach()
endif()

if(NOT DEFINED IOT_SOLUTION_DIR OR NOT EXISTS "${IOT_SOLUTION_DIR}/components/usb/iot_usbh_modem")
    message(FATAL_ERROR 
        "pyDirect usbmodem: esp-iot-solution not found!\n"
        "Please set ESP_IOT_SOLUTION_DIR or clone esp-iot-solution next to pyDirect"
    )
endif()

# Component directories
set(USBH_CDC_DIR "${IOT_SOLUTION_DIR}/components/usb/iot_usbh_cdc")
set(USBH_MODEM_DIR "${IOT_SOLUTION_DIR}/components/usb/iot_usbh_modem")

# Create the usermod interface library
add_library(usermod_usbmodem INTERFACE)

# Source files
target_sources(usermod_usbmodem INTERFACE
    ${MODULE_DIR}/modusbmodem.c
    
    # iot_usbh_modem component
    ${USBH_MODEM_DIR}/src/esp_modem.c
    ${USBH_MODEM_DIR}/src/esp_modem_dce.c
    ${USBH_MODEM_DIR}/src/esp_modem_dce_command_lib.c
    ${USBH_MODEM_DIR}/src/esp_modem_dce_common_commands.c
    ${USBH_MODEM_DIR}/src/esp_modem_netif.c
    ${USBH_MODEM_DIR}/src/esp_modem_recov_helper.c
    ${USBH_MODEM_DIR}/src/esp_modem_usb_dte.c
    ${USBH_MODEM_DIR}/src/usbh_modem_board.c
    # Note: usbh_modem_wifi.c excluded - not needed
    
    # iot_usbh_cdc component (dependency)
    ${USBH_CDC_DIR}/iot_usbh_cdc.c
    ${USBH_CDC_DIR}/iot_usbh_descriptor.c
)

target_include_directories(usermod_usbmodem INTERFACE
    ${MODULE_DIR}
    ${USBH_MODEM_DIR}/include
    ${USBH_MODEM_DIR}/private_include
    ${USBH_CDC_DIR}
    ${USBH_CDC_DIR}/include
    ${USBH_CDC_DIR}/private_include
)

# Configuration defines (normally from Kconfig)
# Note: USB interface settings now in sdkconfig.board
target_compile_definitions(usermod_usbmodem INTERFACE
    # iot_usbh_cdc version
    IOT_USBH_CDC_VER_MAJOR=1
    IOT_USBH_CDC_VER_MINOR=1
    IOT_USBH_CDC_VER_PATCH=0
    
    # iot_usbh_modem version
    IOT_USBH_MODEM_VER_MAJOR=1
    IOT_USBH_MODEM_VER_MINOR=0
    IOT_USBH_MODEM_VER_PATCH=0
    
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
    # Interface 0x03: Primary (PPP data)
    # Interface 0x02: Secondary AT port (always available during PPP)
    CONFIG_MODEM_SUPPORT_SECONDARY_AT_PORT=1
    CONFIG_MODEM_USB_ITF=0x03
    CONFIG_MODEM_USB_ITF2=0x02
    
    # Modem timeouts
    CONFIG_MODEM_COMMAND_TIMEOUT_DEFAULT=2000
    CONFIG_MODEM_COMMAND_TIMEOUT_OPERATOR=6000
    CONFIG_MODEM_COMMAND_TIMEOUT_RESET=6000
    CONFIG_MODEM_COMMAND_TIMEOUT_MODE_CHANGE=10000
    CONFIG_MODEM_COMMAND_TIMEOUT_POWEROFF=1000
    
    # Modem dial retry
    CONFIG_MODEM_DIAL_RETRY_TIMES=5
    
    # GPIO configuration
    CONFIG_MODEM_POWER_GPIO=0
    CONFIG_MODEM_RESET_GPIO=0
    CONFIG_MODEM_POWER_GPIO_INACTIVE_LEVEL=1
    CONFIG_MODEM_RESET_GPIO_INACTIVE_LEVEL=1
    
    # APN and SIM PIN defaults (runtime overridable)
    CONFIG_MODEM_PPP_APN="internet"
    CONFIG_MODEM_SIM_PIN_PWD=""
    
    # USB Host configuration
    CONFIG_USB_OTG_SUPPORTED=1
)

# Compiler options
target_compile_options(usermod_usbmodem INTERFACE
    -Wno-error=implicit-function-declaration
    -Wno-error=unused-variable
    -Wno-error=unused-function
)

# Link the usermod to MicroPython
target_link_libraries(usermod INTERFACE usermod_usbmodem)
