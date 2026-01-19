/*
 * modusbmodem.c - MicroPython module for USB CDC modem communication
 *
 * Implementation using iot_usbh_modem V2.0 for AT command and PPP functionality
 *
 * Copyright (c) 2025 Jonathan Peace
 * SPDX-License-Identifier: MIT
 */

#include "py/runtime.h"
#include "py/obj.h"
#include "py/mphal.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "ping/ping_sock.h"

// V2.0 headers
#include "iot_usbh_modem.h"
#include "modem_at_parser.h"
#include "at_3gpp_ts_27_007.h"

static const char *TAG = "USBMODEM";

// Global state
static bool g_modem_initialized = false;
static at_handle_t g_at_parser = NULL;
static bool g_ppp_connected = false;
static bool g_ppp_connecting = false;  // True when auto-connect enabled but not yet connected
static esp_ip4_addr_t g_ppp_ip = {0};

// Runtime config values (override compile-time defaults if set)
static char g_runtime_apn[64] = "";
static char g_runtime_sim_pin[16] = "";
static char g_runtime_username[64] = "";
static bool g_runtime_auto_init = true;

// Response buffer for AT commands
static char g_at_response_buffer[2048];
static size_t g_at_response_len = 0;

// Ping state
static volatile bool g_ping_done = false;
static volatile bool g_ping_success = false;
static volatile uint32_t g_ping_time_ms = 0;

// AT command response handler - V2.0 signature returns bool
static bool at_response_handler(at_handle_t at_handle, const char *line) {
    if (line == NULL) {
        return false;
    }
    
    // Collect response lines
    size_t line_len = strlen(line);
    if (g_at_response_len + line_len + 1 < sizeof(g_at_response_buffer)) {
        memcpy(g_at_response_buffer + g_at_response_len, line, line_len);
        g_at_response_len += line_len;
        g_at_response_buffer[g_at_response_len++] = '\n';
        g_at_response_buffer[g_at_response_len] = '\0';
    }
    
    // Check for final response markers
    if (strstr(line, "OK") != NULL) return true;
    if (strstr(line, "ERROR") != NULL) return true;
    if (strstr(line, "+CME ERROR") != NULL) return true;
    if (strstr(line, "+CMS ERROR") != NULL) return true;
    
    return false;  // Continue collecting lines
}

// Ping callbacks
static void ping_success_cb(esp_ping_handle_t hdl, void *args) {
    uint32_t elapsed_time;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time, sizeof(elapsed_time));
    g_ping_time_ms = elapsed_time;
    g_ping_success = true;
}

static void ping_timeout_cb(esp_ping_handle_t hdl, void *args) {
    g_ping_success = false;
}

static void ping_end_cb(esp_ping_handle_t hdl, void *args) {
    g_ping_done = true;
}

// Helper: Convert RSSI to dBm (3GPP TS 27.007)
static int rssi_to_dbm(int rssi) {
    if (rssi == 99 || rssi == 199) {
        return -999; // Unknown
    } else if (rssi >= 0 && rssi <= 31) {
        return -113 + (rssi * 2);
    } else if (rssi >= 100 && rssi <= 191) {
        return -116 + (rssi - 100);
    }
    return -999; // Unknown
}

// IP event handler - tracks PPP connection state
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data) {
    if (event_base == IP_EVENT && event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        g_ppp_ip = event->ip_info.ip;
        g_ppp_connected = true;
        g_ppp_connecting = false;
        ESP_LOGI(TAG, "PPP Got IP: " IPSTR, IP2STR(&g_ppp_ip));
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "PPP Lost IP");
        g_ppp_connected = false;
        g_ppp_connecting = false;
        g_ppp_ip.addr = 0;
    }
}

