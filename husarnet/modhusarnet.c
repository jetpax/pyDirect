/*
 * modhusarnet.c - MicroPython module for Husarnet P2P VPN
 *
 * Wraps the Husarnet ESP32 SDK to provide secure P2P VPN connectivity.
 * Devices get a stable fc94::/16 IPv6 address based on their public key.
 *
 * Copyright (c) 2026 Jonathan Elliot Peace
 * SPDX-License-Identifier: MIT
 */

#include "py/runtime.h"
#include "py/obj.h"
#include "py/objlist.h"
#include "py/mphal.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Include Husarnet C API from managed component
#include "husarnet.h"

static const char *TAG = "HUSARNET";

// Global state
static HusarnetClient *g_husarnet_client = NULL;
static bool g_husarnet_initialized = false;

// Python: husarnet.init()
// Initialize Husarnet client (must be called before join)
static mp_obj_t husarnet_mp_init(void) {
    if (g_husarnet_initialized) {
        ESP_LOGW(TAG, "Husarnet already initialized");
        return mp_const_true;
    }
    
    ESP_LOGI(TAG, "Initializing Husarnet VPN client");
    
    g_husarnet_client = husarnet_init();
    if (g_husarnet_client == NULL) {
        mp_raise_msg(&mp_type_OSError, 
            MP_ERROR_TEXT("Failed to initialize Husarnet client"));
    }
    
    g_husarnet_initialized = true;
    ESP_LOGI(TAG, "Husarnet client initialized successfully");
    
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_0(husarnet_init_obj, husarnet_mp_init);

// Python: husarnet.join(hostname, join_code)
// Join a Husarnet network with the given hostname and join code
static mp_obj_t husarnet_mp_join(mp_obj_t hostname_obj, mp_obj_t join_code_obj) {
    if (!g_husarnet_initialized || g_husarnet_client == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, 
            MP_ERROR_TEXT("Husarnet not initialized. Call husarnet.init() first"));
    }
    
    const char *hostname = mp_obj_str_get_str(hostname_obj);
    const char *join_code = mp_obj_str_get_str(join_code_obj);
    
    if (strlen(hostname) == 0) {
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("Hostname cannot be empty"));
    }
    
    if (strlen(join_code) == 0) {
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("Join code cannot be empty"));
    }
    
    ESP_LOGI(TAG, "Joining Husarnet network as '%s'", hostname);
    
    // This call blocks until the VPN connection is established
    husarnet_join(g_husarnet_client, hostname, join_code);
    
    ESP_LOGI(TAG, "Successfully joined Husarnet network");
    
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_2(husarnet_join_obj, husarnet_mp_join);

// Python: husarnet.is_joined() -> bool
// Returns True if connected to a Husarnet network
static mp_obj_t husarnet_mp_is_joined(void) {
    if (!g_husarnet_initialized || g_husarnet_client == NULL) {
        return mp_const_false;
    }
    
    return husarnet_is_joined(g_husarnet_client) ? mp_const_true : mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_0(husarnet_is_joined_obj, husarnet_mp_is_joined);

// Python: husarnet.get_ip() -> str
// Returns the device's Husarnet IPv6 address
static mp_obj_t husarnet_mp_get_ip(void) {
    if (!g_husarnet_initialized || g_husarnet_client == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, 
            MP_ERROR_TEXT("Husarnet not initialized"));
    }
    
    if (!husarnet_is_joined(g_husarnet_client)) {
        return mp_const_none;
    }
    
    char ip_buffer[HUSARNET_IP_STR_LEN];
    memset(ip_buffer, 0, sizeof(ip_buffer));
    
    uint8_t result = husarnet_get_ip_address(g_husarnet_client, ip_buffer, sizeof(ip_buffer));
    if (!result) {
        return mp_const_none;
    }
    
    return mp_obj_new_str(ip_buffer, strlen(ip_buffer));
}
static MP_DEFINE_CONST_FUN_OBJ_0(husarnet_get_ip_obj, husarnet_mp_get_ip);

