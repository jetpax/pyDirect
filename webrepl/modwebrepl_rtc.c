/*
 * modwebrepl_rtc.c - WebREPL Binary Protocol over WebRTC DataChannel
 *
 * This implements the WebREPL Binary Protocol (WBP) using WebRTC DataChannel
 * as the transport layer instead of WebSocket.
 *
 * Protocol Name: WBP (WebREPL Channelized Binary)
 * Transport: WebRTC DataChannel (via modwebrtc.c)
 * Specification: See docs/webrepl_cb_rfc.md
 *
 * Key Architecture:
 * - Reuses ring buffer + drain task pattern from modwebrepl.c
 * - Calls esp_peer_send_data() directly (C-to-C, no Python layer)
 * - Python signaling layer passes peer object to start()
 * - Dupterm attached to slot 1 (WebRTC), slot 2 is WSS, slot 0 is UART
 *
 * Copyright (c) 2025 Jonathan Peace
 * SPDX-License-Identifier: MIT
 */

#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "esp_log.h"
#include "esp_system.h"

#include "py/runtime.h"
#include "py/compile.h"
#include "py/mphal.h"
#include "py/stream.h"
#include "py/builtin.h"
#include "py/repl.h"
#include "extmod/vfs.h"
#include "extmod/misc.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// TinyCBOR library (ESP-IDF component: espressif/cbor)
#include "cbor.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

// WebRTC peer C API (from modwebrtc.c)
#include "modwebrtc.h"

// MicroPython 1.27 uses 'static' instead of 'STATIC' macro
#ifndef STATIC
#define STATIC static
#endif

static const char *TAG = "WBP_RTC";

//=============================================================================
// WebREPL Binary Protocol Constants (CBOR Arrays)
//=============================================================================

// Special Channels (first array element discriminates message type)
#define WBP_CH_EVENT  0   // System events: [0, event, ...]
#define WBP_CH_TRM    1   // Terminal REPL: [1, opcode, ...]
#define WBP_CH_M2M    2   // Machine-to-Machine: [2, opcode, ...]
#define WBP_CH_DBG    3   // Debug output: [3, opcode, ...]
#define WBP_CH_FILE   23  // File operations (TFTP-based): [23, opcode, ...]

// Channel Opcodes (Client → Server)
#define WBP_OP_EXE   0   // Execute code
#define WBP_OP_INT   1   // Interrupt
#define WBP_OP_RST   2   // Reset

// Channel Opcodes (Server → Client)
#define WBP_OP_RES   0   // Result data
#define WBP_OP_CON   1   // Continuation prompt
#define WBP_OP_PRO   2   // Progress/Status
#define WBP_OP_COM   3   // Completions

// File Operations (for channel 23) - TFTP-inspired
#define WBP_FILE_RRQ   1  // Read Request (download)
#define WBP_FILE_WRQ   2  // Write Request (upload)
#define WBP_FILE_DATA  3  // Data block
#define WBP_FILE_ACK   4  // Acknowledgment
#define WBP_FILE_ERROR 5  // Error response

// TFTP Error Codes
#define WBP_ERR_UNDEFINED    0  // Not defined
#define WBP_ERR_NOT_FOUND    1  // File not found
#define WBP_ERR_ACCESS       2  // Access violation
#define WBP_ERR_DISK_FULL    3  // Disk full
#define WBP_ERR_ILLEGAL_OP   4  // Illegal TFTP operation
#define WBP_ERR_UNKNOWN_TID  5  // Unknown transfer ID

// File transfer constants
#define WBP_DEFAULT_BLKSIZE  4096   // 4KB blocks (ESP32 flash sector)
#define WBP_MAX_BLOCK_NUM    65535  // 16-bit block number

// Execution formats
#define WBP_FMT_PY   0   // Python source code
#define WBP_FMT_MPY  1   // .mpy bytecode

// Event Types (for channel 0)
#define WBP_EVT_AUTH      0  // Authentication
#define WBP_EVT_AUTH_OK   1  // Auth success
#define WBP_EVT_AUTH_FAIL 2  // Auth failure
#define WBP_EVT_INFO      3  // Informational message (CBOR map)
#define WBP_EVT_LOG       4  // Structured log message

//=============================================================================
// Global State (WebRTC-specific)
//=============================================================================

// WebRTC peer state (no client_id, just single peer connection)
static esp_peer_handle_t g_peer_handle = NULL;
static bool g_data_channel_open = false;
static mp_obj_t g_peer_obj = MP_OBJ_NULL;  // Keep reference to prevent GC

// Output redirection state
static uint8_t g_wbp_current_channel = WBP_CH_TRM;
static char *g_wbp_current_id = NULL; // Current executing message ID
static bool g_wbp_executing = false;

// File transfer state (TFTP-inspired)
typedef struct {
    uint8_t opcode;          // WBP_FILE_RRQ or WBP_FILE_WRQ
    char path[256];
    mp_obj_t file_obj;       // MicroPython file object
    uint16_t next_block;     // Next expected/send block number
    size_t blksize;          // Block size
    size_t tsize;            // Total file size (for progress)
    size_t bytes_transferred;
    bool active;
    int64_t last_activity;   // For timeout detection
} wbp_file_transfer_t;

static wbp_file_transfer_t g_file_transfer = {
    .active = false,
    .file_obj = MP_OBJ_NULL
};

//=============================================================================
// Message Queue System (for GIL-safe execution)
//=============================================================================

#define WBP_QUEUE_SIZE 50
#define WBP_MAX_CMD_LEN 4096

typedef enum {
    WBP_MSG_RAW_EXEC,
    WBP_MSG_RESET
} wbp_msg_type_t;

typedef struct {
    wbp_msg_type_t type;
    uint8_t channel;
    char *data;
    size_t data_len;
    char *id;          // Message ID
    bool is_bytecode;  // For .mpy execution
} wbp_queue_msg_t;

static QueueHandle_t wbp_msg_queue = NULL;
static SemaphoreHandle_t wbp_queue_mutex = NULL;

//=============================================================================
// Ring Buffer for Non-Blocking Output
//=============================================================================

#define WBP_OUTPUT_RING_SIZE 32768  // 32KB ring buffer

typedef struct {
    uint8_t buffer[WBP_OUTPUT_RING_SIZE];
    volatile size_t head;  // Write position
    volatile size_t tail;  // Read position
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t data_available;
    TaskHandle_t drain_task;
    volatile bool active;
} wbp_output_ring_t;

static wbp_output_ring_t g_output_ring = {
    .head = 0,
    .tail = 0,
    .mutex = NULL,
    .data_available = NULL,
    .drain_task = NULL,
    .active = false
};

// Forward declarations
static void wbp_drain_task(void *pvParameters);
static bool wbp_ring_init(void);
static void wbp_ring_deinit(void);
static size_t wbp_ring_write(const uint8_t *data, size_t len);
static size_t wbp_ring_read(uint8_t *data, size_t max_len);
static size_t wbp_ring_available(void);

//=============================================================================
// WebREPL dupterm Stream Type
//=============================================================================

typedef struct _wbp_stream_obj_t {
    mp_obj_base_t base;
} wbp_stream_obj_t;

// Forward declarations for stream protocol
static mp_uint_t wbp_stream_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode);
static mp_uint_t wbp_stream_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode);
static mp_uint_t wbp_stream_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode);

// Stream protocol definition
static const mp_stream_p_t wbp_stream_p = {
    .read = wbp_stream_read,
    .write = wbp_stream_write,
    .ioctl = wbp_stream_ioctl,
};

// Type definition
MP_DEFINE_CONST_OBJ_TYPE(
    wbp_rtc_stream_type,
    MP_QSTR_WBPRTCStream,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    protocol, &wbp_stream_p
);

// Global stream object
static wbp_stream_obj_t *g_wbp_stream = NULL;

//=============================================================================
// Helper Functions
//=============================================================================

/**
 * Send raw CBOR data via WebRTC DataChannel
 * Returns false if send failed
 * 
 * Note: webrtc_peer_send_raw() handles copying the data internally.
 */
static bool wbp_rtc_send_cbor(const uint8_t *data, size_t len) {
    if (!g_peer_handle || !g_data_channel_open || !data || len == 0) {
        ESP_LOGD(TAG, "wbp_rtc_send_cbor: cannot send (handle=%p, open=%d, data=%p, len=%d)", 
                 g_peer_handle, g_data_channel_open, data, (int)len);
        return false;
    }
    
    ESP_LOGD(TAG, "wbp_rtc_send_cbor: sending %d bytes via WebRTC", (int)len);
    
    int err = webrtc_peer_send_raw(g_peer_handle, data, len);
    if (err != 0) {
        ESP_LOGE(TAG, "Failed to send via DataChannel: %d", err);
        return false;
    }
    
    ESP_LOGD(TAG, "wbp_rtc_send_cbor: sent successfully");
    return true;
}

//=============================================================================
// WBP Message Builders (using TinyCBOR)
//=============================================================================

/**
 * Send channel result: [ch, 0, data, ?id]
 */
