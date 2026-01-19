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

# Link LittleFS for webfiles module (needed for managed components)
# Managed component name is 'joltwallet__littlefs'
if(TARGET idf::joltwallet__littlefs)
    target_link_libraries(usermod_httpserver INTERFACE idf::joltwallet__littlefs)
    message(STATUS "pyDirect httpserver: Linked idf::joltwallet__littlefs component")
elseif(TARGET idf::littlefs)
    target_link_libraries(usermod_httpserver INTERFACE idf::littlefs)
    message(STATUS "pyDirect httpserver: Linked idf::littlefs component")
else()
    message(STATUS "pyDirect httpserver: littlefs managed component not found (modwebfiles.c may fail)")
endif()

# Link the module to MicroPython's usermod target
target_link_libraries(usermod INTERFACE usermod_httpserver)
