# CMake configuration for pyDirect GVRET module
# GVRET protocol implementation for CAN over TCP (SavvyCAN compatible)
#
# NOTE: GVRET depends on the CAN module for CAN manager API

# Include CAN module first if not already included (gvret requires it)
# This must be done BEFORE setting GVRET_MODULE_DIR to avoid variable conflict
if(NOT TARGET usermod_can)
    include(${PYDIRECT_DIR}/can/micropython.cmake)
endif()

# Get the directory where this cmake file is located (use unique variable name)
set(GVRET_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

# Create the usermod interface library
add_library(usermod_gvret INTERFACE)

# Add source files
target_sources(usermod_gvret INTERFACE
    ${GVRET_MODULE_DIR}/modgvret.c
)

# Add include directories (including can/ for modcan.h dependency)
target_include_directories(usermod_gvret INTERFACE
    ${GVRET_MODULE_DIR}
    ${PYDIRECT_DIR}/can
)

# Link to usermod_can for the CAN manager API
target_link_libraries(usermod_gvret INTERFACE usermod_can)

# Link to MicroPython's usermod target
target_link_libraries(usermod INTERFACE usermod_gvret)