static void wbp_send_result(uint8_t channel, const uint8_t *data, size_t len, const char *id) {
    ESP_LOGD(TAG, "wbp_send_result: called (ch=%d, len=%d, id=%s)", 
             channel, (int)len, id ? id : "NULL");
    
    // Calculate required size: standard overhead ~20 bytes + data length + id length + safety margin
    size_t id_len = id ? strlen(id) : 0;
    size_t buf_size = 64 + len + id_len;
    
    uint8_t *buf = malloc(buf_size);
    if (!buf) {
        ESP_LOGE(TAG, "wbp_send_result: Out of memory (req=%d)", (int)buf_size);
        return;
    }
    
    CborEncoder encoder, arrayEncoder;
    CborError err;
    
    cbor_encoder_init(&encoder, buf, buf_size, 0);
    
    // Array size: 3 normally, 4 if ID present
    size_t array_len = id ? 4 : 3;
    err = cbor_encoder_create_array(&encoder, &arrayEncoder, array_len);
    if (err != CborNoError) {
        ESP_LOGE(TAG, "wbp_send_result: Failed to create array: %d", err);
        free(buf);
        return;
    }
    
    cbor_encode_uint(&arrayEncoder, channel);
    cbor_encode_uint(&arrayEncoder, WBP_OP_RES);
    err = cbor_encode_text_string(&arrayEncoder, (const char *)data, len);
    if (err != CborNoError) {
        ESP_LOGE(TAG, "wbp_send_result: Failed to encode data (len=%d): %d", (int)len, err);
        free(buf);
        return;
    }
    
    if (id) {
        err = cbor_encode_text_string(&arrayEncoder, id, id_len);
        if (err != CborNoError) {
            ESP_LOGE(TAG, "wbp_send_result: Failed to encode id (len=%d): %d", (int)id_len, err);
            free(buf);
            return;
        }
    }
    
    err = cbor_encoder_close_container(&encoder, &arrayEncoder);
    if (err != CborNoError) {
        ESP_LOGE(TAG, "wbp_send_result: Failed to close (array_len=%d, id=%s): %d", 
                 (int)array_len, id ? "yes" : "no", err);
        free(buf);
        return;
    }
    
    size_t encoded_size = cbor_encoder_get_buffer_size(&encoder, buf);
    ESP_LOGD(TAG, "wbp_send_result: CBOR encoded (%d bytes), sending", (int)encoded_size);
    wbp_rtc_send_cbor(buf, encoded_size);
    free(buf);
}

/**
 * Send progress: [ch, 2, status, ?error, ?id]
 */
static void wbp_send_progress(uint8_t channel, int status, const char *error, const char *id) {
    uint8_t buf[2048];
    CborEncoder encoder, arrayEncoder;
    CborError err;
    
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    
    // Array: [channel, opcode, status, ?error, ?id]
    // If error is NULL but id is present, we must include a null placeholder for error
    size_t array_len = 3;  // base: channel, opcode, status
    if (error) {
        array_len++;  // error slot
    } else if (id) {
        array_len++;  // null placeholder for error (so id can be in correct position)
    }
    if (id) array_len++;  // id slot
    
    err = cbor_encoder_create_array(&encoder, &arrayEncoder, array_len);
    if (err != CborNoError) {
        ESP_LOGE(TAG, "Failed to create array: %d", err);
        return;
    }
    
    cbor_encode_uint(&arrayEncoder, channel);
    cbor_encode_uint(&arrayEncoder, WBP_OP_PRO);
    cbor_encode_uint(&arrayEncoder, status);
    
    // If error is present, encode it. If error is NULL but ID is present,
    // we must encode null as placeholder so ID is in correct position.
    if (error) {
        err = cbor_encode_text_string(&arrayEncoder, error, strlen(error));
        if (err != CborNoError) {
            ESP_LOGE(TAG, "Failed to encode error: %d", err);
            return;
        }
    } else if (id) {
        // Null placeholder for error field
        cbor_encode_null(&arrayEncoder);
    }
    
    if (id) {
        cbor_encode_text_string(&arrayEncoder, id, strlen(id));
    }
    
    err = cbor_encoder_close_container(&encoder, &arrayEncoder);
    if (err != CborNoError) {
        ESP_LOGE(TAG, "wbp_send_progress: Failed to close (array_len=%d, error=%s, id=%s): %d", 
                 (int)array_len, error ? "yes" : "no", id ? "yes" : "no", err);
        return;
    }
    
    size_t encoded_size = cbor_encoder_get_buffer_size(&encoder, buf);
    wbp_rtc_send_cbor(buf, encoded_size);
}

/**
 * Send LOG event: [0, 4, level, message, ?timestamp, ?source]
 */
static void wbp_send_log(uint8_t level, const uint8_t *message, size_t message_len,
                         int64_t timestamp, const char *source) {
    if (!g_peer_handle || !g_data_channel_open || !message || message_len == 0) {
        return;
    }
    
    uint8_t buf[4096];
    CborEncoder encoder, arrayEncoder;
    CborError err;
    
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    
    size_t array_len = 4;  // channel, opcode, level, message
    if (timestamp > 0) array_len++;
    if (source) array_len++;
    
    err = cbor_encoder_create_array(&encoder, &arrayEncoder, array_len);
    if (err != CborNoError) {
        ESP_LOGE(TAG, "Failed to create LOG array: %d", err);
        return;
    }
    
    cbor_encode_uint(&arrayEncoder, WBP_CH_EVENT);
    cbor_encode_uint(&arrayEncoder, WBP_EVT_LOG);
    cbor_encode_uint(&arrayEncoder, level);
    err = cbor_encode_text_string(&arrayEncoder, (const char *)message, message_len);
    if (err != CborNoError) {
        ESP_LOGE(TAG, "Failed to encode LOG message (len=%d): %d", (int)message_len, err);
        return;
    }
    
    if (timestamp > 0) {
        cbor_encode_int(&arrayEncoder, timestamp);
    }
    if (source) {
        cbor_encode_text_string(&arrayEncoder, source, strlen(source));
    }
    
    err = cbor_encoder_close_container(&encoder, &arrayEncoder);
    if (err != CborNoError) {
        ESP_LOGE(TAG, "Failed to close LOG array: %d", err);
        return;
    }
    
    size_t encoded_size = cbor_encoder_get_buffer_size(&encoder, buf);
    wbp_rtc_send_cbor(buf, encoded_size);
}

/**
 * Completion collector - stores completion strings in a dynamic array
 */
typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} completion_collector_t;

static void completion_collector_init(completion_collector_t *collector) {
    collector->items = NULL;
    collector->count = 0;
    collector->capacity = 0;
}

static void completion_collector_add(completion_collector_t *collector, const char *str, size_t len) {
    if (collector->count >= collector->capacity) {
        size_t new_capacity = collector->capacity == 0 ? 8 : collector->capacity * 2;
        char **new_items = realloc(collector->items, new_capacity * sizeof(char *));
        if (!new_items) {
            ESP_LOGE(TAG, "Out of memory for completion collector");
            return;
        }
        collector->items = new_items;
        collector->capacity = new_capacity;
    }
    
    collector->items[collector->count] = malloc(len + 1);
    if (collector->items[collector->count]) {
        memcpy(collector->items[collector->count], str, len);
        collector->items[collector->count][len] = '\0';
        collector->count++;
    }
}

static void completion_collector_free(completion_collector_t *collector) {
    for (size_t i = 0; i < collector->count; i++) {
        free(collector->items[i]);
    }
    free(collector->items);
    collector->items = NULL;
    collector->count = 0;
    collector->capacity = 0;
}

/**
 * Custom print function that collects completion strings
 */
static void completion_print_strn(void *data, const char *str, size_t len) {
    completion_collector_t *collector = (completion_collector_t *)data;
    if (!collector) return;
    
    // Parse the formatted output - completions may be separated by spaces
    size_t start = 0;
    
    while (start < len) {
        // Skip whitespace
        while (start < len && (str[start] == ' ' || str[start] == '\t' || str[start] == '\n' || str[start] == '\r')) {
            start++;
        }
        
        if (start >= len) break;
        
        // Find end of this completion (whitespace or end of string)
        size_t end = start;
        while (end < len && str[end] != ' ' && str[end] != '\t' && str[end] != '\n' && str[end] != '\r') {
            end++;
        }
        
        if (end > start) {
            // Found a completion - add it
            completion_collector_add(collector, str + start, end - start);
        }
        
        start = end;
    }
}

/**
 * Collect completions for a given input string
 * Returns number of completions found
 */
static int wbp_collect_completions(const char *str, size_t len, completion_collector_t *collector) {
    #if MICROPY_HELPER_REPL
    mp_print_t print = {
        .print_strn = completion_print_strn,
        .data = collector
    };
    
    const char *compl_str;
    size_t compl_len = mp_repl_autocomplete(str, len, &print, &compl_str);
    
    if (compl_len == (size_t)-1) {
        // Multiple matches - already collected via print callback
        return collector->count;
    } else if (compl_len > 0) {
        // Single match - compl_str is the suffix to append
        completion_collector_add(collector, compl_str, compl_len);
        return 1;
    }
    return 0;
    #else
    return 0;
    #endif
}

/**
 * Send completions: [ch, 3, completions_array]
 */
static void wbp_send_completions(uint8_t channel, completion_collector_t *collector) {
    // Calculate buffer size: overhead + completions
    size_t buf_size = 64 + collector->count * 32; // Rough estimate
    for (size_t i = 0; i < collector->count; i++) {
        buf_size += strlen(collector->items[i]) + 16;
    }
    
    uint8_t *buf = malloc(buf_size);
    if (!buf) {
        ESP_LOGE(TAG, "Out of memory for completions message");
        return;
    }
    
    CborEncoder encoder, arrayEncoder, completionsEncoder;
    CborError err;
    
    cbor_encoder_init(&encoder, buf, buf_size, 0);
    
    err = cbor_encoder_create_array(&encoder, &arrayEncoder, 3);
    if (err != CborNoError) {
        ESP_LOGE(TAG, "Failed to create completions array: %d", err);
        free(buf);
        return;
    }
    
    cbor_encode_uint(&arrayEncoder, channel);
    cbor_encode_uint(&arrayEncoder, WBP_OP_COM);
    
    // Encode completions array
    err = cbor_encoder_create_array(&arrayEncoder, &completionsEncoder, collector->count);
    if (err != CborNoError) {
        ESP_LOGE(TAG, "Failed to create completions items array: %d", err);
        free(buf);
        return;
    }
    
    for (size_t i = 0; i < collector->count; i++) {
        err = cbor_encode_text_string(&completionsEncoder, collector->items[i], strlen(collector->items[i]));
        if (err != CborNoError) {
            ESP_LOGE(TAG, "Failed to encode completion %d: %d", (int)i, err);
            break;
        }
    }
    
    cbor_encoder_close_container(&arrayEncoder, &completionsEncoder);
    cbor_encoder_close_container(&encoder, &arrayEncoder);
    
    size_t encoded_size = cbor_encoder_get_buffer_size(&encoder, buf);
    wbp_rtc_send_cbor(buf, encoded_size);
    free(buf);
}

