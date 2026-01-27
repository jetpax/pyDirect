/*
 * modwebrepl_binary.c - Unified WebREPL Binary Protocol Module
 *
 * This implements the WebREPL Binary Protocol (WBP) with pluggable transports:
 * - WebSocket (wss://) via ESP HTTP Server
 * - WebRTC DataChannel via libdatachannel
 *
 * Python API:
 *   webrepl_binary.start(password, path)    - Start WebSocket transport
 *   webrepl_binary.start_rtc(peer)          - Start WebRTC transport
 *   webrepl_binary.stop()                   - Stop active transport
 *   webrepl_binary.running()                - Check if any transport active
 *   webrepl_binary.logHandler               - Logging handler class
 *   webrepl_binary.notify(json)             - Send INFO event
 *   webrepl_binary.log(msg, level, source)  - Send LOG event
 *
 * Copyright (c) 2025-2026 Jonathan E. Peace
 * SPDX-License-Identifier: MIT
 */

#include "wbp_common.h"

#include "py/runtime.h"
#include "py/obj.h"
#include "py/mphal.h"
#include "py/objstr.h"
#include "py/repl.h"
#include "py/compile.h"
#include "extmod/vfs.h"

// ESP HTTP Server (for httpd_handle_t)
#include "esp_http_server.h"
#include "esp_log.h"

// External wsserver C API
extern bool wsserver_register_c_callback(int event_type, void *callback_func);
extern httpd_handle_t wsserver_get_handle(void);
extern int wsserver_get_client_sockfd(int client_id);
extern bool wsserver_send_to_client(int client_id, const uint8_t *data, size_t len, bool is_binary);
extern bool wsserver_is_running(void);
extern bool wsserver_start_c(const char *path, int ping_interval, int ping_timeout);

// wsserver event types
#define WSSERVER_EVENT_CONNECT         0
#define WSSERVER_EVENT_DISCONNECT      1
#define WSSERVER_EVENT_MESSAGE         2
#define WSSERVER_EVENT_PREQUEUE_FILTER 3

// WebRTC peer C API (available when usermod_webrtc is linked)
#include "modwebrtc.h"


#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "WBP_BINARY";

// MicroPython 1.27 uses 'static' instead of 'STATIC' macro
#ifndef STATIC
#define STATIC static
#endif


//=============================================================================
// Transport State
//=============================================================================

typedef enum {
    TRANSPORT_NONE = 0,
    TRANSPORT_WEBSOCKET,
    TRANSPORT_WEBRTC
} wbp_transport_type_t;

static wbp_transport_type_t g_active_transport_type = TRANSPORT_NONE;
static bool g_wbp_authenticated = false;  // Unified auth state for active transport

//=============================================================================
// WebSocket Transport State
//=============================================================================

// Single client - only one WebSocket connection at a time (one VM = one client)
static int g_ws_client_id = -1;           // Active client ID, -1 if none
static uint8_t g_ws_current_channel = 1;  // Current channel (TRM=1)
static char *g_ws_password = NULL;
static char *g_ws_path = NULL;
static httpd_handle_t g_ws_server = NULL;
static bool g_ws_running = false;
static mp_obj_t g_wbp_auth_callback = MP_OBJ_NULL;  // Python callback for auth events

//=============================================================================
// WebRTC Transport State
//=============================================================================

static void *g_rtc_peer_handle = NULL;
static bool g_rtc_data_channel_open = false;
static bool g_rtc_running = false;
// Note: Uses g_wbp_authenticated for auth state (unified)

//=============================================================================
// WebSocket Transport Implementation
//=============================================================================

static bool ws_send(const uint8_t *data, size_t len, void *ctx) {
    (void)ctx;
    if (g_ws_client_id < 0) return false;
    return wsserver_send_to_client(g_ws_client_id, data, len, true);  // true = binary
}


static bool ws_is_connected(void *ctx) {
    (void)ctx;
    return g_ws_client_id >= 0;
}

