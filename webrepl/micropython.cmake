# CMake configuration for pyDirect WebREPL module
# This includes: webrepl (WebSocket REPL) and webrepl_rtc (WebRTC REPL)

# Get the directory where this cmake file is located
set(MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

# WebREPL module
add_library(usermod_webrepl INTERFACE)

target_sources(usermod_webrepl INTERFACE
    ${MODULE_DIR}/modwebrepl.c
    ${MODULE_DIR}/modwebrepl_rtc.c
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

# Link the module to embedded Python's usermod target
target_link_libraries(usermod INTERFACE usermod_webrepl)
