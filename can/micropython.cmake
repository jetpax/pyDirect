# CMake configuration for pyDirect CAN module
# ESP32 TWAI (Two-Wire Automotive Interface) / CAN bus support

# Get the directory where this cmake file is located
set(MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

# Create the usermod interface library
add_library(usermod_can INTERFACE)

# Add source files
target_sources(usermod_can INTERFACE
    ${MODULE_DIR}/modcan.c
)

# Add include directory
target_include_directories(usermod_can INTERFACE
    ${MODULE_DIR}
)

# Link to embedded Python's usermod target
target_link_libraries(usermod INTERFACE usermod_can)