static wbp_transport_t ws_transport = {
    .send = ws_send,
    .is_connected = ws_is_connected,
    .context = NULL
};

//=============================================================================
// WebRTC Transport Implementation
//=============================================================================

static bool rtc_send(const uint8_t *data, size_t len, void *ctx) {
    (void)ctx;
    if (!g_rtc_peer_handle || !g_rtc_data_channel_open) return false;
    return webrtc_peer_send_raw((esp_peer_handle_t)g_rtc_peer_handle, data, len) == 0;
}

static bool rtc_is_connected(void *ctx) {
    (void)ctx;
    return g_rtc_peer_handle && g_rtc_data_channel_open;
}

static wbp_transport_t rtc_transport = {
    .send = rtc_send,
    .is_connected = rtc_is_connected,
    .context = NULL
};

// Note: Single-client model - no client management functions needed

//=============================================================================
// WebSocket Message Handler
//=============================================================================

static void handle_ws_channel_message(int client_id, uint8_t channel, CborValue *array) {
    // Extract opcode
    CborValue opcode_val;
    cbor_value_enter_container(array, &opcode_val);
    cbor_value_advance(&opcode_val);  // Skip channel
    
    if (!cbor_value_is_unsigned_integer(&opcode_val)) {
        ESP_LOGE(TAG, "Invalid opcode type");
        return;
    }
    
    uint64_t opcode;
    cbor_value_get_uint64(&opcode_val, &opcode);
    cbor_value_advance(&opcode_val);
    
    ESP_LOGD(TAG, "Channel %d opcode %d", channel, (int)opcode);
    
    switch (opcode) {
        case WBP_OP_EXE: {
            // [ch, 0, code, ?format, ?id]
            if (!cbor_value_is_valid(&opcode_val)) {
                ESP_LOGE(TAG, "Missing code in EXE");
                return;
            }
            
            uint8_t *code_data = NULL;
            size_t code_len = 0;
            
            // Handle text or byte string
            if (cbor_value_is_text_string(&opcode_val)) {
                cbor_value_dup_text_string(&opcode_val, (char**)&code_data, &code_len, NULL);
                cbor_value_advance(&opcode_val);
            } else if (cbor_value_is_byte_string(&opcode_val) || cbor_value_is_tag(&opcode_val)) {
                cbor_extract_byte_data(&opcode_val, &code_data, &code_len);
                cbor_value_advance(&opcode_val);
            } else {
                ESP_LOGE(TAG, "Invalid code type");
                return;
            }
            
            // Optional format
            uint8_t format = WBP_FMT_PY;
            if (cbor_value_is_valid(&opcode_val) && cbor_value_is_unsigned_integer(&opcode_val)) {
                uint64_t fmt;
                cbor_value_get_uint64(&opcode_val, &fmt);
                format = (uint8_t)fmt;
                cbor_value_advance(&opcode_val);
            }
            
            // Optional ID - use separate variable for length!
            char *id = NULL;
            size_t id_len = 0;
            if (cbor_value_is_valid(&opcode_val) && cbor_value_is_text_string(&opcode_val)) {
                cbor_value_dup_text_string(&opcode_val, &id, &id_len, NULL);
            }
            
            ESP_LOGI(TAG, "EXE: ch=%d code_len=%d id=%s", channel, (int)code_len, id ? id : "(null)");
            
            // Check for tab completion request (code ends with \t)
            bool is_bytecode = (format == WBP_FMT_MPY);
            if (!is_bytecode && code_len > 0 && code_data[code_len - 1] == '\t') {
                // Tab completion request - collect and send completions
                ESP_LOGI(TAG, "Tab completion request: '%.*s'", (int)(code_len - 1), code_data);
                
                completion_collector_t collector;
                completion_collector_init(&collector);
                
                wbp_collect_completions((const char *)code_data, code_len - 1, &collector);
                wbp_send_completions(channel, &collector);
                
                completion_collector_free(&collector);
                free(code_data);
                if (id) free(id);
                break;
            }
            
            // Queue execution for all channels
            wbp_queue_message(WBP_MSG_RAW_EXEC, channel, (const char *)code_data, code_len, 
                             id, is_bytecode);
            
            free(code_data);
            if (id) free(id);
            break;
        }
        
        case WBP_OP_INT: {
            // Keyboard interrupt
            ESP_LOGI(TAG, "Interrupt requested");
            mp_sched_keyboard_interrupt();
            break;
        }
        
        case WBP_OP_RST: {
            // Reset - mode in next field
            uint8_t mode = 0;
            if (cbor_value_is_valid(&opcode_val) && cbor_value_is_unsigned_integer(&opcode_val)) {
                uint64_t m;
                cbor_value_get_uint64(&opcode_val, &m);
                mode = (uint8_t)m;
            }
            wbp_queue_message(WBP_MSG_RESET, mode, NULL, 0, NULL, false);
            break;
        }
        
        default:
            ESP_LOGW(TAG, "Unknown opcode %d on channel %d", (int)opcode, channel);
            break;
    }
}