// Python: usbmodem.set_config(apn="", sim_pin="", username="", auto_init_modem=True)
static mp_obj_t usbmodem_set_config(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_apn, ARG_sim_pin, ARG_username, ARG_auto_init_modem };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_apn, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_sim_pin, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_username, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_auto_init_modem, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    };
    
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    
    if (args[ARG_apn].u_obj != MP_OBJ_NULL) {
        const char *apn = mp_obj_str_get_str(args[ARG_apn].u_obj);
        if (strlen(apn) > 0) {
            strncpy(g_runtime_apn, apn, sizeof(g_runtime_apn) - 1);
            g_runtime_apn[sizeof(g_runtime_apn) - 1] = '\0';
        }
    }
    
    if (args[ARG_sim_pin].u_obj != MP_OBJ_NULL) {
        const char *pin = mp_obj_str_get_str(args[ARG_sim_pin].u_obj);
        if (strlen(pin) > 0) {
            strncpy(g_runtime_sim_pin, pin, sizeof(g_runtime_sim_pin) - 1);
            g_runtime_sim_pin[sizeof(g_runtime_sim_pin) - 1] = '\0';
        }
    }
    
    if (args[ARG_username].u_obj != MP_OBJ_NULL) {
        const char *username = mp_obj_str_get_str(args[ARG_username].u_obj);
        if (strlen(username) > 0) {
            strncpy(g_runtime_username, username, sizeof(g_runtime_username) - 1);
            g_runtime_username[sizeof(g_runtime_username) - 1] = '\0';
        }
    }
    
    if (args[ARG_auto_init_modem].u_obj != MP_OBJ_NULL) {
        g_runtime_auto_init = mp_obj_is_true(args[ARG_auto_init_modem].u_obj);
    }
    
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(usbmodem_set_config_obj, 0, usbmodem_set_config);

// Modem ID list for SIM7600 (dual interface)
static const usb_modem_id_t modem_id_list[] = {
    {
        .match_id = {
            .match_flags = USB_DEVICE_ID_MATCH_VID_PID,
            .idVendor = 0x1E0E,   // SIMCOM VID
            .idProduct = 0x9001,  // SIM7600 PID
        },
        .modem_itf_num = 0x03,  // PPP data interface
        .at_itf_num = 0x02,     // Secondary AT port
        .name = "SIM7600",
    },
    // Terminator
    {
        .match_id = {0},
        .modem_itf_num = -1,
        .at_itf_num = -1,
        .name = NULL,
    }
};

// Python: usbmodem.init()
static mp_obj_t usbmodem_init(void) {
    if (g_modem_initialized) {
        ESP_LOGW(TAG, "Modem already initialized");
        return mp_const_true;
    }
    
    ESP_LOGI(TAG, "Initializing USB modem (V2.0 API)");
    
    // Register for IP events to track PPP connection
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, ip_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP, ip_event_handler, NULL);
    
    // Configure and install modem - V2.0 API
    usbh_modem_config_t config = {
        .modem_id_list = modem_id_list,
        .at_tx_buffer_size = 1024,
        .at_rx_buffer_size = 2048,
    };
    
    esp_err_t err = usbh_modem_install(&config);
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_OSError, 
            MP_ERROR_TEXT("Failed to install modem: %d"), err);
    }
    
    // Get AT parser handle for AT commands
    g_at_parser = usbh_modem_get_atparser();
    if (g_at_parser == NULL) {
        ESP_LOGW(TAG, "AT parser not available yet (modem may still be connecting)");
    }
    
    // Set APN if runtime value provided
    if (strlen(g_runtime_apn) > 0 && g_at_parser != NULL) {
        esp_modem_at_pdp_t pdp = {
            .cid = 1,
            .type = "IP",
            .apn = g_runtime_apn,
        };
        at_cmd_set_pdp_context(g_at_parser, &pdp);
        ESP_LOGI(TAG, "APN set to: %s", g_runtime_apn);
    }
    
    // Disable auto-connect by default (user controls when to connect)
    usbh_modem_ppp_auto_connect(false);
    
    g_modem_initialized = true;
    g_ppp_connected = false;
    g_ppp_connecting = false;
    g_ppp_ip.addr = 0;
    ESP_LOGI(TAG, "USB modem initialized successfully");
    
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbmodem_init_obj, usbmodem_init);

// Python: usbmodem.deinit()
static mp_obj_t usbmodem_deinit(void) {
    if (!g_modem_initialized) {
        return mp_const_none;
    }
    
    ESP_LOGI(TAG, "Deinitializing USB modem");
    
    // Disconnect PPP if connected or connecting
    if (g_ppp_connected || g_ppp_connecting) {
        usbh_modem_ppp_auto_connect(false);
        usbh_modem_ppp_stop();
        g_ppp_connected = false;
        g_ppp_connecting = false;
    }
    
    esp_err_t err = usbh_modem_uninstall();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to uninstall modem: %d", err);
    }
    
    g_at_parser = NULL;
    g_modem_initialized = false;
    
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbmodem_deinit_obj, usbmodem_deinit);

