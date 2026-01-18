# PLC module - HomePlug / SLAC / EXI for CCS charging
# Provides:
# - SLAC responder (EVSE mode) for CCS DC fast charging
# - MME commands to configure TPLink HomePlug modem
# - DIN EXI codec (future)

add_library(usermod_plc INTERFACE)

target_sources(usermod_plc INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/modplc.c
    ${CMAKE_CURRENT_LIST_DIR}/exi_din.c
)

target_include_directories(usermod_plc INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

# Link against ESP-IDF components
target_link_libraries(usermod_plc INTERFACE
    idf::esp_eth
    idf::esp_netif
    idf::vfs
)

target_link_libraries(usermod INTERFACE usermod_plc)