/**
 * Send ACK packet: [23, 4, block#, ?tsize, ?mtime, ?mode]
 */
static void wbp_send_file_ack(uint16_t block_num, size_t tsize, 
                               size_t blksize, int64_t mtime, uint16_t mode) {
    uint8_t buf[512];
    CborEncoder encoder, arrayEncoder;
    
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    
    size_t array_len = 3;  // channel, opcode, block#
    if (block_num == 0) {
        array_len++;  // tsize
        if (mtime >= 0) array_len++;
        if (mode > 0) array_len++;
        if (blksize > 0) array_len++;
    }
    
    cbor_encoder_create_array(&encoder, &arrayEncoder, array_len);
    cbor_encode_uint(&arrayEncoder, WBP_CH_FILE);
    cbor_encode_uint(&arrayEncoder, WBP_FILE_ACK);
    cbor_encode_uint(&arrayEncoder, block_num);
    
    if (block_num == 0) {
        cbor_encode_uint(&arrayEncoder, tsize);
        if (blksize > 0) {
            cbor_encode_uint(&arrayEncoder, blksize);
        } else if (mtime >= 0) {
            cbor_encode_int(&arrayEncoder, mtime);
            if (mode > 0) {
                cbor_encode_uint(&arrayEncoder, mode);
            }
        }
    }
    
    cbor_encoder_close_container(&encoder, &arrayEncoder);
    
    size_t encoded_size = cbor_encoder_get_buffer_size(&encoder, buf);
    wbp_rtc_send_cbor(buf, encoded_size);
}

/**
 * Send DATA packet: [23, 3, block#, data]
 */
static void wbp_send_file_data(uint16_t block_num, const uint8_t *data, size_t data_len) {
    // Allocate buffer for CBOR overhead + data
    size_t buf_size = 64 + data_len;
    uint8_t *buf = malloc(buf_size);
    if (!buf) {
        ESP_LOGE(TAG, "Out of memory for DATA packet");
        return;
    }
    
    CborEncoder encoder, arrayEncoder;
    cbor_encoder_init(&encoder, buf, buf_size, 0);
    
    cbor_encoder_create_array(&encoder, &arrayEncoder, 4);
    cbor_encode_uint(&arrayEncoder, WBP_CH_FILE);
    cbor_encode_uint(&arrayEncoder, WBP_FILE_DATA);
    cbor_encode_uint(&arrayEncoder, block_num);
    cbor_encode_byte_string(&arrayEncoder, data, data_len);
    
    cbor_encoder_close_container(&encoder, &arrayEncoder);
    
    size_t encoded_size = cbor_encoder_get_buffer_size(&encoder, buf);
    wbp_rtc_send_cbor(buf, encoded_size);
    free(buf);
}

/**
 * Send ERROR packet: [23, 5, error_code, error_msg]
 */
static void wbp_send_file_error(uint8_t error_code, const char *error_msg) {
    uint8_t buf[1024];
    CborEncoder encoder, arrayEncoder;
    
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    
    cbor_encoder_create_array(&encoder, &arrayEncoder, 4);
    cbor_encode_uint(&arrayEncoder, WBP_CH_FILE);
    cbor_encode_uint(&arrayEncoder, WBP_FILE_ERROR);
    cbor_encode_uint(&arrayEncoder, error_code);
    cbor_encode_text_string(&arrayEncoder, error_msg, strlen(error_msg));
    
    cbor_encoder_close_container(&encoder, &arrayEncoder);
    
    size_t encoded_size = cbor_encoder_get_buffer_size(&encoder, buf);
    wbp_rtc_send_cbor(buf, encoded_size);
}

//=============================================================================
// Ring Buffer Implementation
//=============================================================================

static size_t wbp_ring_space(void) {
    size_t head = g_output_ring.head;
    size_t tail = g_output_ring.tail;
    
    if (head >= tail) {
        return WBP_OUTPUT_RING_SIZE - (head - tail) - 1;
    } else {
        return tail - head - 1;
    }
}

static size_t wbp_ring_available(void) {
    if (!g_output_ring.mutex) return 0;
    xSemaphoreTake(g_output_ring.mutex, portMAX_DELAY);
    size_t head = g_output_ring.head;
    size_t tail = g_output_ring.tail;
    size_t avail;
    if (head >= tail) {
        avail = head - tail;
    } else {
        avail = WBP_OUTPUT_RING_SIZE - tail + head;
    }
    xSemaphoreGive(g_output_ring.mutex);
    return avail;
}

static bool wbp_ring_init(void) {
    if (g_output_ring.mutex != NULL) {
        xSemaphoreTake(g_output_ring.mutex, portMAX_DELAY);
        g_output_ring.head = 0;
        g_output_ring.tail = 0;
        xSemaphoreGive(g_output_ring.mutex);
        ESP_LOGD(TAG, "Ring buffer already initialized, reset");
        return true;
    }
    
    g_output_ring.mutex = xSemaphoreCreateMutex();
    g_output_ring.data_available = xSemaphoreCreateBinary();
    
    if (!g_output_ring.mutex || !g_output_ring.data_available) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        if (g_output_ring.mutex) vSemaphoreDelete(g_output_ring.mutex);
        if (g_output_ring.data_available) vSemaphoreDelete(g_output_ring.data_available);
        g_output_ring.mutex = NULL;
        g_output_ring.data_available = NULL;
        return false;
    }
    
    g_output_ring.head = 0;
    g_output_ring.tail = 0;
    g_output_ring.active = false;
    g_output_ring.drain_task = NULL;
    
    ESP_LOGD(TAG, "Ring buffer initialized (%d bytes)", WBP_OUTPUT_RING_SIZE);
    return true;
}

static void wbp_ring_deinit(void) {
    g_output_ring.active = false;
    
    if (g_output_ring.data_available) {
        xSemaphoreGive(g_output_ring.data_available);
    }
    
    if (g_output_ring.drain_task) {
        vTaskDelay(pdMS_TO_TICKS(100));
        g_output_ring.drain_task = NULL;
    }
    
    if (g_output_ring.mutex) {
        vSemaphoreDelete(g_output_ring.mutex);
        g_output_ring.mutex = NULL;
    }
    if (g_output_ring.data_available) {
        vSemaphoreDelete(g_output_ring.data_available);
        g_output_ring.data_available = NULL;
    }
    
    g_output_ring.head = 0;
    g_output_ring.tail = 0;
    
    ESP_LOGD(TAG, "Ring buffer deinitialized");
}