// Python: usbmodem.connected()
static mp_obj_t usbmodem_connected(void) {
    if (!g_modem_initialized) {
        return mp_const_false;
    }
    
    // Check if AT parser handle is available (means USB modem is connected)
    at_handle_t parser = usbh_modem_get_atparser();
    return parser != NULL ? mp_const_true : mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbmodem_connected_obj, usbmodem_connected);

// Python: response = usbmodem.send_at("AT")
static mp_obj_t usbmodem_send_at(mp_obj_t cmd_obj) {
    if (!g_modem_initialized) {
        mp_raise_msg(&mp_type_RuntimeError, 
            MP_ERROR_TEXT("Modem not initialized. Call usbmodem.init() first"));
    }
    
    at_handle_t parser = usbh_modem_get_atparser();
    if (parser == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, 
            MP_ERROR_TEXT("Modem not connected. Check USB connection."));
    }
    
    const char *cmd = mp_obj_str_get_str(cmd_obj);
    
    // Clear response buffer
    g_at_response_len = 0;
    memset(g_at_response_buffer, 0, sizeof(g_at_response_buffer));
    
    // Send command using V2.0 API
    esp_err_t err = modem_at_send_command(
        parser, cmd, CONFIG_MODEM_COMMAND_TIMEOUT_DEFAULT,
        at_response_handler, NULL
    );
    
    if (err != ESP_OK) {
        if (g_ppp_connected || g_ppp_connecting) {
            mp_raise_msg_varg(&mp_type_OSError, 
                MP_ERROR_TEXT("AT command failed (PPP active): %d. Secondary AT port may not be working."), 
                err);
        } else {
            mp_raise_msg_varg(&mp_type_OSError, 
                MP_ERROR_TEXT("AT command failed: %d"), err);
        }
    }
    
    return mp_obj_new_str(g_at_response_buffer, g_at_response_len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(usbmodem_send_at_obj, usbmodem_send_at);

// ============================================================================
// PPP Functions
// ============================================================================

// Python: usbmodem.ppp_connect()
// Non-blocking: enables auto-connect and returns immediately
// Use ppp_status() to poll for connection state
static mp_obj_t usbmodem_ppp_connect(void) {
    if (!g_modem_initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Modem not initialized"));
    }
    
    if (g_ppp_connected) {
        ESP_LOGW(TAG, "PPP already connected");
        return mp_const_true;
    }
    
    ESP_LOGI(TAG, "Enabling PPP auto-connect (non-blocking)...");
    
    // Mark as connecting
    g_ppp_connecting = true;
    
    // Enable PPP auto-connect - V2.0 API
    usbh_modem_ppp_auto_connect(true);
    
    // Return immediately - caller should poll ppp_status() for connection state
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbmodem_ppp_connect_obj, usbmodem_ppp_connect);

// Python: usbmodem.ppp_disconnect()
static mp_obj_t usbmodem_ppp_disconnect(void) {
    if (!g_modem_initialized) {
        return mp_const_none;
    }
    
    if (!g_ppp_connected && !g_ppp_connecting) {
        ESP_LOGW(TAG, "PPP not connected");
        return mp_const_none;
    }
    
    ESP_LOGI(TAG, "Disconnecting PPP...");
    usbh_modem_ppp_auto_connect(false);
    usbh_modem_ppp_stop();
    g_ppp_connected = false;
    g_ppp_connecting = false;
    
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbmodem_ppp_disconnect_obj, usbmodem_ppp_disconnect);

// Python: usbmodem.ppp_status() -> dict
static mp_obj_t usbmodem_ppp_status(void) {
    mp_obj_t dict = mp_obj_new_dict(3);
    
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_connected), 
        g_ppp_connected ? mp_const_true : mp_const_false);
    
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_connecting), 
        g_ppp_connecting ? mp_const_true : mp_const_false);
    
    if (g_ppp_connected && g_ppp_ip.addr != 0) {
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&g_ppp_ip));
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ip), 
            mp_obj_new_str(ip_str, strlen(ip_str)));
    } else {
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ip), mp_const_none);
    }
    
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbmodem_ppp_status_obj, usbmodem_ppp_status);