static void handle_ws_file_message(CborValue *array) {
    // Enter array
    CborValue opcode_val;
    cbor_value_enter_container(array, &opcode_val);
    cbor_value_advance(&opcode_val);  // Skip channel (23)
    
    if (!cbor_value_is_unsigned_integer(&opcode_val)) {
        ESP_LOGE(TAG, "Invalid file opcode type");
        return;
    }
    
    uint64_t opcode;
    cbor_value_get_uint64(&opcode_val, &opcode);
    cbor_value_advance(&opcode_val);
    
    switch (opcode) {
        case WBP_FILE_WRQ: {
            // [23, 2, path, ?tsize, ?blksize]
            char *path = NULL;
            size_t path_len;
            if (!cbor_value_is_text_string(&opcode_val)) {
                wbp_send_file_error(WBP_ERR_ILLEGAL_OP, "Missing path");
                return;
            }
            cbor_value_dup_text_string(&opcode_val, &path, &path_len, NULL);
            cbor_value_advance(&opcode_val);
            
            size_t tsize = 0, blksize = 0;
            if (cbor_value_is_valid(&opcode_val) && cbor_value_is_unsigned_integer(&opcode_val)) {
                uint64_t t;
                cbor_value_get_uint64(&opcode_val, &t);
                tsize = (size_t)t;
                cbor_value_advance(&opcode_val);
            }
            if (cbor_value_is_valid(&opcode_val) && cbor_value_is_unsigned_integer(&opcode_val)) {
                uint64_t b;
                cbor_value_get_uint64(&opcode_val, &b);
                blksize = (size_t)b;
            }
            
            wbp_handle_wrq(path, tsize, blksize);
            free(path);
            break;
        }
        
        case WBP_FILE_RRQ: {
            // [23, 1, path, ?blksize]
            char *path = NULL;
            size_t path_len;
            if (!cbor_value_is_text_string(&opcode_val)) {
                wbp_send_file_error(WBP_ERR_ILLEGAL_OP, "Missing path");
                return;
            }
            cbor_value_dup_text_string(&opcode_val, &path, &path_len, NULL);
            cbor_value_advance(&opcode_val);
            
            size_t blksize = 0;
            if (cbor_value_is_valid(&opcode_val) && cbor_value_is_unsigned_integer(&opcode_val)) {
                uint64_t b;
                cbor_value_get_uint64(&opcode_val, &b);
                blksize = (size_t)b;
            }
            
            wbp_handle_rrq(path, blksize);
            free(path);
            break;
        }
        
        case WBP_FILE_DATA: {
            // [23, 3, block#, data]
            if (!cbor_value_is_unsigned_integer(&opcode_val)) {
                wbp_send_file_error(WBP_ERR_ILLEGAL_OP, "Missing block number");
                return;
            }
            uint64_t block_num;
            cbor_value_get_uint64(&opcode_val, &block_num);
            cbor_value_advance(&opcode_val);
            
            uint8_t *data = NULL;
            size_t data_len = 0;
            if (cbor_extract_byte_data(&opcode_val, &data, &data_len) != CborNoError) {
                wbp_send_file_error(WBP_ERR_ILLEGAL_OP, "Invalid data");
                return;
            }
            
            wbp_handle_data((uint16_t)block_num, data, data_len);
            free(data);
            break;
        }
        
        case WBP_FILE_ACK: {
            // [23, 4, block#]
            if (!cbor_value_is_unsigned_integer(&opcode_val)) {
                return;
            }
            uint64_t block_num;
            cbor_value_get_uint64(&opcode_val, &block_num);
            wbp_handle_ack((uint16_t)block_num);
            break;
        }
        
        default:
            ESP_LOGW(TAG, "Unknown file opcode %d", (int)opcode);
            break;
    }
}

