# pyDirect: High-Performance C Modules for Embedded Python
#
# This is the top-level CMake orchestrator for pyDirect modules.
# Supports ESP32 (ESP-IDF) and Zephyr RTOS.
#
# Usage:
#   make USER_C_MODULES=/path/to/pyDirect/micropython.cmake
#
# Or enable/disable specific modules:
#   cmake -DMODULE_PYDIRECT_HTTPSERVER=ON -DMODULE_PYDIRECT_CAN=ON ...

# Get the directory where this cmake file is located
set(PYDIRECT_DIR ${CMAKE_CURRENT_LIST_DIR})

# Module options - users can enable/disable modules
option(MODULE_PYDIRECT_HTTPSERVER "Enable pyDirect HTTP Server modules (httpserver, webfiles, wsserver, webDAP)" ON)
option(MODULE_PYDIRECT_WEBREPL "Enable pyDirect WebREPL modules (webrepl, webrepl_rtc)" ON)
option(MODULE_PYDIRECT_WEBRTC "Enable pyDirect WebRTC module (DataChannel transport)" OFF)
option(MODULE_PYDIRECT_CAN "Enable pyDirect CAN module (TWAI/CAN bus)" OFF)
option(MODULE_PYDIRECT_GVRET "Enable pyDirect GVRET module (CAN over TCP for SavvyCAN)" OFF)
option(MODULE_PYDIRECT_HUSARNET "Enable pyDirect Husarnet P2P VPN module" OFF)
option(MODULE_PYDIRECT_USBMODEM "Enable pyDirect USB Modem module" OFF)
option(MODULE_PYDIRECT_PLC "Enable pyDirect PLC module (CCS/NACS charging via HomePlug)" OFF)

message(STATUS "pyDirect: Module options:")
message(STATUS "  HTTPSERVER: ${MODULE_PYDIRECT_HTTPSERVER}")
message(STATUS "  WEBREPL: ${MODULE_PYDIRECT_WEBREPL}")
message(STATUS "  WEBRTC: ${MODULE_PYDIRECT_WEBRTC}")
message(STATUS "  CAN: ${MODULE_PYDIRECT_CAN}")
message(STATUS "  GVRET: ${MODULE_PYDIRECT_GVRET}")
message(STATUS "  HUSARNET: ${MODULE_PYDIRECT_HUSARNET}")
message(STATUS "  USBMODEM: ${MODULE_PYDIRECT_USBMODEM}")
message(STATUS "  PLC: ${MODULE_PYDIRECT_PLC}")

# Include modules conditionally
if(MODULE_PYDIRECT_HTTPSERVER)
    message(STATUS "pyDirect: Including HTTP Server modules...")
    include(${PYDIRECT_DIR}/httpserver/micropython.cmake)
endif()

if(MODULE_PYDIRECT_WEBREPL)
    message(STATUS "pyDirect: Including WebREPL modules...")
    include(${PYDIRECT_DIR}/webrepl/micropython.cmake)
endif()

if(MODULE_PYDIRECT_WEBRTC)
    message(STATUS "pyDirect: Including WebRTC module...")
    include(${PYDIRECT_DIR}/webrtc/micropython.cmake)
endif()

if(MODULE_PYDIRECT_CAN)
    message(STATUS "pyDirect: Including CAN module...")
    include(${PYDIRECT_DIR}/can/micropython.cmake)
endif()

if(MODULE_PYDIRECT_GVRET)
    message(STATUS "pyDirect: Including GVRET module...")
    include(${PYDIRECT_DIR}/gvret/micropython.cmake)
endif()

if(MODULE_PYDIRECT_HUSARNET)
    message(STATUS "pyDirect: Including Husarnet VPN module...")
    include(${PYDIRECT_DIR}/husarnet/micropython.cmake)
endif()

if(MODULE_PYDIRECT_USBMODEM)
    message(STATUS "pyDirect: Including USB Modem module...")
    include(${PYDIRECT_DIR}/usbmodem/micropython.cmake)
endif()

if(MODULE_PYDIRECT_PLC)
    message(STATUS "pyDirect: Including PLC module (CCS/NACS charging)...")
    include(${PYDIRECT_DIR}/plc/micropython.cmake)
endif()

# Report what was included
set(INCLUDED_MODULES)
if(MODULE_PYDIRECT_HTTPSERVER)
    list(APPEND INCLUDED_MODULES "httpserver")
endif()
if(MODULE_PYDIRECT_WEBREPL)
    list(APPEND INCLUDED_MODULES "webrepl")
endif()
if(MODULE_PYDIRECT_WEBRTC)
    list(APPEND INCLUDED_MODULES "webrtc")
endif()
if(MODULE_PYDIRECT_CAN)
    list(APPEND INCLUDED_MODULES "can")
endif()
if(MODULE_PYDIRECT_GVRET)
    list(APPEND INCLUDED_MODULES "gvret")
endif()
if(MODULE_PYDIRECT_HUSARNET)
    list(APPEND INCLUDED_MODULES "husarnet")
endif()
if(MODULE_PYDIRECT_USBMODEM)
    list(APPEND INCLUDED_MODULES "usbmodem")
endif()
if(MODULE_PYDIRECT_PLC)
    list(APPEND INCLUDED_MODULES "plc")
endif()

if(INCLUDED_MODULES)
    string(REPLACE ";" ", " INCLUDED_MODULES_STR "${INCLUDED_MODULES}")
    message(STATUS "pyDirect: Enabled modules: ${INCLUDED_MODULES_STR}")
else()
    message(WARNING "pyDirect: No modules enabled! Enable at least one module using CMake options.")
endif()
