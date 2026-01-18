# CMake configuration for pyDirect WebRTC module
# Provides WebRTC PeerConnection with DataChannel for browser integration

# Get the directory where this cmake file is located
set(MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

# Create the usermod interface library
add_library(usermod_webrtc INTERFACE)

# Add source files
target_sources(usermod_webrtc INTERFACE
    ${MODULE_DIR}/modwebrtc.c
)

# Add include directory
target_include_directories(usermod_webrtc INTERFACE
    ${MODULE_DIR}
)

# Link esp-webrtc-solution components
# These are expected to be available via ESP-IDF component manager
if(TARGET idf::esp_peer)
    target_link_libraries(usermod_webrtc INTERFACE idf::esp_peer)
    message(STATUS "pyDirect webrtc: Linked idf::esp_peer component")
else()
    message(STATUS "pyDirect webrtc: idf::esp_peer not available yet (will resolve during build)")
endif()

# Link CBOR library for WebREPL Binary Protocol
if(TARGET idf::cbor)
    target_link_libraries(usermod_webrtc INTERFACE idf::cbor)
    message(STATUS "pyDirect webrtc: Linked idf::cbor component")
else()
    message(STATUS "pyDirect webrtc: idf::cbor not available yet (will resolve during build)")
endif()

# Note: media_lib_sal is NOT needed for data-channel-only mode
# esp_peer provides weak implementations of media_lib functions

# Link to MicroPython's usermod target
target_link_libraries(usermod INTERFACE usermod_webrtc)