static void handle_ws_auth_message(int client_id, CborValue *array) {
    // [0, 0, password]
    CborValue val;
    cbor_value_enter_container(array, &val);
    cbor_value_advance(&val);  // Skip channel (0)
    cbor_value_advance(&val);  // Skip opcode (0)
    
    if (!cbor_value_is_text_string(&val)) {
        wbp_send_auth_fail("Invalid password format");
        return;
    }
    
    char *password = NULL;
    size_t password_len;
    cbor_value_dup_text_string(&val, &password, &password_len, NULL);
    
    // Check password
    if (g_ws_password && strcmp(password, g_ws_password) == 0) {
        // Set unified auth state
        g_wbp_authenticated = true;
        
        // Switch to WebSocket transport
        g_wbp_active_transport = &ws_transport;
        g_active_transport_type = TRANSPORT_WEBSOCKET;
        
        // Attach dupterm
        wbp_dupterm_attach();
        
        wbp_send_auth_ok();
        ESP_LOGI(TAG, "Client %d authenticated (WebSocket)", client_id);
        
        // Call Python auth callback if registered
        if (g_wbp_auth_callback != MP_OBJ_NULL && mp_obj_is_callable(g_wbp_auth_callback)) {
            ESP_LOGD(TAG, "Calling Python auth callback for client %d", client_id);
            nlr_buf_t nlr;
            if (nlr_push(&nlr) == 0) {
                mp_obj_t args[1] = { mp_obj_new_int(client_id) };
                mp_call_function_n_kw(g_wbp_auth_callback, 1, 0, args);
                nlr_pop();
            } else {
                mp_obj_t exc = MP_OBJ_FROM_PTR(nlr.ret_val);
                mp_printf(&mp_plat_print, "[WEBREPL] Error in auth callback:\n");
                mp_obj_print_exception(&mp_plat_print, exc);
            }
        }
    } else {
        wbp_send_auth_fail("Invalid password");
        ESP_LOGW(TAG, "Auth failed for client %d", client_id);
    }
    
    free(password);
}

//=============================================================================
// WebSocket Callbacks
//=============================================================================

static void ws_on_connect(int client_id) {
    ESP_LOGI(TAG, "WebSocket client %d connected", client_id);
    
    // Single-client model: reject if already have a client
    if (g_ws_client_id >= 0) {
        ESP_LOGW(TAG, "Rejecting client %d: already have active client %d", client_id, g_ws_client_id);
        // Note: wsserver should handle rejection, but log for visibility
        return;
    }
    
    g_ws_client_id = client_id;
    g_ws_current_channel = WBP_CH_TRM;
}

static void ws_on_disconnect(int client_id) {
    ESP_LOGI(TAG, "WebSocket client %d disconnected", client_id);
    
    if (g_ws_client_id == client_id) {
        g_ws_client_id = -1;
        g_wbp_authenticated = false;
        
        if (g_active_transport_type == TRANSPORT_WEBSOCKET) {
            wbp_dupterm_detach();
            g_wbp_active_transport = NULL;
            g_active_transport_type = TRANSPORT_NONE;
        }
    }
}

