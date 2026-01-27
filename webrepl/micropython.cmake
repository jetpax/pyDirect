# CMake configuration for pyDirect WebREPL Binary Protocol module
# Unified module with pluggable transports (WebSocket, WebRTC)

# Get the directory where this cmake file is located
set(MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

# WebREPL Binary module
add_library(usermod_webrepl INTERFACE)

target_sources(usermod_webrepl INTERFACE
    ${MODULE_DIR}/wbp_common.c
    ${MODULE_DIR}/modwebrepl_binary.c
    # Legacy modules (kept for reference, will be removed after testing)
    # ${MODULE_DIR}/modwebrepl.c
    # ${MODULE_DIR}/modwebrepl_rtc.c
)

target_include_directories(usermod_webrepl INTERFACE
    ${MODULE_DIR}
)

# Link TinyCBOR component (from espressif/cbor managed component)
if(TARGET idf::cbor)
    target_link_libraries(usermod_webrepl INTERFACE idf::cbor)
    message(STATUS "pyDirect webrepl: Linked idf::cbor managed component")
else()
    message(STATUS "pyDirect webrepl: idf::cbor not available yet (will resolve during build)")
endif()

# Link usermod_webrtc for modwebrtc.h include path
if(TARGET usermod_webrtc)
    target_link_libraries(usermod_webrepl INTERFACE usermod_webrtc)
    message(STATUS "pyDirect webrepl: Linked usermod_webrtc for WebRTC support")
else()
    message(STATUS "pyDirect webrepl: usermod_webrtc not available (WebRTC transport disabled)")
endif()

# Link the module to embedded Python's usermod target
target_link_libraries(usermod INTERFACE usermod_webrepl)