static size_t wbp_ring_write(const uint8_t *data, size_t len) {
    if (!g_output_ring.mutex || !g_output_ring.active) {
        return 0;
    }
    
    if (xSemaphoreTake(g_output_ring.mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return 0;
    }
    
    size_t space = wbp_ring_space();
    size_t to_write = (len < space) ? len : space;
    
    if (to_write == 0) {
        xSemaphoreGive(g_output_ring.mutex);
        return 0;
    }
    
    for (size_t i = 0; i < to_write; i++) {
        g_output_ring.buffer[g_output_ring.head] = data[i];
        g_output_ring.head = (g_output_ring.head + 1) % WBP_OUTPUT_RING_SIZE;
    }
    
    xSemaphoreGive(g_output_ring.mutex);
    
    if (g_output_ring.data_available) {
        ESP_LOGD(TAG, "wbp_ring_write: signaling drain task (wrote %d bytes)", (int)to_write);
        xSemaphoreGive(g_output_ring.data_available);
    } else {
        ESP_LOGE(TAG, "wbp_ring_write: data_available semaphore is NULL!");
    }
    
    return to_write;
}

static size_t wbp_ring_read(uint8_t *data, size_t max_len) {
    ESP_LOGD(TAG, "wbp_ring_read: called with max_len=%d", (int)max_len);
    
    if (!g_output_ring.mutex) {
        ESP_LOGE(TAG, "wbp_ring_read: mutex is NULL!");
        return 0;
    }
    
    if (xSemaphoreTake(g_output_ring.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "wbp_ring_read: failed to take mutex");
        return 0;
    }
    
    // Calculate available bytes directly (already holding mutex)
    size_t head = g_output_ring.head;
    size_t tail = g_output_ring.tail;
    size_t available;
    if (head >= tail) {
        available = head - tail;
    } else {
        available = WBP_OUTPUT_RING_SIZE - tail + head;
    }
    
    ESP_LOGD(TAG, "wbp_ring_read: available=%d bytes in ring buffer", (int)available);
    
    size_t to_read = (max_len < available) ? max_len : available;
    
    if (to_read == 0) {
        ESP_LOGD(TAG, "wbp_ring_read: nothing to read, returning 0");
        xSemaphoreGive(g_output_ring.mutex);
        return 0;
    }
    
    for (size_t i = 0; i < to_read; i++) {
        data[i] = g_output_ring.buffer[g_output_ring.tail];
        g_output_ring.tail = (g_output_ring.tail + 1) % WBP_OUTPUT_RING_SIZE;
    }
    
    xSemaphoreGive(g_output_ring.mutex);
    
    ESP_LOGD(TAG, "wbp_ring_read: returning %d bytes", (int)to_read);
    return to_read;
}

//=============================================================================
// Drain Task - Sends ring buffer data to WebRTC
//=============================================================================

#define DRAIN_CHUNK_SIZE 4096

static void wbp_drain_task(void *pvParameters) {
    (void)pvParameters;
    
    uint8_t *chunk = malloc(DRAIN_CHUNK_SIZE);
    if (!chunk) {
        ESP_LOGE(TAG, "Drain task: Out of memory");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Drain task started");
    
    while (g_output_ring.active) {
        // Wait for data signal, but with a timeout to check active status
        if (xSemaphoreTake(g_output_ring.data_available, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        
        ESP_LOGD(TAG, "Drain task: data signal received");
        
        // Removed batching delay to reduce latency and transport congestion
        
        while (g_output_ring.active) {
            size_t bytes_read = wbp_ring_read(chunk, DRAIN_CHUNK_SIZE);
            
            if (bytes_read == 0) {
                ESP_LOGD(TAG, "Drain task: no more data in ring buffer");
                break;
            }
            
            ESP_LOGD(TAG, "Drain task: read %d bytes from ring buffer", (int)bytes_read);
            
            if (g_data_channel_open) {
                ESP_LOGD(TAG, "Drain task: calling wbp_send_result (ch=%d, id=%s)", 
                         g_wbp_current_channel, g_wbp_current_id ? g_wbp_current_id : "NULL");
                // Pass current message ID if execution is active
                wbp_send_result(g_wbp_current_channel, chunk, bytes_read, g_wbp_current_id);
            } else {
                ESP_LOGW(TAG, "Drain task: channel not open, dropping %d bytes", (int)bytes_read);
            }
        }
        
        taskYIELD();
    }
    
    free(chunk);
    ESP_LOGI(TAG, "Drain task exiting");
    g_output_ring.drain_task = NULL;
    vTaskDelete(NULL);
}

static bool wbp_drain_task_start(void) {
    if (g_output_ring.drain_task != NULL) {
        g_output_ring.active = true;
        ESP_LOGI(TAG, "Drain task already running");
        return true;
    }
    
    g_output_ring.active = true;
    
    BaseType_t ret = xTaskCreate(
        wbp_drain_task,
        "wbp_rtc_drain",
        8192,
        NULL,
        5,
        &g_output_ring.drain_task
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create drain task");
        g_output_ring.active = false;
        return false;
    }
    
    ESP_LOGI(TAG, "Drain task created");
    return true;
}

static void wbp_drain_task_stop(void) {
    g_output_ring.active = false;
    
    if (g_output_ring.data_available) {
        xSemaphoreGive(g_output_ring.data_available);
    }
    
    int timeout = 50;
    while (g_output_ring.drain_task != NULL && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

//=============================================================================
// WebREPL dupterm Stream Implementation
//=============================================================================

static mp_uint_t wbp_stream_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode) {
    (void)self_in;
    (void)buf;
    (void)size;
    
    *errcode = MP_EAGAIN;
    return MP_STREAM_ERROR;
}

static mp_uint_t wbp_stream_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode) {
    (void)self_in;
    
    ESP_LOGD(TAG, "wbp_stream_write: called with %d bytes", (int)size);
    
    const uint8_t *data = (const uint8_t *)buf;
    
    if (!g_output_ring.active || !g_data_channel_open) {
        ESP_LOGD(TAG, "wbp_stream_write: ring not active or channel closed (active=%d, open=%d)", 
                 g_output_ring.active, g_data_channel_open);
        *errcode = 0;
        return size;
    }
    
    size_t remaining = size;
    size_t total_written = 0;
    int retries = 0;
    const int max_retries = 50;
    
    while (remaining > 0 && retries < max_retries) {
        size_t written = wbp_ring_write(data + total_written, remaining);
        
        if (written > 0) {
            total_written += written;
            remaining -= written;
            retries = 0;
        } else {
            retries++;
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        
        if (!g_output_ring.active || !g_data_channel_open) {
            *errcode = 0;
            return size;
        }
    }
    
    *errcode = 0;
    return size;
}

static mp_uint_t wbp_stream_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    (void)self_in;
    
    if (request == MP_STREAM_POLL) {
        uintptr_t flags = arg;
        uintptr_t ret = 0;
        if (flags & MP_STREAM_POLL_WR) {
            ret |= MP_STREAM_POLL_WR;
        }
        return ret;
    }
    
    *errcode = MP_EINVAL;
    return MP_STREAM_ERROR;
}

// Attach dupterm
static void wbp_dupterm_attach(void) {
    if (g_wbp_stream == NULL) {
        g_wbp_stream = m_new_obj(wbp_stream_obj_t);
        g_wbp_stream->base.type = &wbp_rtc_stream_type;
    }
    
    // Call os.dupterm(stream, 1) using the function object
    // Slot 1 is for WebRTC, Slot 2 is for WSS, Slot 0 is for UART
    mp_obj_t dupterm_args[2] = {
        MP_OBJ_FROM_PTR(g_wbp_stream),
        MP_OBJ_NEW_SMALL_INT(1)
    };
    mp_call_function_n_kw(MP_OBJ_FROM_PTR(&mp_os_dupterm_obj), 2, 0, dupterm_args);
    
    ESP_LOGI(TAG, "Dupterm attached to slot 1 (WebRTC)");
}

// Detach dupterm from slot 1
static void wbp_dupterm_detach(void) {
    // Call os.dupterm(None, 1) using the function object
    mp_obj_t dupterm_args[2] = {
        mp_const_none,
        MP_OBJ_NEW_SMALL_INT(1)
    };
    mp_call_function_n_kw(MP_OBJ_FROM_PTR(&mp_os_dupterm_obj), 2, 0, dupterm_args);
    
    ESP_LOGI(TAG, "Dupterm detached from slot 1");
}

//=============================================================================
// File I/O Handlers
//=============================================================================

static void wbp_close_transfer(void) {
    if (g_file_transfer.active) {
        if (g_file_transfer.file_obj != MP_OBJ_NULL) {
            mp_stream_close(g_file_transfer.file_obj);
            g_file_transfer.file_obj = MP_OBJ_NULL;
        }
        g_file_transfer.active = false;
        ESP_LOGI(TAG, "File transfer closed");
    }
}

// Handle WRQ (Client wants to upload)
static void wbp_handle_wrq(const char *path, size_t tsize, size_t blksize) {
    if (g_file_transfer.active) {
        wbp_send_file_error(WBP_ERR_ACCESS, "Transfer already in progress");
        return;
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t file = mp_call_function_2(MP_OBJ_FROM_PTR(&mp_builtin_open_obj),
                                            mp_obj_new_str(path, strlen(path)),
                                            mp_obj_new_str("wb", 2));
        
        g_file_transfer.active = true;
        g_file_transfer.opcode = WBP_FILE_WRQ;
        strncpy(g_file_transfer.path, path, sizeof(g_file_transfer.path)-1);
        g_file_transfer.file_obj = file;
        g_file_transfer.tsize = tsize;
        g_file_transfer.blksize = (blksize > 0 && blksize <= 16384) ? blksize : WBP_DEFAULT_BLKSIZE;
        g_file_transfer.next_block = 1; // Expect block 1
        g_file_transfer.bytes_transferred = 0;
        g_file_transfer.last_activity = mp_hal_ticks_ms();

        nlr_pop();

        // Send ACK 0 to confirm ready
        wbp_send_file_ack(0, tsize, g_file_transfer.blksize, -1, 0);
        ESP_LOGI(TAG, "WRQ accepted: %s (%d bytes)", path, (int)tsize);
    } else {
        wbp_send_file_error(WBP_ERR_ACCESS, "Could not open file for writing");
    }
}

// Handle RRQ (Client wants to download)
static void wbp_handle_rrq(const char *path, size_t blksize) {
    if (g_file_transfer.active) {
        wbp_send_file_error(WBP_ERR_ACCESS, "Transfer already in progress");
        return;
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        // Open file
        mp_obj_t file = mp_call_function_2(MP_OBJ_FROM_PTR(&mp_builtin_open_obj),
                                            mp_obj_new_str(path, strlen(path)),
                                            mp_obj_new_str("rb", 2));
        
        // Get file size (seek to end, tell, seek to start)
        mp_obj_t seek_method = mp_load_attr(file, MP_QSTR_seek);
        mp_obj_t tell_method = mp_load_attr(file, MP_QSTR_tell);
        
        // seek(0, 2) -> end
        mp_obj_t args[2] = { MP_OBJ_NEW_SMALL_INT(0), MP_OBJ_NEW_SMALL_INT(2) };
        mp_call_function_n_kw(seek_method, 2, 0, args);
        
        // tell()
        mp_obj_t size_obj = mp_call_function_0(tell_method);
        mp_int_t size = mp_obj_get_int(size_obj);
        
        // seek(0, 0) -> start
        args[1] = MP_OBJ_NEW_SMALL_INT(0);
        mp_call_function_n_kw(seek_method, 2, 0, args);

        g_file_transfer.active = true;
        g_file_transfer.opcode = WBP_FILE_RRQ;
        strncpy(g_file_transfer.path, path, sizeof(g_file_transfer.path)-1);
        g_file_transfer.file_obj = file;
        g_file_transfer.tsize = size;
        g_file_transfer.blksize = (blksize > 0 && blksize <= 16384) ? blksize : WBP_DEFAULT_BLKSIZE;
        g_file_transfer.next_block = 0; 
        g_file_transfer.bytes_transferred = 0;
        g_file_transfer.last_activity = mp_hal_ticks_ms();

        nlr_pop();

        // Send ACK 0 with metadata
        wbp_send_file_ack(0, size, 0, 0, 0);
        ESP_LOGI(TAG, "RRQ accepted: %s (%d bytes)", path, (int)size);

    } else {
        wbp_send_file_error(WBP_ERR_NOT_FOUND, "File not found or unreadable");
    }
}

// Handle DATA block (from Client during Upload/WRQ)
static void wbp_handle_data(uint16_t block_num, const uint8_t *data, size_t data_len) {
    if (!g_file_transfer.active || g_file_transfer.opcode != WBP_FILE_WRQ) {
        wbp_send_file_error(WBP_ERR_UNKNOWN_TID, "No active upload");
        return;
    }

    if (block_num != g_file_transfer.next_block) {
        // Resend ACK for previous block (lost ACK scenario)
        if (block_num == g_file_transfer.next_block - 1) {
             wbp_send_file_ack(block_num, 0, 0, -1, 0);
             return;
        }
        wbp_send_file_error(WBP_ERR_UNKNOWN_TID, "Invalid block number");
        return;
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t write_method = mp_load_attr(g_file_transfer.file_obj, MP_QSTR_write);
        mp_obj_t data_bytes = mp_obj_new_bytes(data, data_len);
        mp_call_function_1(write_method, data_bytes);
        
        nlr_pop();

        g_file_transfer.bytes_transferred += data_len;
        g_file_transfer.last_activity = mp_hal_ticks_ms();
        
        // Send ACK
        wbp_send_file_ack(block_num, 0, 0, -1, 0);
        
        g_file_transfer.next_block++;
        
        // Check for completion
        if (data_len < g_file_transfer.blksize) {
            ESP_LOGI(TAG, "Upload complete: %s (%d bytes)", g_file_transfer.path, (int)g_file_transfer.bytes_transferred);
            wbp_close_transfer();
        }

    } else {
        wbp_send_file_error(WBP_ERR_DISK_FULL, "Write failed");
        wbp_close_transfer();
    }
}

// Handle ACK (from Client during Download/RRQ)
static void wbp_handle_ack(uint16_t block_num) {
    if (!g_file_transfer.active || g_file_transfer.opcode != WBP_FILE_RRQ) {
        // Ignore stray ACKs
        return;
    }
    
    if (block_num != g_file_transfer.next_block) {
        ESP_LOGW(TAG, "Unexpected ACK %d (expected %d)", block_num, g_file_transfer.next_block);
        return;
    }

    g_file_transfer.last_activity = mp_hal_ticks_ms();

    // Send NEXT block (block_num + 1)
    uint16_t send_block = block_num + 1;

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t read_method = mp_load_attr(g_file_transfer.file_obj, MP_QSTR_read);
        mp_obj_t size_obj = MP_OBJ_NEW_SMALL_INT(g_file_transfer.blksize);
        mp_obj_t data_obj = mp_call_function_1(read_method, size_obj);
        
        mp_buffer_info_t bufinfo;
        mp_get_buffer_raise(data_obj, &bufinfo, MP_BUFFER_READ);
        
        wbp_send_file_data(send_block, bufinfo.buf, bufinfo.len);
        
        nlr_pop();
        
        g_file_transfer.next_block = send_block; // Wait for ACK of this block
        g_file_transfer.bytes_transferred += bufinfo.len;

        // Check EOF
        if (bufinfo.len < g_file_transfer.blksize) {
            ESP_LOGI(TAG, "Download complete sent: %s", g_file_transfer.path);
            wbp_close_transfer();
        }

    } else {
        wbp_send_file_error(WBP_ERR_ACCESS, "Read failed");
        wbp_close_transfer();
    }
}

//=============================================================================
// Message Queue Helpers
//=============================================================================

static bool wbp_queue_init(void) {
    if (wbp_msg_queue && wbp_queue_mutex) {
        // Already initialized
        xQueueReset(wbp_msg_queue); // Clear any stale messages
        return true;
    }

    wbp_msg_queue = xQueueCreate(WBP_QUEUE_SIZE, sizeof(wbp_queue_msg_t));
    wbp_queue_mutex = xSemaphoreCreateMutex();
    
    if (!wbp_msg_queue || !wbp_queue_mutex) {
        ESP_LOGE(TAG, "Failed to create message queue");
        return false;
    }
    
    ESP_LOGI(TAG, "Message queue created");
    return true;
}

static void wbp_queue_message(wbp_msg_type_t type, uint8_t channel, 
                                const char *data, size_t data_len, const char *id, bool is_bytecode) {
    ESP_LOGD(TAG, "wbp_queue_message: type=%d channel=%d len=%d", type, channel, (int)data_len);

    wbp_queue_msg_t msg = {
        .type = type,
        .channel = channel,
        .data = NULL,
        .data_len = data_len,
        .id = NULL,
        .is_bytecode = is_bytecode
    };
    
    if (data && data_len > 0) {
        msg.data = malloc(data_len + 1);
        if (!msg.data) {
            ESP_LOGE(TAG, "Out of memory for queue message");
            return;
        }
        memcpy(msg.data, data, data_len);
        msg.data[data_len] = '\0';
    }
    
    if (id) {
        msg.id = strdup(id);
    }
    
    if (xQueueSend(wbp_msg_queue, &msg, 0) != pdPASS) {
        ESP_LOGW(TAG, "Queue full, message dropped");
        if (msg.data) free(msg.data);
        if (msg.id) free(msg.id);
    } else {
        ESP_LOGD(TAG, "Message queued successfully");
    }
}

// Process queue (called from MP main task)
static mp_obj_t wbp_process_queue(void) {
    // Check if queue is initialized - fail gracefully if not
    if (!wbp_msg_queue) {
        // Queue not initialized - return 0 processed (no error, just nothing to do)
        ESP_LOGD(TAG, "process_queue: queue not initialized");
        return MP_OBJ_NEW_SMALL_INT(0);
    }
    
    wbp_queue_msg_t msg;
    int processed = 0;
    
    // Process up to 10 messages to avoid starving main loop
    for (int i = 0; i < 10; i++) {
        if (xQueueReceive(wbp_msg_queue, &msg, 0) != pdPASS) {
            break;
        }
        
        processed++;
        ESP_LOGD(TAG, "process_queue: processing msg type=%d channel=%d", msg.type, msg.channel);
        
        switch (msg.type) {
            case WBP_MSG_RAW_EXEC: {
                ESP_LOGD(TAG, "process_queue: executing code on channel=%d", msg.channel);
                g_wbp_executing = true;
                g_wbp_current_channel = msg.channel;
                g_wbp_current_id = msg.id; // Set current ID for output correlation
                
                nlr_buf_t nlr;
                if (nlr_push(&nlr) == 0) {
                    if (msg.is_bytecode) {
                        // TODO: Execute .mpy bytecode
                        ESP_LOGW(TAG, "Bytecode execution not yet implemented");
                    } else {
                        // Execute Python source
                        bool success = false;
                        
                        // For terminal channel, try SINGLE_INPUT first to support auto-printing (REPL behavior)
                        if (msg.channel == WBP_CH_TRM) {
                            nlr_buf_t nlr2;
                            if (nlr_push(&nlr2) == 0) {
                                mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_,
                                                                              msg.data, msg.data_len, false);
                                qstr source_name = lex->source_name;
                                mp_parse_tree_t parse_tree = mp_parse(lex, MP_PARSE_SINGLE_INPUT);
                                mp_obj_t module_fun = mp_compile(&parse_tree, source_name, true); // is_repl=true
                                mp_call_function_0(module_fun);
                                nlr_pop();
                                success = true;
                            } else {
                                // Failed (likely syntax error due to multi-statement), swallow exception and retry as FILE_INPUT
                                mp_obj_t exc = MP_OBJ_FROM_PTR(nlr2.ret_val);
                                (void)exc; // Suppress unused warning
                                success = false;
                            }
                        }
                        
                        // If not successful (failed SINGLE attempt OR non-terminal channel), use FILE_INPUT
                        if (!success) {
                            mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_,
                                                                          msg.data, msg.data_len, false);
                            qstr source_name = lex->source_name;
                            
                            mp_parse_tree_t parse_tree = mp_parse(lex, MP_PARSE_FILE_INPUT);
                            
                            // Compile and execute - is_repl=false for script mode
                            mp_obj_t module_fun = mp_compile(&parse_tree, source_name, false);
                            mp_call_function_0(module_fun);
                        }
                    }
                    
                    nlr_pop();
                    
                    // CRITICAL: Lock ring buffer for entire flush + progress send
                    // This guarantees ordering and prevents drain task interleaving
                    if (g_data_channel_open && g_output_ring.mutex) {
                        xSemaphoreTake(g_output_ring.mutex, portMAX_DELAY);
                        
                        uint8_t flush_buf[256];
                        size_t head = g_output_ring.head;
                        size_t tail = g_output_ring.tail;
                        size_t available;
                        
                        while (1) {
                            if (head >= tail) available = head - tail;
                            else available = WBP_OUTPUT_RING_SIZE - tail + head;
                            
                            if (available == 0) break;
                            
                            size_t to_read = (available > sizeof(flush_buf)) ? sizeof(flush_buf) : available;
                            for (size_t i = 0; i < to_read; i++) {
                                flush_buf[i] = g_output_ring.buffer[g_output_ring.tail];
                                g_output_ring.tail = (g_output_ring.tail + 1) % WBP_OUTPUT_RING_SIZE;
                            }
                            wbp_send_result(g_wbp_current_channel, flush_buf, to_read, g_wbp_current_id);
                            
                            head = g_output_ring.head;
                            tail = g_output_ring.tail;
                        }
                        
                        // Send success with ID (while still holding ring mutex)
                        wbp_send_progress(msg.channel, 0, NULL, msg.id);
                        
                        xSemaphoreGive(g_output_ring.mutex);
                    } else {
                        // Fallback if not open or no mutex
                        wbp_send_progress(msg.channel, 0, NULL, msg.id);
                    }
                } else {
                    mp_obj_t exc = MP_OBJ_FROM_PTR(nlr.ret_val);
                    
                    bool is_interrupt = mp_obj_exception_match(exc, &mp_type_KeyboardInterrupt);
                    
                    if (!is_interrupt) {
                        mp_obj_print_exception(&mp_plat_print, exc);
                    }
                    
                    // CRITICAL: Lock ring buffer for entire flush + progress/error send
                    if (g_data_channel_open && g_output_ring.mutex) {
                        xSemaphoreTake(g_output_ring.mutex, portMAX_DELAY);
                        
                        uint8_t flush_buf[256];
                        size_t head = g_output_ring.head;
                        size_t tail = g_output_ring.tail;
                        size_t available;
                        
                        while (1) {
                            if (head >= tail) available = head - tail;
                            else available = WBP_OUTPUT_RING_SIZE - tail + head;
                            
                            if (available == 0) break;
                            
                            size_t to_read = (available > sizeof(flush_buf)) ? sizeof(flush_buf) : available;
                            for (size_t i = 0; i < to_read; i++) {
                                flush_buf[i] = g_output_ring.buffer[g_output_ring.tail];
                                g_output_ring.tail = (g_output_ring.tail + 1) % WBP_OUTPUT_RING_SIZE;
                            }
                            wbp_send_result(g_wbp_current_channel, flush_buf, to_read, g_wbp_current_id);
                            
                            head = g_output_ring.head;
                            tail = g_output_ring.tail;
                        }
                        
                        if (is_interrupt) {
                            wbp_send_progress(msg.channel, 0, NULL, msg.id);
                        } else {
                            wbp_send_progress(msg.channel, 1, "Execution failed", msg.id);
                        }
                        
                        xSemaphoreGive(g_output_ring.mutex);
                    } else {
                        // Fallback
                        if (is_interrupt) {
                            wbp_send_progress(msg.channel, 0, NULL, msg.id);
                        } else {
                            wbp_send_progress(msg.channel, 1, "Execution failed", msg.id);
                        }
                    }
                }
                
                // Yield for success case too
                vTaskDelay(pdMS_TO_TICKS(10));
                
                g_wbp_executing = false;
                g_wbp_current_channel = WBP_CH_TRM;  // Reset to terminal channel
                g_wbp_current_id = NULL; // Clear ID
                break;
            }
            
            case WBP_MSG_RESET: {
                // Use channel field for mode (1=Hard, 0=Soft)
                bool hard = (msg.channel == 1);
                
                if (hard) {
                    ESP_LOGW(TAG, "Executing HARD RESET via WBP");
                    // Flush any pending logs
                    vTaskDelay(pdMS_TO_TICKS(200));
                    esp_restart();
                } else {
                    ESP_LOGI(TAG, "Executing SOFT RESET via WBP");
                    
                    // Cleanup stale pointers before heap is reset
                    g_wbp_stream = NULL;
                    
                    // Reset file transfer state
                    if (g_file_transfer.active) {
                        g_file_transfer.active = false;
                        g_file_transfer.file_obj = MP_OBJ_NULL; 
                    }
                    
                    // Raise SystemExit to trigger soft reset in main loop
                    mp_raise_type(&mp_type_SystemExit);
                }
                break;
            }

            default:
                break;
        }
        
        if (msg.data) free(msg.data);
        if (msg.id) free(msg.id);
    }
    
    return MP_OBJ_NEW_SMALL_INT(processed);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(wbp_rtc_process_queue_obj, wbp_process_queue);

//=============================================================================
// Message Handlers (called from on_data callback)
//=============================================================================

// Forward declarations
static void handle_channel_message(uint8_t channel, const uint8_t *buf, size_t len);
static void handle_file_message(const uint8_t *buf, size_t len);

/**
 * Main WBP message dispatcher - discriminates by channel number
 */
static void wbp_handle_message(const uint8_t *data, size_t len) {
    ESP_LOGD(TAG, "wbp_handle_message: received %d bytes", (int)len);
    
    if (len < 2) {
        ESP_LOGE(TAG, "Message too short: %d", (int)len);
        return;
    }
    
    CborParser parser;
    CborValue value, arrayValue;
    
    if (cbor_parser_init(data, len, 0, &parser, &value) != CborNoError) {
        ESP_LOGE(TAG, "Failed to parse CBOR");
        return;
    }
    
    if (!cbor_value_is_array(&value)) {
        ESP_LOGE(TAG, "Not a CBOR array");
        return;
    }
    
    // Enter array and read channel number (first element)
    cbor_value_enter_container(&value, &arrayValue);
    
    uint64_t channel_u64;
    if (cbor_value_get_uint64(&arrayValue, &channel_u64) != CborNoError) {
        ESP_LOGE(TAG, "Invalid channel");
        return;
    }
    uint8_t channel = (uint8_t)channel_u64;
    
    // Dispatch based on channel number
    if (channel == WBP_CH_EVENT) {
        // Channel 0: Events and keepalive pings
        // Silently ignore - these are typically keepalive messages [0, 99]
        // from the client to prevent SCTP congestion window collapse
        ESP_LOGD(TAG, "Received channel 0 message (event/keepalive)");
    } else if (channel == WBP_CH_FILE) {
        // Channel 23: File operations
        handle_file_message(data, len);
    } else if (channel >= WBP_CH_TRM && channel <= 22) {
        // Channels 1-22: Execution channels
        handle_channel_message(channel, data, len);
    } else {
        ESP_LOGW(TAG, "Unknown channel: %d", channel);
    }
}

/**
 * Handle channel message: [ch, op, ...fields]
 */
static void handle_channel_message(uint8_t channel, const uint8_t *buf, size_t len) {
    CborParser parser;
    CborValue value, arrayValue;
    
    // Parse CBOR array
    if (cbor_parser_init(buf, len, 0, &parser, &value) != CborNoError) {
        ESP_LOGE(TAG, "Failed to parse CBOR");
        return;
    }
    
    if (!cbor_value_is_array(&value)) {
        ESP_LOGE(TAG, "Invalid channel message (not array)");
        return;
    }
    
    size_t array_len = 0;
    if (cbor_value_get_array_length(&value, &array_len) != CborNoError || array_len < 2) {
        ESP_LOGE(TAG, "Channel array too short: %d", (int)array_len);
        return;
    }
    
    // Enter array
    cbor_value_enter_container(&value, &arrayValue);
    
    // Read channel ID (already know it, but need to advance past it)
    uint64_t channel_check;
    cbor_value_get_uint64(&arrayValue, &channel_check);
    cbor_value_advance(&arrayValue);
    
    // Read opcode
    uint64_t opcode_u64;
    cbor_value_get_uint64(&arrayValue, &opcode_u64);
    cbor_value_advance(&arrayValue);
    uint8_t opcode = opcode_u64;
    
    ESP_LOGD(TAG, "Channel msg: ch=%d op=%d", channel, opcode);
    
    switch (opcode) {
        case WBP_OP_EXE: {
            // Read code/data field
            const char *code = NULL;
            size_t code_len = 0;
            bool is_bytecode = false;
            
            if (cbor_value_is_text_string(&arrayValue)) {
                cbor_value_dup_text_string(&arrayValue, (char **)&code, &code_len, NULL);
            } else if (cbor_value_is_byte_string(&arrayValue)) {
                cbor_value_dup_byte_string(&arrayValue, (uint8_t **)&code, &code_len, NULL);
                is_bytecode = true;
            } else {
                wbp_send_progress(channel, 1, "Invalid data type", NULL);
                return;
            }
            cbor_value_advance(&arrayValue);
            
            // Optional format field (position 3)
            if (!cbor_value_at_end(&arrayValue)) {
                uint64_t format;
                if (cbor_value_get_uint64(&arrayValue, &format) == CborNoError) {
                    if (format == WBP_FMT_MPY) {
                        is_bytecode = true;
                    }
                    cbor_value_advance(&arrayValue);
                }
            }

            // Optional ID field (position 4)
            char *id = NULL;
            size_t id_len = 0;
            if (!cbor_value_at_end(&arrayValue)) {
                 if (cbor_value_is_text_string(&arrayValue)) {
                     cbor_value_dup_text_string(&arrayValue, &id, &id_len, NULL);
                     cbor_value_advance(&arrayValue);
                 }
            }
            
            // Check for tab completion request (code ends with \t)
            if (!is_bytecode && code_len > 0 && code[code_len - 1] == '\t') {
                // Tab completion request - collect and send completions
                completion_collector_t collector;
                completion_collector_init(&collector);
                
                wbp_collect_completions(code, code_len - 1, &collector);
                
                wbp_send_completions(channel, &collector);
                completion_collector_free(&collector);
                
                free((void *)code);
                if (id) free(id);
                break;
            }
            
            ESP_LOGD(TAG, "Execute %s on ch=%d: %.*s%s", 
                     is_bytecode ? "bytecode" : "source",
                     channel,
                     code_len > 20 ? 20 : (int)code_len,
                     code,
                     code_len > 20 ? "..." : "");
            
            // Queue for GIL-safe execution
            wbp_queue_message(WBP_MSG_RAW_EXEC, channel, code, code_len, id, is_bytecode);
            
            // Free duplicated strings
            free((void *)code);
            if (id) free(id);
            break;
        }
        
        case WBP_OP_INT: {
            ESP_LOGI(TAG, "Interrupt on ch=%d", channel);
            mp_sched_keyboard_interrupt();
            wbp_send_progress(channel, 0, NULL, NULL);  // Acknowledge immediately
            break;
        }
        
        case WBP_OP_RST: {
            ESP_LOGI(TAG, "Reset request");
            uint64_t mode = 0;
            // Parse mode if present [channel, 2, mode]
            if (!cbor_value_at_end(&arrayValue)) {
                 cbor_value_get_uint64(&arrayValue, &mode);
            }
            
            ESP_LOGI(TAG, "Queueing %s reset", mode ? "HARD" : "SOFT");
            
            // Queue reset message
            wbp_queue_message(WBP_MSG_RESET, (uint8_t)mode, NULL, 0, NULL, false);
            break;
        }
        
        default:
            ESP_LOGW(TAG, "Unknown opcode: %d", opcode);
            break;
    }
}

/**
 * Handle file operation: [23, opcode, ...args]
 */
static void handle_file_message(const uint8_t *buf, size_t len) {
    CborParser parser;
    CborValue value, arrayValue;
    
    if (cbor_parser_init(buf, len, 0, &parser, &value) != CborNoError) {
        ESP_LOGE(TAG, "Failed to parse CBOR");
        return;
    }
    
    if (!cbor_value_is_array(&value)) {
        ESP_LOGE(TAG, "Invalid file message (not array)");
        return;
    }
    
    cbor_value_enter_container(&value, &arrayValue);
    
    // Read channel (23 for files)
    uint64_t channel;
    if (cbor_value_get_uint64(&arrayValue, &channel) != CborNoError) return;
    cbor_value_advance(&arrayValue);
    
    // Read opcode
    uint64_t opcode_u64;
    if (cbor_value_get_uint64(&arrayValue, &opcode_u64) != CborNoError) return;
    cbor_value_advance(&arrayValue);
    uint8_t opcode = (uint8_t)opcode_u64;
    
    switch (opcode) {
        case WBP_FILE_WRQ: { // [23, 2, filename, tsize, ?blksize, ?timeout, ?mtime]
            char *path = NULL;
            size_t path_len;
            if (cbor_value_dup_text_string(&arrayValue, &path, &path_len, NULL) != CborNoError) {
                wbp_send_file_error(WBP_ERR_ILLEGAL_OP, "Invalid filename");
                return;
            }
            cbor_value_advance(&arrayValue);
            
            uint64_t tsize = 0;
            if (cbor_value_get_uint64(&arrayValue, &tsize) != CborNoError) {
                wbp_send_file_error(WBP_ERR_ILLEGAL_OP, "Invalid size");
                free(path);
                return;
            }
            cbor_value_advance(&arrayValue);
            
            // Optional args
            uint64_t blksize = 0;
            if (!cbor_value_at_end(&arrayValue)) {
                cbor_value_get_uint64(&arrayValue, &blksize);
                cbor_value_advance(&arrayValue);
            }
            
            wbp_handle_wrq(path, (size_t)tsize, (size_t)blksize);
            free(path);
            break;
        }
        
        case WBP_FILE_RRQ: { // [23, 1, filename, ?blksize, ?timeout]
            char *path = NULL;
            size_t path_len;
            if (cbor_value_dup_text_string(&arrayValue, &path, &path_len, NULL) != CborNoError) {
                wbp_send_file_error(WBP_ERR_ILLEGAL_OP, "Invalid filename");
                return;
            }
            cbor_value_advance(&arrayValue);
            
            uint64_t blksize = 0;
            if (!cbor_value_at_end(&arrayValue)) {
                cbor_value_get_uint64(&arrayValue, &blksize);
            }
            
            wbp_handle_rrq(path, (size_t)blksize);
            free(path);
            break;
        }
        
        case WBP_FILE_DATA: { // [23, 3, block#, data]
            uint64_t block_num;
            if (cbor_value_get_uint64(&arrayValue, &block_num) != CborNoError) return;
            cbor_value_advance(&arrayValue);
            
            uint8_t *data = NULL;
            size_t data_len;
            if (cbor_value_dup_byte_string(&arrayValue, &data, &data_len, NULL) != CborNoError) {
                return;
            }
            
            wbp_handle_data((uint16_t)block_num, data, data_len);
            free(data);
            break;
        }
        
        case WBP_FILE_ACK: { // [23, 4, block#]
            uint64_t block_num;
            if (cbor_value_get_uint64(&arrayValue, &block_num) != CborNoError) return;
            
            wbp_handle_ack((uint16_t)block_num);
            break;
        }
        
        case WBP_FILE_ERROR: {
            // Client sent error, abort transfer
            if (g_file_transfer.active) {
                ESP_LOGW(TAG, "Client sent error, aborting transfer");
                wbp_close_transfer();
            }
            break;
        }
        
        default:
            wbp_send_file_error(WBP_ERR_ILLEGAL_OP, "Unknown opcode");
            break;
    }
}

//=============================================================================
// Python API
//=============================================================================

/**
 * webrepl_rtc.start(peer) - Initialize WBP handler with WebRTC peer
 */
static mp_obj_t webrepl_rtc_start(mp_obj_t peer_obj) {
    ESP_LOGI(TAG, "webrepl_rtc_start: called");
    
    // Extract handle from peer object
    g_peer_handle = webrtc_peer_get_handle(peer_obj);
    if (!g_peer_handle) {
        ESP_LOGE(TAG, "webrepl_rtc_start: Invalid peer object");
        mp_raise_TypeError(MP_ERROR_TEXT("Invalid peer object"));
    }
    
    ESP_LOGI(TAG, "webrepl_rtc_start: peer handle extracted");
    
    // Keep reference to prevent GC
    g_peer_obj = peer_obj;
    
    // Check if data channel is open
    g_data_channel_open = webrtc_peer_is_data_channel_open(peer_obj);
    
    ESP_LOGI(TAG, "Starting WBP over WebRTC (DataChannel %s)", 
             g_data_channel_open ? "OPEN" : "CLOSED");
    
    // Initialize ring buffer
    ESP_LOGI(TAG, "webrepl_rtc_start: initializing ring buffer");
    if (!wbp_ring_init()) {
        ESP_LOGE(TAG, "webrepl_rtc_start: Failed to init ring buffer");
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to init ring buffer"));
    }
    
    // Initialize message queue
    ESP_LOGI(TAG, "webrepl_rtc_start: initializing message queue");
    if (!wbp_queue_init()) {
        ESP_LOGE(TAG, "webrepl_rtc_start: Failed to init queue");
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to init queue"));
    }
    
    // Start drain task
    ESP_LOGI(TAG, "webrepl_rtc_start: starting drain task");
    if (!wbp_drain_task_start()) {
        ESP_LOGE(TAG, "webrepl_rtc_start: Failed to start drain task");
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to start drain task"));
    }
    
    // Reset file transfer state
    g_file_transfer.active = false;
    g_file_transfer.file_obj = MP_OBJ_NULL;
    
    // Reset execution state
    g_wbp_executing = false;
    g_wbp_current_channel = WBP_CH_TRM;
    if (g_wbp_current_id) {
         free(g_wbp_current_id);
         g_wbp_current_id = NULL;
    }
    
    // Attach dupterm to slot 1 (WebRTC)
    wbp_dupterm_attach();
    
    ESP_LOGI(TAG, "WBP over WebRTC started successfully");
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(webrepl_rtc_start_obj, webrepl_rtc_start);

/**
 * webrepl_rtc.stop() - Stop WBP handler
 */
static mp_obj_t webrepl_rtc_stop(void) {
    ESP_LOGI(TAG, "Stopping WBP over WebRTC...");
    
    // Detach dupterm
    wbp_dupterm_detach();
    
    // Stop drain task
    wbp_drain_task_stop();
    
    // Cleanup ring buffer
    wbp_ring_deinit();
    
    // Clear peer reference
    g_peer_handle = NULL;
    g_data_channel_open = false;
    g_peer_obj = MP_OBJ_NULL;
    
    ESP_LOGI(TAG, "WBP over WebRTC stopped");
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(webrepl_rtc_stop_obj, webrepl_rtc_stop);

/**
 * webrepl_rtc.on_data(cbor_bytes) - Called by Python when peer.on_data fires
 */
static mp_obj_t webrepl_rtc_on_data(mp_obj_t data_in) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);
    
    ESP_LOGD(TAG, "webrepl_rtc_on_data: called with %d bytes", (int)bufinfo.len);
    
    // Process WBP message
    wbp_handle_message(bufinfo.buf, bufinfo.len);
    
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(webrepl_rtc_on_data_obj, webrepl_rtc_on_data);

/**
 * webrepl_rtc.update_channel_state(is_open) - Update data channel state
 */
static mp_obj_t webrepl_rtc_update_channel_state(mp_obj_t is_open_obj) {
    g_data_channel_open = mp_obj_is_true(is_open_obj);
    
    if (!g_data_channel_open) {
        // Channel closed - clear peer handle to prevent sends to dead connection
        ESP_LOGI(TAG, "DataChannel CLOSED - clearing peer handle");
        g_peer_handle = NULL;
        g_peer_obj = MP_OBJ_NULL;
    }
    
    ESP_LOGI(TAG, "DataChannel state updated: %s", g_data_channel_open ? "OPEN" : "CLOSED");
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(webrepl_rtc_update_channel_state_obj, webrepl_rtc_update_channel_state);

/**
 * webrepl_rtc.notify(payload_json) - Send INFO event over DataChannel
 * 
 * Sends [0, 3, payload_json_string] to connected WebRTC client.
 * Returns True if sent successfully, False if not connected.
 * 
 * The payload must be a JSON string (use json.dumps() in Python).
 * 
 * Example:
 *   import json
 *   webrepl_rtc.notify(json.dumps({"welcome": {"banner": "ScriptO Studio", "tagline": "..."}}))
 */
static mp_obj_t webrepl_rtc_notify(mp_obj_t payload_obj) {
    if (!g_peer_handle || !g_data_channel_open) {
        return mp_const_false;  // Not connected
    }
    
    // Validate that payload is a string (JSON)
    if (!mp_obj_is_str(payload_obj)) {
        ESP_LOGE(TAG, "notify: payload must be a JSON string (use json.dumps())");
        return mp_const_false;
    }
    
    size_t json_len;
    const char *json_str = mp_obj_str_get_data(payload_obj, &json_len);
    
    uint8_t buf[2048];  // Buffer for CBOR encoding
    CborEncoder encoder, arrayEncoder;
    CborError err;
    
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    
    // Create array: [0, 3, payload_json_string]
    err = cbor_encoder_create_array(&encoder, &arrayEncoder, 3);
    if (err != CborNoError) {
        ESP_LOGE(TAG, "Failed to create INFO array: %d", err);
        return mp_const_false;
    }
    
    // Channel 0 (EVENT)
    cbor_encode_uint(&arrayEncoder, WBP_CH_EVENT);
    
    // Event 3 (INFO)
    cbor_encode_uint(&arrayEncoder, 3);
    
    // Encode JSON string as CBOR text string
    err = cbor_encode_text_string(&arrayEncoder, json_str, json_len);
    if (err != CborNoError) {
        ESP_LOGE(TAG, "Failed to encode INFO payload: %d", err);
        return mp_const_false;
    }
    
    err = cbor_encoder_close_container(&encoder, &arrayEncoder);
    if (err != CborNoError) {
        ESP_LOGE(TAG, "Failed to close INFO array: %d", err);
        return mp_const_false;
    }
    
    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    
    // Send via DataChannel
    if (!wbp_rtc_send_cbor(buf, len)) {
        ESP_LOGE(TAG, "Failed to send INFO event via WebRTC");
        return mp_const_false;
    }
    
    ESP_LOGI(TAG, "INFO event sent via WebRTC (%d bytes)", (int)len);
    return mp_const_true;  // Success
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(webrepl_rtc_notify_obj, webrepl_rtc_notify);

/**
 * Python function: webrepl_rtc.log(message, level=1, source=None)
 * Send a LOG event from Python code
 */
static mp_obj_t webrepl_rtc_log(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_message, ARG_level, ARG_source };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_message, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_level, MP_ARG_INT, {.u_int = 1} },
        { MP_QSTR_source, MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    
    if (!g_peer_handle || !g_data_channel_open) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Not connected"));
    }
    
    // Get message string
    size_t msg_len;
    const char *msg = mp_obj_str_get_data(args[ARG_message].u_obj, &msg_len);
    
    // Get source (optional)
    const char *source = NULL;
    if (args[ARG_source].u_obj != mp_const_none) {
        source = mp_obj_str_get_str(args[ARG_source].u_obj);
    }
    
    // Get level (0=DEBUG, 1=INFO, 2=WARN, 3=ERROR)
    uint8_t level = (uint8_t)args[ARG_level].u_int;
    if (level > 3) level = 3;
    
    // Send LOG event
    wbp_send_log(level, (const uint8_t *)msg, msg_len,
                 mp_hal_ticks_ms() / 1000, source);
    
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(webrepl_rtc_log_obj, 0, webrepl_rtc_log);

//=============================================================================
// WebREPL Logging Handler (for Python logging module)
//=============================================================================

// Handler object structure
typedef struct _wbp_log_handler_obj_t {
    mp_obj_base_t base;
    mp_int_t level;  // Handler level threshold
    mp_obj_t formatter;  // Optional formatter (can be None)
} wbp_log_handler_obj_t;

// Map Python logging levels to WBP LOG levels
static uint8_t wbp_map_log_level(mp_int_t python_level) {
    // Python logging levels: DEBUG=10, INFO=20, WARNING=30, ERROR=40, CRITICAL=50
    if (python_level <= 10) return 0;      // DEBUG
    if (python_level <= 20) return 1;      // INFO
    if (python_level <= 30) return 2;      // WARNING
    return 3;                                // ERROR/CRITICAL
}

/**
 * Handler.emit(record) - Send log record to WebREPL LOG channel
 */
static mp_obj_t wbp_log_handler_emit(mp_obj_t self_in, mp_obj_t record) {
    wbp_log_handler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    ESP_LOGD(TAG, "logHandler.emit() called");
    
    if (!g_peer_handle || !g_data_channel_open) {
        ESP_LOGD(TAG, "logHandler: not connected");
        return mp_const_none; // Not connected, silently ignore
    }
    
    // Extract levelno from record
    mp_obj_t levelno_obj = mp_load_attr(record, MP_QSTR_levelno);
    mp_int_t levelno = mp_obj_get_int(levelno_obj);
    
    ESP_LOGD(TAG, "logHandler: levelno=%ld, handler_level=%ld", (long)levelno, (long)self->level);
    
    // Check if this level should be emitted (handler level threshold)
    if (levelno < self->level) {
        ESP_LOGD(TAG, "logHandler: below threshold, ignoring");
        return mp_const_none; // Below threshold, ignore
    }
    
    // Format the message
    mp_obj_t msg_obj;
    if (self->formatter != mp_const_none && self->formatter != NULL) {
        // Use formatter if available
        mp_obj_t format_method = mp_load_attr(self->formatter, MP_QSTR_format);
        msg_obj = mp_call_function_1(format_method, record);
    } else {
        // No formatter - use record.message directly
        msg_obj = mp_load_attr(record, MP_QSTR_message);
    }
    
    // Get message string
    size_t msg_len;
    const char *msg = mp_obj_str_get_data(msg_obj, &msg_len);
    
    // Extract logger name (source) from record
    mp_obj_t name_obj = mp_load_attr(record, MP_QSTR_name);
    const char *source = NULL;
    if (name_obj != mp_const_none) {
        const char *name_str = mp_obj_str_get_str(name_obj);
        // Don't use "root" as source
        if (strcmp(name_str, "root") != 0) {
            source = name_str;
        }
    }
    
    // Map Python logging level to WBP LOG level
    uint8_t wbp_level = wbp_map_log_level(levelno);
    
    // Get timestamp (use record.ct if available, otherwise current time)
    int64_t timestamp = mp_hal_ticks_ms() / 1000;
    mp_obj_t ct_obj = mp_load_attr(record, MP_QSTR_ct);
    if (ct_obj != mp_const_none) {
        // Try to get timestamp from record
        if (mp_obj_is_float(ct_obj)) {
            timestamp = (int64_t)mp_obj_get_float(ct_obj);
        } else if (mp_obj_is_int(ct_obj)) {
            timestamp = mp_obj_get_int(ct_obj);
        }
    }
    
    // Send LOG event
    ESP_LOGD(TAG, "logHandler: sending LOG event: level=%d, msg_len=%d, source=%s", 
             wbp_level, msg_len, source ? source : "NULL");
    wbp_send_log(wbp_level, (const uint8_t *)msg, msg_len,
                 timestamp, source);
    
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(wbp_log_handler_emit_obj, wbp_log_handler_emit);

/**
 * Handler.setLevel(level)
 */
static mp_obj_t wbp_log_handler_set_level(mp_obj_t self_in, mp_obj_t level_obj) {
    wbp_log_handler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->level = mp_obj_get_int(level_obj);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(wbp_log_handler_set_level_obj, wbp_log_handler_set_level);

/**
 * Handler.setFormatter(formatter)
 */
static mp_obj_t wbp_log_handler_set_formatter(mp_obj_t self_in, mp_obj_t formatter_obj) {
    wbp_log_handler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->formatter = formatter_obj;
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(wbp_log_handler_set_formatter_obj, wbp_log_handler_set_formatter);

/**
 * Handler.close()
 */
static mp_obj_t wbp_log_handler_close(mp_obj_t self_in) {
    (void)self_in;
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(wbp_log_handler_close_obj, wbp_log_handler_close);

static mp_obj_t wbp_log_handler_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_val_t vals[1];
    mp_arg_parse_all_kw_array(n_args, n_kw, args, MP_ARRAY_SIZE(vals),
        (mp_arg_t[]) {
            { MP_QSTR_level, MP_ARG_INT, {.u_int = 0} }, // NOTSET = 0
        }, vals);
    
    wbp_log_handler_obj_t *self = m_new_obj(wbp_log_handler_obj_t);
    self->base.type = type;
    self->level = vals[0].u_int;
    self->formatter = mp_const_none;
    
    return MP_OBJ_FROM_PTR(self);
}

// Handler class methods
static const mp_rom_map_elem_t wbp_log_handler_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_emit), MP_ROM_PTR(&wbp_log_handler_emit_obj) },
    { MP_ROM_QSTR(MP_QSTR_setLevel), MP_ROM_PTR(&wbp_log_handler_set_level_obj) },
    { MP_ROM_QSTR(MP_QSTR_setFormatter), MP_ROM_PTR(&wbp_log_handler_set_formatter_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&wbp_log_handler_close_obj) },
};
static MP_DEFINE_CONST_DICT(wbp_log_handler_locals_dict, wbp_log_handler_locals_dict_table);

// Handler type definition
MP_DEFINE_CONST_OBJ_TYPE(
    wbp_rtc_log_handler_type,
    MP_QSTR_logHandler,
    MP_TYPE_FLAG_NONE,
    make_new, wbp_log_handler_make_new,
    locals_dict, &wbp_log_handler_locals_dict
);

//=============================================================================
// Module Definition
//=============================================================================

static const mp_rom_map_elem_t webrepl_rtc_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_webrepl_rtc) },
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&webrepl_rtc_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&webrepl_rtc_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_data), MP_ROM_PTR(&webrepl_rtc_on_data_obj) },
    { MP_ROM_QSTR(MP_QSTR_update_channel_state), MP_ROM_PTR(&webrepl_rtc_update_channel_state_obj) },
    { MP_ROM_QSTR(MP_QSTR_process_queue), MP_ROM_PTR(&wbp_rtc_process_queue_obj) },
    { MP_ROM_QSTR(MP_QSTR_notify), MP_ROM_PTR(&webrepl_rtc_notify_obj) },
    { MP_ROM_QSTR(MP_QSTR_log), MP_ROM_PTR(&webrepl_rtc_log_obj) },
    // Log level constants
    { MP_ROM_QSTR(MP_QSTR_LOG_DEBUG), MP_ROM_INT(0) },
    { MP_ROM_QSTR(MP_QSTR_LOG_INFO), MP_ROM_INT(1) },
    { MP_ROM_QSTR(MP_QSTR_LOG_WARN), MP_ROM_INT(2) },
    { MP_ROM_QSTR(MP_QSTR_LOG_ERROR), MP_ROM_INT(3) },
    // Logging handler for Python logging module
    { MP_ROM_QSTR(MP_QSTR_logHandler), MP_ROM_PTR(&wbp_rtc_log_handler_type) },
};
STATIC MP_DEFINE_CONST_DICT(webrepl_rtc_module_globals, webrepl_rtc_module_globals_table);

const mp_obj_module_t webrepl_rtc_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&webrepl_rtc_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_webrepl_rtc, webrepl_rtc_module);