static void ws_on_message(int client_id, const uint8_t *data, size_t len, bool is_binary) {
    ESP_LOGI(TAG, "WS message: client=%d, len=%d, binary=%d", client_id, (int)len, is_binary);
    
    if (!is_binary) return;  // Only handle binary WBP messages
    
    // Reject messages from non-active client
    if (client_id != g_ws_client_id) {
        ESP_LOGW(TAG, "Ignoring message from non-active client %d", client_id);
        return;
    }
    
    // Parse CBOR
    CborParser parser;
    CborValue root;
    if (cbor_parser_init(data, len, 0, &parser, &root) != CborNoError) {
        ESP_LOGE(TAG, "Failed to parse CBOR");
        return;
    }
    
    if (!cbor_value_is_array(&root)) {
        ESP_LOGE(TAG, "Message is not an array");
        return;
    }
    
    // Get channel number
    CborValue channel_val;
    cbor_value_enter_container(&root, &channel_val);
    
    if (!cbor_value_is_unsigned_integer(&channel_val)) {
        ESP_LOGE(TAG, "Invalid channel type");
        return;
    }
    
    uint64_t channel;
    cbor_value_get_uint64(&channel_val, &channel);
    
    // Handle based on channel
    if (channel == WBP_CH_EVENT) {
        // AUTH message
        handle_ws_auth_message(client_id, &root);
    } else if (!g_wbp_authenticated) {
        ESP_LOGW(TAG, "Unauthenticated message from client %d", client_id);
        wbp_send_auth_fail("Not authenticated");
    } else if (channel == WBP_CH_FILE) {
        handle_ws_file_message(&root);
    } else {
        handle_ws_channel_message(client_id, (uint8_t)channel, &root);
    }
}

static bool ws_msg_filter(int client_id, const uint8_t *data, size_t len, bool is_binary) {
    (void)client_id;
    (void)data;
    (void)len;
    (void)is_binary;
    
    // Pre-queue filter for urgent message handling (e.g., Ctrl+C)
    // For now, always queue all messages normally
    // Could be extended to handle interrupt signals before queuing
    return true;
}

//=============================================================================
// WebRTC Message Handler
//=============================================================================

static void handle_rtc_auth_message(CborValue *array) {
    // [0, 0, password] - Auth message format
    CborValue val;
    cbor_value_enter_container(array, &val);
    cbor_value_advance(&val);  // Skip channel (0)
    cbor_value_advance(&val);  // Skip opcode (0)
    
    if (!cbor_value_is_text_string(&val)) {
        wbp_send_auth_fail("Invalid password format");
        return;
    }
    
    char *password = NULL;
    size_t password_len;
    cbor_value_dup_text_string(&val, &password, &password_len, NULL);
    
    // Check password (using same password as WebSocket)
    if (g_ws_password && strcmp(password, g_ws_password) == 0) {
        // Set unified auth state
        g_wbp_authenticated = true;
        
        // Attach dupterm now that we're authenticated
        wbp_dupterm_attach();
        
        wbp_send_auth_ok();
        ESP_LOGI(TAG, "WebRTC authenticated");
        
        // Call Python auth callback if registered (deferred to avoid stack issues)
        // The LED state callback runs in Python layer, not here
    } else {
        wbp_send_auth_fail("Invalid password");
        ESP_LOGW(TAG, "WebRTC auth failed");
    }
    
    free(password);
}