// ============================================================================
// Info/Status Functions
// ============================================================================

// Python: usbmodem.get_info() -> dict with manufacturer, model, revision, imei
static mp_obj_t usbmodem_get_info(void) {
    if (!g_modem_initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Modem not initialized"));
    }
    
    at_handle_t parser = usbh_modem_get_atparser();
    if (parser == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Modem not connected"));
    }
    
    mp_obj_t dict = mp_obj_new_dict(4);
    
    // Send ATI command
    g_at_response_len = 0;
    memset(g_at_response_buffer, 0, sizeof(g_at_response_buffer));
    esp_err_t err = modem_at_send_command(parser, "ATI", 
        CONFIG_MODEM_COMMAND_TIMEOUT_DEFAULT, at_response_handler, NULL);
    
    if (err == ESP_OK) {
        // Parse ATI response - typically:
        // Manufacturer: SIMCOM INCORPORATED
        // Model: SIMCOM_SIM7600G-H
        // Revision: SIM7600G_V2.0.2
        // IMEI: 862636053055057
        char *line = strtok(g_at_response_buffer, "\n");
        while (line != NULL) {
            if (strncmp(line, "Manufacturer:", 13) == 0) {
                char *val = line + 13;
                while (*val == ' ') val++;
                size_t len = strlen(val);
                if (len > 0 && val[len-1] == '\r') len--;
                mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_manufacturer),
                    mp_obj_new_str(val, len));
            } else if (strncmp(line, "Model:", 6) == 0) {
                char *val = line + 6;
                while (*val == ' ') val++;
                size_t len = strlen(val);
                if (len > 0 && val[len-1] == '\r') len--;
                mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_model),
                    mp_obj_new_str(val, len));
            } else if (strncmp(line, "Revision:", 9) == 0) {
                char *val = line + 9;
                while (*val == ' ') val++;
                size_t len = strlen(val);
                if (len > 0 && val[len-1] == '\r') len--;
                mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_revision),
                    mp_obj_new_str(val, len));
            } else if (strncmp(line, "IMEI:", 5) == 0) {
                char *val = line + 5;
                while (*val == ' ') val++;
                size_t len = strlen(val);
                if (len > 0 && val[len-1] == '\r') len--;
                mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_imei),
                    mp_obj_new_str(val, len));
            }
            line = strtok(NULL, "\n");
        }
    }
    
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbmodem_get_info_obj, usbmodem_get_info);

// Python: usbmodem.get_signal() -> dict with rssi, ber, dbm
static mp_obj_t usbmodem_get_signal(void) {
    if (!g_modem_initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Modem not initialized"));
    }
    
    mp_obj_t dict = mp_obj_new_dict(3);
    
    at_handle_t parser = usbh_modem_get_atparser();
    if (parser == NULL) {
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_rssi), mp_const_none);
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ber), mp_const_none);
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_dbm), mp_const_none);
        return dict;
    }
    
    // Use V2.0 API - at_cmd_get_signal_quality
    esp_modem_at_csq_t csq = {.rssi = 99, .ber = 99};
    esp_err_t err = at_cmd_get_signal_quality(parser, &csq);
    
    if (err == ESP_OK) {
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_rssi), mp_obj_new_int(csq.rssi));
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ber), mp_obj_new_int(csq.ber));
        
        int dbm = rssi_to_dbm(csq.rssi);
        if (dbm != -999) {
            mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_dbm), mp_obj_new_int(dbm));
        } else {
            mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_dbm), mp_const_none);
        }
    } else {
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_rssi), mp_const_none);
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ber), mp_const_none);
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_dbm), mp_const_none);
    }
    
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbmodem_get_signal_obj, usbmodem_get_signal);

