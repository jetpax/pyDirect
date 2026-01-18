# CMake configuration for pyDirect HTTP Server modules
# This includes: httpserver, webfiles, wsserver, webDAP

# Get the directory where this cmake file is located
set(MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

# HTTP Server module
add_library(usermod_httpserver INTERFACE)

target_sources(usermod_httpserver INTERFACE
    ${MODULE_DIR}/modhttpserver.c
    ${MODULE_DIR}/modwebfiles.c
    ${MODULE_DIR}/modwsserver.c
    ${MODULE_DIR}/modwebDAP.c
)

target_include_directories(usermod_httpserver INTERFACE
    ${MODULE_DIR}
)

# Link cJSON component (ESP-IDF built-in) for webDAP module
if(TARGET idf::json)
    target_link_libraries(usermod_httpserver INTERFACE idf::json)
    message(STATUS "pyDirect httpserver: Linked idf::json component")
else()
    message(STATUS "pyDirect httpserver: idf::json not available yet (will resolve during build)")
endif()

# Link the module to MicroPython's usermod target
target_link_libraries(usermod INTERFACE usermod_httpserver)