static void handle_rtc_message(const uint8_t *data, size_t len) {
    // Parse CBOR
    CborParser parser;
    CborValue root;
    if (cbor_parser_init(data, len, 0, &parser, &root) != CborNoError) {
        ESP_LOGE(TAG, "Failed to parse CBOR");
        return;
    }
    
    if (!cbor_value_is_array(&root)) {
        ESP_LOGE(TAG, "Message is not an array");
        return;
    }
    
    // Get channel and opcode
    CborValue val;
    cbor_value_enter_container(&root, &val);
    
    if (!cbor_value_is_unsigned_integer(&val)) {
        ESP_LOGE(TAG, "Invalid channel type");
        return;
    }
    
    uint64_t channel;
    cbor_value_get_uint64(&val, &channel);
    
    // Handle auth message first (channel 0, opcode 0)
    if (channel == WBP_CH_EVENT) {
        cbor_value_advance(&val);
        uint64_t opcode = 0;
        if (cbor_value_is_unsigned_integer(&val)) {
            cbor_value_get_uint64(&val, &opcode);
        }
        
        if (opcode == WBP_EVT_AUTH) {
            handle_rtc_auth_message(&root);
            return;
        }
    }
    
    // Reject all other messages if not authenticated
    if (!g_wbp_authenticated) {
        ESP_LOGW(TAG, "Unauthenticated WebRTC message");
        wbp_send_auth_fail("Not authenticated");
        return;
    }
    
    // Authenticated - process message normally
    if (channel == WBP_CH_FILE) {
        handle_ws_file_message(&root);  // Same handler works
    } else if (channel == WBP_CH_EVENT) {
        // INFO messages from client (if any)
        ESP_LOGD(TAG, "Event message on RTC");
    } else {
        handle_ws_channel_message(-1, (uint8_t)channel, &root);
    }
}

//=============================================================================
// Python API: WebSocket Transport
//=============================================================================

// webrepl_binary.start(password, path="/wbp") - Start WebSocket transport
STATIC mp_obj_t webrepl_start(size_t n_args, const mp_obj_t *args) {
    if (g_ws_running) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("WebSocket already running"));
    }
    
    // Get password
    if (g_ws_password) {
        free(g_ws_password);
        g_ws_password = NULL;
    }
    g_ws_password = strdup(mp_obj_str_get_str(args[0]));
    
    // Get path (optional, default "/wbp")
    if (g_ws_path) {
        free(g_ws_path);
        g_ws_path = NULL;
    }
    if (n_args > 1) {
        g_ws_path = strdup(mp_obj_str_get_str(args[1]));
    } else {
        g_ws_path = strdup("/wbp");
    }
    
    // Initialize queue and ring buffers
    if (!wbp_queue_init()) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to create queue"));
    }
    if (!wbp_ring_init()) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to create ring buffer"));
    }
    if (!wbp_input_ring_init()) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to create input ring"));
    }
    
    // Start drain task
    if (!wbp_drain_task_start()) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to start drain task"));
    }
    
    // Register WebSocket callbacks with the server
    wsserver_register_c_callback(WSSERVER_EVENT_CONNECT, (void *)ws_on_connect);
    wsserver_register_c_callback(WSSERVER_EVENT_DISCONNECT, (void *)ws_on_disconnect);
    wsserver_register_c_callback(WSSERVER_EVENT_MESSAGE, (void *)ws_on_message);
    wsserver_register_c_callback(WSSERVER_EVENT_PREQUEUE_FILTER, (void *)ws_msg_filter);
    
    // Start wsserver if not already running
    if (!wsserver_is_running()) {
        if (!wsserver_start_c(g_ws_path, 20, 60)) {
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to start wsserver"));
        }
    }
    
    g_ws_running = true;
    ESP_LOGI(TAG, "WebSocket transport started on %s", g_ws_path);
    
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(webrepl_start_obj, 1, 2, webrepl_start);

//=============================================================================
// Python API: WebRTC Transport
//=============================================================================