// Python: husarnet.set_dashboard_fqdn(fqdn)
// Set custom dashboard FQDN for self-hosted setups
// Must be called BEFORE join()
static mp_obj_t husarnet_mp_set_dashboard_fqdn(mp_obj_t fqdn_obj) {
    if (!g_husarnet_initialized || g_husarnet_client == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, 
            MP_ERROR_TEXT("Husarnet not initialized. Call husarnet.init() first"));
    }
    
    if (husarnet_is_joined(g_husarnet_client)) {
        mp_raise_msg(&mp_type_RuntimeError, 
            MP_ERROR_TEXT("Cannot set dashboard FQDN after joining network"));
    }
    
    const char *fqdn = mp_obj_str_get_str(fqdn_obj);
    
    if (strlen(fqdn) == 0) {
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("FQDN cannot be empty"));
    }
    
    ESP_LOGI(TAG, "Setting dashboard FQDN to: %s", fqdn);
    husarnet_set_dashboard_fqdn(g_husarnet_client, fqdn);
    
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(husarnet_set_dashboard_fqdn_obj, husarnet_mp_set_dashboard_fqdn);

// Python: husarnet.list_peers() -> list of (hostname, ip) tuples
// Returns list of peers connected to the network
// Note: This requires the C++ API, we'll implement a basic version
static mp_obj_t husarnet_mp_list_peers(void) {
    if (!g_husarnet_initialized || g_husarnet_client == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, 
            MP_ERROR_TEXT("Husarnet not initialized"));
    }
    
    if (!husarnet_is_joined(g_husarnet_client)) {
        // Return empty list if not connected
        return mp_obj_new_list(0, NULL);
    }
    
    // Note: The C API doesn't expose listPeers(), but the C++ HusarnetClient does.
    // For now, we return an empty list. In a future update, we can add a C wrapper
    // function in user_interface.h/cpp for this functionality.
    // 
    // The peer list is typically obtained from the ConfigStorage host table,
    // which requires C++ access to the HusarnetManager.
    
    ESP_LOGW(TAG, "list_peers() not fully implemented - returning empty list");
    return mp_obj_new_list(0, NULL);
}
static MP_DEFINE_CONST_FUN_OBJ_0(husarnet_list_peers_obj, husarnet_mp_list_peers);

// Python: husarnet.status() -> dict
// Returns a dictionary with VPN status information
static mp_obj_t husarnet_mp_status(void) {
    mp_obj_t dict = mp_obj_new_dict(4);
    
    // initialized
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_initialized),
        g_husarnet_initialized ? mp_const_true : mp_const_false);
    
    // joined
    bool joined = g_husarnet_initialized && g_husarnet_client != NULL && 
                  husarnet_is_joined(g_husarnet_client);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_joined),
        joined ? mp_const_true : mp_const_false);
    
    // ip
    if (joined) {
        char ip_buffer[HUSARNET_IP_STR_LEN];
        memset(ip_buffer, 0, sizeof(ip_buffer));
        if (husarnet_get_ip_address(g_husarnet_client, ip_buffer, sizeof(ip_buffer))) {
            mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ip),
                mp_obj_new_str(ip_buffer, strlen(ip_buffer)));
        } else {
            mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ip), mp_const_none);
        }
    } else {
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ip), mp_const_none);
    }
    
    // available (module is compiled in)
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_available), mp_const_true);
    
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(husarnet_status_obj, husarnet_mp_status);

// Module globals table
static const mp_rom_map_elem_t husarnet_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_husarnet) },
    
    // Core functions
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&husarnet_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_join), MP_ROM_PTR(&husarnet_join_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_joined), MP_ROM_PTR(&husarnet_is_joined_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_ip), MP_ROM_PTR(&husarnet_get_ip_obj) },
    { MP_ROM_QSTR(MP_QSTR_list_peers), MP_ROM_PTR(&husarnet_list_peers_obj) },
    { MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&husarnet_status_obj) },
    
    // Advanced configuration
    { MP_ROM_QSTR(MP_QSTR_set_dashboard_fqdn), MP_ROM_PTR(&husarnet_set_dashboard_fqdn_obj) },
};
static MP_DEFINE_CONST_DICT(husarnet_module_globals, husarnet_module_globals_table);

// Define module
const mp_obj_module_t husarnet_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&husarnet_module_globals,
};

// Register the module
MP_REGISTER_MODULE(MP_QSTR_husarnet, husarnet_module);