// Python: usbmodem.get_firmware() -> str
static mp_obj_t usbmodem_get_firmware(void) {
    if (!g_modem_initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Modem not initialized"));
    }
    
    at_handle_t parser = usbh_modem_get_atparser();
    if (parser == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Modem not connected"));
    }
    
    // Send AT+CGMR command
    g_at_response_len = 0;
    memset(g_at_response_buffer, 0, sizeof(g_at_response_buffer));
    esp_err_t err = modem_at_send_command(parser, "AT+CGMR", 
        CONFIG_MODEM_COMMAND_TIMEOUT_DEFAULT, at_response_handler, NULL);
    
    if (err == ESP_OK) {
        // Parse +CGMR: version or just version
        char *cgmr = strstr(g_at_response_buffer, "+CGMR:");
        if (cgmr != NULL) {
            char *val = cgmr + 6;
            while (*val == ' ') val++;
            char *end = strchr(val, '\r');
            if (end == NULL) end = strchr(val, '\n');
            size_t len = end ? (size_t)(end - val) : strlen(val);
            return mp_obj_new_str(val, len);
        }
        // Some modems return version without +CGMR: prefix
        char *line = g_at_response_buffer;
        while (*line == '\r' || *line == '\n') line++;
        char *end = strchr(line, '\r');
        if (end == NULL) end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len > 0) {
            return mp_obj_new_str(line, len);
        }
    }
    
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbmodem_get_firmware_obj, usbmodem_get_firmware);

// ============================================================================
// Ping Function
// ============================================================================

// Python: usbmodem.ping(host, count=1, timeout=3000) -> dict
static mp_obj_t usbmodem_ping(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_host, ARG_count, ARG_timeout };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_host, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_count, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 1} },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 3000} },
    };
    
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    
    const char *host = mp_obj_str_get_str(args[ARG_host].u_obj);
    int count = args[ARG_count].u_int;
    int timeout_ms = args[ARG_timeout].u_int;
    
    // Parse IP address (host must be an IP address string like "8.8.8.8")
    ip_addr_t target_addr;
    memset(&target_addr, 0, sizeof(target_addr));
    
    if (!ipaddr_aton(host, &target_addr)) {
        mp_raise_msg_varg(&mp_type_ValueError, 
            MP_ERROR_TEXT("Invalid IP address: %s"), host);
    }
    
    // Configure ping
    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = target_addr;
    ping_config.count = count;
    ping_config.timeout_ms = timeout_ms;
    ping_config.interval_ms = 1000;
    
    esp_ping_callbacks_t cbs = {
        .on_ping_success = ping_success_cb,
        .on_ping_timeout = ping_timeout_cb,
        .on_ping_end = ping_end_cb,
        .cb_args = NULL,
    };
    
    // Reset state
    g_ping_done = false;
    g_ping_success = false;
    g_ping_time_ms = 0;
    
    // Create and start ping session
    esp_ping_handle_t ping_hdl;
    esp_err_t err = esp_ping_new_session(&ping_config, &cbs, &ping_hdl);
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("Ping failed: %d"), err);
    }
    
    esp_ping_start(ping_hdl);
    
    // Wait for ping to complete
    int wait_ms = 0;
    int max_wait = (count * (timeout_ms + 1000)) + 1000;
    while (!g_ping_done && wait_ms < max_wait) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_ms += 100;
    }
    
    // Get results
    uint32_t transmitted, received;
    esp_ping_get_profile(ping_hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(ping_hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    
    // Cleanup
    esp_ping_stop(ping_hdl);
    esp_ping_delete_session(ping_hdl);
    
    // Return result dict
    mp_obj_t dict = mp_obj_new_dict(4);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_success), 
        received > 0 ? mp_const_true : mp_const_false);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_transmitted), 
        mp_obj_new_int(transmitted));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_received), 
        mp_obj_new_int(received));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_time_ms), 
        mp_obj_new_int(g_ping_time_ms));
    
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(usbmodem_ping_obj, 1, usbmodem_ping);

// ============================================================================
// GPS Functions
// ============================================================================