// webrepl_binary.start_rtc(peer) - Start WebRTC transport
STATIC mp_obj_t webrepl_start_rtc(mp_obj_t peer_obj) {
    if (g_rtc_running) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("WebRTC already running"));
    }
    
    // Get peer handle from Python object
    g_rtc_peer_handle = (void *)webrtc_peer_get_handle(peer_obj);
    if (!g_rtc_peer_handle) {
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("Invalid peer object"));
    }
    
    // Initialize queue and ring buffers
    if (!wbp_queue_init()) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to create queue"));
    }
    if (!wbp_ring_init()) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to create ring buffer"));
    }
    if (!wbp_input_ring_init()) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to create input ring"));
    }
    
    // Start drain task
    if (!wbp_drain_task_start()) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to start drain task"));
    }
    
    // Note: Data arrives via on_data() Python callback - no C-level callback registration
    // The Python layer calls webrepl_binary.on_data(bytes) when data arrives
    
    // Switch to WebRTC transport (dupterm attach is deferred until after auth)
    // (MUST be after ring buffer and drain task are initialized)
    g_wbp_active_transport = &rtc_transport;
    g_active_transport_type = TRANSPORT_WEBRTC;
    g_wbp_authenticated = false;  // Reset unified auth state - client must authenticate
    
    g_rtc_running = true;
    ESP_LOGI(TAG, "WebRTC transport started (awaiting auth)");
    
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(webrepl_start_rtc_obj, webrepl_start_rtc);

// webrepl_binary.update_channel_state(open) - Called when DataChannel opens/closes
STATIC mp_obj_t webrepl_update_channel_state(mp_obj_t open_obj) {
    bool open = mp_obj_is_true(open_obj);
    
    g_rtc_data_channel_open = open;
    
    if (open) {
        // Note: Transport and dupterm attachment is handled in start_rtc()
        // This function only updates the channel state flag
        ESP_LOGI(TAG, "WebRTC DataChannel opened");
    } else {
        if (g_active_transport_type == TRANSPORT_WEBRTC) {
            wbp_dupterm_detach();
            g_wbp_active_transport = NULL;
            g_active_transport_type = TRANSPORT_NONE;
            g_wbp_authenticated = false;  // Reset unified auth state for next connection
        }
        ESP_LOGI(TAG, "WebRTC DataChannel closed");
    }
    
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(webrepl_update_channel_state_obj, webrepl_update_channel_state);

// webrepl_binary.on_data(data) - Called when data arrives on DataChannel
STATIC mp_obj_t webrepl_on_data(mp_obj_t data_obj) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_obj, &bufinfo, MP_BUFFER_READ);
    
    handle_rtc_message(bufinfo.buf, bufinfo.len);
    
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(webrepl_on_data_obj, webrepl_on_data);

//=============================================================================
// Python API: Common
//=============================================================================

