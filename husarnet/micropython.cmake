# CMake configuration for pyDirect Husarnet VPN module
#
# Wraps the Husarnet P2P VPN managed component for MicroPython:
# - Secure P2P VPN connectivity
# - IPv6-based device identity (fc94::/16 network)
# - Peer discovery and management
#
# DEPENDENCY: husarnet/esp_husarnet managed component (in idf_component.yml)

set(MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

# Create the usermod interface library
add_library(usermod_husarnet INTERFACE)

# Source files - just our MicroPython wrapper
target_sources(usermod_husarnet INTERFACE
    ${MODULE_DIR}/modhusarnet.c
)

# Include directory for our module
target_include_directories(usermod_husarnet INTERFACE
    ${MODULE_DIR}
)

# Link the usermod to MicroPython
target_link_libraries(usermod INTERFACE usermod_husarnet)

# Link the Husarnet managed component
# The esp_husarnet component is pulled in via idf_component.yml
if(TARGET idf::esp_husarnet)
    target_link_libraries(usermod_husarnet INTERFACE idf::esp_husarnet)
    message(STATUS "pyDirect husarnet: Linked idf::esp_husarnet managed component")
else()
    message(STATUS "pyDirect husarnet: idf::esp_husarnet not available yet (will resolve during build)")
endif()

message(STATUS "pyDirect husarnet: Module configured (using managed component)")