// Python: usbmodem.gps_enable(mode=1)
static mp_obj_t usbmodem_gps_enable(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_mode };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_mode, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 1} },
    };
    
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    
    if (!g_modem_initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Modem not initialized"));
    }
    
    at_handle_t parser = usbh_modem_get_atparser();
    if (parser == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Modem not connected"));
    }
    
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CGPS=%ld", (long)args[ARG_mode].u_int);
    
    g_at_response_len = 0;
    memset(g_at_response_buffer, 0, sizeof(g_at_response_buffer));
    esp_err_t err = modem_at_send_command(parser, cmd, CONFIG_MODEM_COMMAND_TIMEOUT_DEFAULT, at_response_handler, NULL);
    
    if (err != ESP_OK) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to enable GPS"));
    }
    
    return mp_obj_new_str(g_at_response_buffer, g_at_response_len);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(usbmodem_gps_enable_obj, 0, usbmodem_gps_enable);

// Python: usbmodem.gps_disable()
static mp_obj_t usbmodem_gps_disable(void) {
    if (!g_modem_initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Modem not initialized"));
    }
    
    at_handle_t parser = usbh_modem_get_atparser();
    if (parser == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Modem not connected"));
    }
    
    g_at_response_len = 0;
    memset(g_at_response_buffer, 0, sizeof(g_at_response_buffer));
    esp_err_t err = modem_at_send_command(parser, "AT+CGPS=0", CONFIG_MODEM_COMMAND_TIMEOUT_DEFAULT, at_response_handler, NULL);
    
    if (err != ESP_OK) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to disable GPS"));
    }
    
    return mp_obj_new_str(g_at_response_buffer, g_at_response_len);
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbmodem_gps_disable_obj, usbmodem_gps_disable);

// Python: usbmodem.gps_info()
static mp_obj_t usbmodem_gps_info(void) {
    if (!g_modem_initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Modem not initialized"));
    }
    
    at_handle_t parser = usbh_modem_get_atparser();
    if (parser == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Modem not connected"));
    }
    
    g_at_response_len = 0;
    memset(g_at_response_buffer, 0, sizeof(g_at_response_buffer));
    esp_err_t err = modem_at_send_command(parser, "AT+CGPSINFO", CONFIG_MODEM_COMMAND_TIMEOUT_DEFAULT, at_response_handler, NULL);
    
    if (err != ESP_OK) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to get GPS info"));
    }
    
    return mp_obj_new_str(g_at_response_buffer, g_at_response_len);
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbmodem_gps_info_obj, usbmodem_gps_info);

// ============================================================================
// Module Definition
// ============================================================================

static const mp_rom_map_elem_t usbmodem_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_usbmodem) },
    
    // Core functions
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&usbmodem_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&usbmodem_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_config), MP_ROM_PTR(&usbmodem_set_config_obj) },
    { MP_ROM_QSTR(MP_QSTR_connected), MP_ROM_PTR(&usbmodem_connected_obj) },
    { MP_ROM_QSTR(MP_QSTR_send_at), MP_ROM_PTR(&usbmodem_send_at_obj) },
    
    // PPP functions
    { MP_ROM_QSTR(MP_QSTR_ppp_connect), MP_ROM_PTR(&usbmodem_ppp_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_ppp_disconnect), MP_ROM_PTR(&usbmodem_ppp_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_ppp_status), MP_ROM_PTR(&usbmodem_ppp_status_obj) },
    
    // Info/Status functions
    { MP_ROM_QSTR(MP_QSTR_get_info), MP_ROM_PTR(&usbmodem_get_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_signal), MP_ROM_PTR(&usbmodem_get_signal_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_firmware), MP_ROM_PTR(&usbmodem_get_firmware_obj) },
    
    // Network functions
    { MP_ROM_QSTR(MP_QSTR_ping), MP_ROM_PTR(&usbmodem_ping_obj) },
    
    // GPS functions
    { MP_ROM_QSTR(MP_QSTR_gps_enable), MP_ROM_PTR(&usbmodem_gps_enable_obj) },
    { MP_ROM_QSTR(MP_QSTR_gps_disable), MP_ROM_PTR(&usbmodem_gps_disable_obj) },
    { MP_ROM_QSTR(MP_QSTR_gps_info), MP_ROM_PTR(&usbmodem_gps_info_obj) },
};
static MP_DEFINE_CONST_DICT(usbmodem_module_globals, usbmodem_module_globals_table);

const mp_obj_module_t usbmodem_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&usbmodem_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_usbmodem, usbmodem_module);