// webrepl_binary.stop() - Stop active transport
STATIC mp_obj_t webrepl_stop(void) {
    // Detach dupterm
    if (g_active_transport_type != TRANSPORT_NONE) {
        wbp_dupterm_detach();
    }
    
    // Stop drain task
    wbp_drain_task_stop();
    
    // Deinit ring buffers
    wbp_ring_deinit();
    wbp_input_ring_deinit();
    
    // Clear transport
    g_wbp_active_transport = NULL;
    g_active_transport_type = TRANSPORT_NONE;
    g_wbp_authenticated = false;  // Reset unified auth state
    
    // WebSocket cleanup
    if (g_ws_running) {
        g_ws_client_id = -1;
        g_ws_running = false;
    }
    
    // WebRTC cleanup
    if (g_rtc_running) {
        g_rtc_peer_handle = NULL;
        g_rtc_data_channel_open = false;
        g_rtc_running = false;
    }
    
    // Close any file transfer
    wbp_close_transfer();
    
    ESP_LOGI(TAG, "WebREPL stopped");
    
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(webrepl_stop_obj, webrepl_stop);

// webrepl_binary.running() - Check if any transport is active
STATIC mp_obj_t webrepl_running(void) {
    return mp_obj_new_bool(g_active_transport_type != TRANSPORT_NONE);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(webrepl_running_obj, webrepl_running);

// webrepl_binary.process() - Process pending messages
STATIC mp_obj_t webrepl_process(void) {
    return wbp_process_queue();
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(webrepl_process_obj, webrepl_process);

// webrepl_binary.notify(json_str) - Send INFO event
STATIC mp_obj_t webrepl_notify(mp_obj_t json_obj) {
    wbp_send_info_json(json_obj);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(webrepl_notify_obj, webrepl_notify);

// webrepl_binary.log(message, level=1, source=None) - Send LOG event
STATIC mp_obj_t webrepl_log(size_t n_args, const mp_obj_t *args) {
    size_t msg_len;
    const char *msg = mp_obj_str_get_data(args[0], &msg_len);
    
    uint8_t level = 1;  // INFO
    if (n_args > 1) {
        level = wbp_map_log_level(mp_obj_get_int(args[1]));
    }
    
    const char *source = NULL;
    if (n_args > 2 && args[2] != mp_const_none) {
        source = mp_obj_str_get_str(args[2]);
    }
    
    int64_t timestamp = mp_hal_ticks_ms() / 1000;
    
    wbp_send_log(level, (const uint8_t *)msg, msg_len, timestamp, source);
    
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(webrepl_log_obj, 1, 3, webrepl_log);

// webrepl_binary.transport() - Get current transport type as string
STATIC mp_obj_t webrepl_transport(void) {
    switch (g_active_transport_type) {
        case TRANSPORT_WEBSOCKET:
            return mp_obj_new_str("websocket", 9);
        case TRANSPORT_WEBRTC:
            return mp_obj_new_str("webrtc", 6);
        default:
            return mp_const_none;
    }
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(webrepl_transport_obj, webrepl_transport);

//=============================================================================
// Python API: Authentication Callback
//=============================================================================

// webrepl_binary.on_auth(callback) - Register callback for auth events
STATIC mp_obj_t webrepl_on_auth(mp_obj_t callback_obj) {
    if (!mp_obj_is_callable(callback_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("on_auth() argument must be callable"));
    }
    g_wbp_auth_callback = callback_obj;
    ESP_LOGD(TAG, "Auth callback registered");
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(webrepl_on_auth_obj, webrepl_on_auth);

//=============================================================================
// Module Definition
//=============================================================================

static const mp_rom_map_elem_t webrepl_binary_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_webrepl_binary) },
    
    // WebSocket transport
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&webrepl_start_obj) },
    
    // WebRTC transport
    { MP_ROM_QSTR(MP_QSTR_start_rtc), MP_ROM_PTR(&webrepl_start_rtc_obj) },
    { MP_ROM_QSTR(MP_QSTR_update_channel_state), MP_ROM_PTR(&webrepl_update_channel_state_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_data), MP_ROM_PTR(&webrepl_on_data_obj) },
    
    // Common
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&webrepl_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_running), MP_ROM_PTR(&webrepl_running_obj) },
    { MP_ROM_QSTR(MP_QSTR_process), MP_ROM_PTR(&webrepl_process_obj) },
    { MP_ROM_QSTR(MP_QSTR_process_queue), MP_ROM_PTR(&webrepl_process_obj) },  // Backwards compat alias
    { MP_ROM_QSTR(MP_QSTR_notify), MP_ROM_PTR(&webrepl_notify_obj) },
    { MP_ROM_QSTR(MP_QSTR_log), MP_ROM_PTR(&webrepl_log_obj) },
    { MP_ROM_QSTR(MP_QSTR_transport), MP_ROM_PTR(&webrepl_transport_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_auth), MP_ROM_PTR(&webrepl_on_auth_obj) },
    
    // Log handler class
    { MP_ROM_QSTR(MP_QSTR_logHandler), MP_ROM_PTR(&wbp_log_handler_type) },
};
static MP_DEFINE_CONST_DICT(webrepl_binary_module_globals, webrepl_binary_module_globals_table);

const mp_obj_module_t mp_module_webrepl_binary = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&webrepl_binary_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_webrepl_binary, mp_module_webrepl_binary);
