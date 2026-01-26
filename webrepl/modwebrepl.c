/*
 * modwebrepl.c - MicroPython WebREPL Binary Protocol (WBP)
 *
 * This implements the WebREPL Binary Protocol using CBOR positional arrays
 * for channel messages, file operations, and events.
 *
 * Protocol Name: WBP (WebREPL Channelized Binary)
 * Subprotocol: webrepl.binary.v1
 * Specification: See docs/webrepl_cb_rfc.md
 *
 * Key Differences from Legacy modwebrepl.c:
 * - No text frames (only binary frames)
 * - No magic bytes (WA/WB) - pure CBOR messages
 * - Positional arrays: [channel, ...fields]
 * - Channel-based message discrimination (not tag byte)
 * - Binary bytecode (.mpy) execution support
 * - Unified file operations (no 82-byte fixed header)
 * - Extensible via optional trailing fields
 *
 * CBOR Library: Uses ESP-IDF component espressif/cbor (TinyCBOR)
 * - Add to project: idf.py add-dependency "espressif/cbor^0.6.1~3"
 * - API docs: https://intel.github.io/tinycbor/current/
 *
 * Copyright (c) 2025 Jonathan Peace
 * SPDX-License-Identifier: MIT
 */

#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "esp_log.h"
#include "esp_task_wdt.h"
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

// ESP HTTP Server (for httpd_handle_t)
#include "esp_http_server.h"

// TinyCBOR library (ESP-IDF component: espressif/cbor)
#include "cbor.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

// External wsserver C API
extern bool wsserver_register_c_callback(int event_type, void *callback_func);
extern httpd_handle_t wsserver_get_handle(void);
extern int wsserver_get_client_sockfd(int client_id);
extern bool wsserver_send_to_client(int client_id, const uint8_t *data, size_t len, bool is_binary);
extern bool wsserver_is_running(void);
extern bool wsserver_start_c(const char *path, int ping_interval, int ping_timeout);

// wsserver event types
#define WSSERVER_EVENT_CONNECT    0
#define WSSERVER_EVENT_DISCONNECT 1
#define WSSERVER_EVENT_MESSAGE    2

static const char *TAG = "WBP";

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

// Event Types (for channel 0)
#define WBP_EVT_AUTH      0  // Authentication
#define WBP_EVT_AUTH_OK   1  // Auth success
#define WBP_EVT_AUTH_FAIL 2  // Auth failure
#define WBP_EVT_INFO      3  // Informational message (CBOR map)
#define WBP_EVT_LOG       4  // Structured log message

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
#define WBP_ERR_FILE_EXISTS  6  // File already exists
#define WBP_ERR_NO_USER      7  // No such user
#define WBP_ERR_OPTION_NEG   8  // Option negotiation failed

// File transfer constants
#define WBP_DEFAULT_BLKSIZE  4096   // 4KB blocks (ESP32 flash sector)
#define WBP_DEFAULT_TIMEOUT  5000   // 5 seconds
#define WBP_MAX_BLOCK_NUM    65535  // 16-bit block number

// Execution formats
#define WBP_FMT_PY   0   // Python source code
#define WBP_FMT_MPY  1   // .mpy bytecode

//=============================================================================
// CBOR Helper: Extract Byte Data (handles tagged typed arrays)
//=============================================================================

/**
 * Safely extract byte data from a CBOR value.
 * 
 * This handles both:
 * - Plain byte strings (CBOR major type 2)
 * - Tagged typed arrays (Tag 64 used by cbor-web for Uint8Array)
 * 
 * If the value is a tag, we skip the tag and extract the inner byte string.
 * This prevents assertion failures when cbor-web encodes Uint8Array with Tag 64.
 * 
 * @param value     Pointer to CborValue to extract from
 * @param data_out  Output pointer for allocated byte data (caller must free)
 * @param len_out   Output for data length
 * @return          CborNoError on success, error code otherwise
 */
static CborError cbor_extract_byte_data(CborValue *value, uint8_t **data_out, size_t *len_out) {
    CborValue inner;
    CborValue *target = value;
    
    // Check if this is a tagged value (e.g., Tag 64 for typed arrays)
    if (cbor_value_is_tag(value)) {
        CborTag tag;
        CborError err = cbor_value_get_tag(value, &tag);
        if (err != CborNoError) {
            ESP_LOGE(TAG, "Failed to get CBOR tag: %d", err);
            return err;
        }
        
        ESP_LOGD(TAG, "CBOR tag detected: %llu", (unsigned long long)tag);
        
        // Skip the tag to get to the inner value
        err = cbor_value_skip_tag(value);
        if (err != CborNoError) {
            ESP_LOGE(TAG, "Failed to skip CBOR tag: %d", err);
            return err;
        }
        
        // Now value points to the inner content
        target = value;
    }
    
    // Check if the (potentially untagged) value is a byte string
    if (!cbor_value_is_byte_string(target)) {
        ESP_LOGE(TAG, "Expected byte string, got CBOR type %d", cbor_value_get_type(target));
        return CborErrorIllegalType;
    }
    
    // Extract the byte string
    return cbor_value_dup_byte_string(target, data_out, len_out, NULL);
}

//=============================================================================
// Client State
//=============================================================================

#define MAX_CLIENTS 4

typedef struct {
    int client_id;
    bool active;
    bool authenticated;
    uint8_t current_channel;  // Active execution channel
} wbp_client_t;

static wbp_client_t wbp_clients[MAX_CLIENTS];
static SemaphoreHandle_t g_wbp_mutex = NULL;
static char wbp_password[64] = "rtyu4567";  // Default password
static mp_obj_t wbp_auth_callback = mp_const_none;  // Python callback for authentication events

// File transfer state (TFTP-inspired)
typedef struct {
    int client_id;
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
    .client_id = -1,
    .file_obj = MP_OBJ_NULL
};

// Output redirection
int g_wbp_output_client_id = -1;  // Made non-static for WebREPLHandler access
static uint8_t g_wbp_current_channel = WBP_CH_TRM;
static char *g_wbp_current_id = NULL; // Current executing message ID

// Execution state
// Execution state
static bool g_wbp_executing = false;

//=============================================================================
// Message Queue System (for GIL-safe execution)
//=============================================================================

#define WBP_QUEUE_SIZE 50
#define WBP_MAX_CMD_LEN 4096

typedef enum {
    WBP_MSG_CONNECT,
    WBP_MSG_DISCONNECT,
    WBP_MSG_RAW_EXEC,
    WBP_MSG_RESET
} wbp_msg_type_t;

typedef struct {
    wbp_msg_type_t type;
    int client_id;
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

//=============================================================================
// Input Ring Buffer (for stdin)
//=============================================================================

#define WBP_INPUT_RING_SIZE 1024  // 1KB input buffer

typedef struct {
    uint8_t buffer[WBP_INPUT_RING_SIZE];
    volatile size_t head;
    volatile size_t tail;
    SemaphoreHandle_t mutex;
    volatile bool active;
} wbp_input_ring_t;

static wbp_input_ring_t g_input_ring = {
    .head = 0,
    .tail = 0,
    .mutex = NULL,
    .active = false
};

static bool wbp_input_ring_init(void) {
    if (g_input_ring.mutex != NULL) {
        xSemaphoreTake(g_input_ring.mutex, portMAX_DELAY);
        g_input_ring.head = 0;
        g_input_ring.tail = 0;
        xSemaphoreGive(g_input_ring.mutex);
        return true;
    }
    g_input_ring.mutex = xSemaphoreCreateMutex();
    if (!g_input_ring.mutex) return false;
    
    g_input_ring.head = 0;
    g_input_ring.tail = 0;
    g_input_ring.active = true;
    return true;
}

static void wbp_input_ring_deinit(void) {
    g_input_ring.active = false;
    if (g_input_ring.mutex) {
        vSemaphoreDelete(g_input_ring.mutex);
        g_input_ring.mutex = NULL;
    }
    g_input_ring.head = 0;
    g_input_ring.tail = 0;
}

static size_t wbp_input_ring_space(void) {
    size_t head = g_input_ring.head;
    size_t tail = g_input_ring.tail;
    if (head >= tail) return WBP_INPUT_RING_SIZE - (head - tail) - 1;
    return tail - head - 1;
}

static size_t wbp_input_ring_available(void) {
    size_t head = g_input_ring.head;
    size_t tail = g_input_ring.tail;
    if (head >= tail) return head - tail;
    return WBP_INPUT_RING_SIZE - tail + head;
}

static size_t wbp_input_ring_write(const uint8_t *data, size_t len) {
    if (!g_input_ring.mutex || !g_input_ring.active) return 0;
    if (xSemaphoreTake(g_input_ring.mutex, pdMS_TO_TICKS(10)) != pdTRUE) return 0;
    
    size_t space = wbp_input_ring_space();
    size_t to_write = (len < space) ? len : space;
    
    for (size_t i = 0; i < to_write; i++) {
        g_input_ring.buffer[g_input_ring.head] = data[i];
        g_input_ring.head = (g_input_ring.head + 1) % WBP_INPUT_RING_SIZE;
    }
    
    xSemaphoreGive(g_input_ring.mutex);
    return to_write;
}

static size_t wbp_input_ring_read(uint8_t *data, size_t max_len) {
    if (!g_input_ring.mutex || !g_input_ring.active) return 0;
    if (xSemaphoreTake(g_input_ring.mutex, pdMS_TO_TICKS(10)) != pdTRUE) return 0;
    
    size_t available = wbp_input_ring_available();
    size_t to_read = (max_len < available) ? max_len : available;
    
    for (size_t i = 0; i < to_read; i++) {
        data[i] = g_input_ring.buffer[g_input_ring.tail];
        g_input_ring.tail = (g_input_ring.tail + 1) % WBP_INPUT_RING_SIZE;
    }
    
    xSemaphoreGive(g_input_ring.mutex);
    return to_read;
}

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
    int client_id;
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
    wbp_stream_type,
    MP_QSTR_WBPStream,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    protocol, &wbp_stream_p
);

// Global stream object
static wbp_stream_obj_t *g_wbp_stream = NULL;

//=============================================================================
// Helper Functions
//=============================================================================

static int find_client_by_id(int client_id) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (wbp_clients[i].active && wbp_clients[i].client_id == client_id) {
            return i;
        }
    }
    return -1;
}

static int find_free_client_slot() {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!wbp_clients[i].active) {
            return i;
        }
    }
    return -1;
}

/**
 * Send raw CBOR data to client
 * Returns false if send failed (caller should stop sending to this client)
 */
static bool wbp_send_cbor(int client_id, const uint8_t *data, size_t len) {
    if (client_id < 0 || !data || len == 0) {
        return false;
    }
    
    bool success = wsserver_send_to_client(client_id, data, len, true);
    if (!success) {
        ESP_LOGE(TAG, "Failed to send to client %d", client_id);
        // Note: wsserver_send_to_client handles client cleanup on fatal errors
    }
    
    return success;
}

//=============================================================================
// WBP Message Builders (using TinyCBOR)
//=============================================================================

/**
 * Send channel result: [ch, 0, data, ?id]
 */
static void wbp_send_result(int client_id, uint8_t channel, const uint8_t *data, size_t len, const char *id) {
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
    
    ESP_LOGD(TAG, "wbp_send_result: ch=%d len=%d id_len=%d", channel, (int)len, (int)id_len);
    
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
    wbp_send_cbor(client_id, buf, encoded_size);
    free(buf);
}

/**
 * Send progress: [ch, 2, status, ?error, ?id]
 */
static void wbp_send_progress(int client_id, uint8_t channel, int status, const char *error, const char *id) {
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
    wbp_send_cbor(client_id, buf, encoded_size);
}

/**
 * Send continuation: [ch, 1]
 */
static void __attribute__((unused)) wbp_send_continuation(int client_id, uint8_t channel) {
    uint8_t buf[1024];
    CborEncoder encoder, arrayEncoder;
    
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    
    cbor_encoder_create_array(&encoder, &arrayEncoder, 2);
    cbor_encode_uint(&arrayEncoder, channel);
    cbor_encode_uint(&arrayEncoder, WBP_OP_CON);
    cbor_encoder_close_container(&encoder, &arrayEncoder);
    
    size_t encoded_size = cbor_encoder_get_buffer_size(&encoder, buf);
    wbp_send_cbor(client_id, buf, encoded_size);
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
 * Handles formatted output from print_completions() which may include
 * multiple completions per line separated by spaces
 */
static void completion_print_strn(void *data, const char *str, size_t len) {
    completion_collector_t *collector = (completion_collector_t *)data;
    if (!collector) return;
    
    // Parse the formatted output - completions may be separated by spaces
    // Format: "    completion1    completion2\n" or "\ncompletion\n"
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
 * Just collects raw completions from MicroPython - client will process them
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
static void wbp_send_completions(int client_id, uint8_t channel, 
                                 completion_collector_t *collector) {
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
    wbp_send_cbor(client_id, buf, encoded_size);
    free(buf);
}

/**
 * Send AUTH_OK event: [0, 1, ?token, ?expires]
 */
static void wbp_send_auth_ok(int client_id) {
    uint8_t buf[512];
    CborEncoder encoder, arrayEncoder;
    
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    cbor_encoder_create_array(&encoder, &arrayEncoder, 2);
    cbor_encode_uint(&arrayEncoder, WBP_CH_EVENT);
    cbor_encode_uint(&arrayEncoder, WBP_EVT_AUTH_OK);
    cbor_encoder_close_container(&encoder, &arrayEncoder);
    
    size_t encoded_size = cbor_encoder_get_buffer_size(&encoder, buf);
    wbp_send_cbor(client_id, buf, encoded_size);
}

/**
 * Send AUTH_FAIL event: [0, 2, error]
 */
static void wbp_send_auth_fail(int client_id, const char *error) {
    uint8_t buf[512];
    CborEncoder encoder, arrayEncoder;
    
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    cbor_encoder_create_array(&encoder, &arrayEncoder, 3);
    cbor_encode_uint(&arrayEncoder, WBP_CH_EVENT);
    cbor_encode_uint(&arrayEncoder, WBP_EVT_AUTH_FAIL);
    cbor_encode_text_string(&arrayEncoder, error, strlen(error));
    cbor_encoder_close_container(&encoder, &arrayEncoder);
    
    size_t encoded_size = cbor_encoder_get_buffer_size(&encoder, buf);
    wbp_send_cbor(client_id, buf, encoded_size);
}


/**
 * Helper: Recursively encode a MicroPython object to CBOR
 * Supports: None, bool, int, str, bytes, list, dict
 * Returns CborNoError on success, error code on failure
 */
/**
 * Send INFO event with JSON string payload: [0, 3, payload_json_string]
 * 
 * @param client_id WebSocket client ID
 * @param payload_obj MicroPython string object containing JSON
 */
static void wbp_send_info_json(int client_id, mp_obj_t payload_obj) {
    uint8_t buf[2048];  // Buffer for CBOR encoding
    CborEncoder encoder, arrayEncoder;
    CborError err;
    
    if (client_id < 0) {
        return;
    }
    
    // Validate that payload is a string (JSON)
    if (!mp_obj_is_str(payload_obj)) {
        ESP_LOGE(TAG, "send_info: payload must be a JSON string (use json.dumps())");
        return;
    }
    
    size_t json_len;
    const char *json_str = mp_obj_str_get_data(payload_obj, &json_len);
    
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    
    // Create array: [0, 3, payload_json_string]
    err = cbor_encoder_create_array(&encoder, &arrayEncoder, 3);
    if (err != CborNoError) {
        ESP_LOGE(TAG, "Failed to create INFO array: %d", err);
        return;
    }
    
    cbor_encode_uint(&arrayEncoder, WBP_CH_EVENT);  // Channel 0
    cbor_encode_uint(&arrayEncoder, WBP_EVT_INFO);  // Event type 3
    
    // Encode JSON string as CBOR text string
    err = cbor_encode_text_string(&arrayEncoder, json_str, json_len);
    if (err != CborNoError) {
        ESP_LOGE(TAG, "Failed to encode INFO payload: %d", err);
        return;
    }
    
    err = cbor_encoder_close_container(&encoder, &arrayEncoder);
    if (err != CborNoError) {
        ESP_LOGE(TAG, "Failed to close INFO array: %d", err);
        return;
    }
    
    size_t encoded_size = cbor_encoder_get_buffer_size(&encoder, buf);
    wbp_send_cbor(client_id, buf, encoded_size);
}

/**
 * Python API: webrepl.notify(payload_json)
 * 
 * Sends a notification [0, 3, payload_json_string] to the connected client.
 * The payload must be a JSON string (use json.dumps() in Python).
 * 
 * Example:
 *   import json
 *   webrepl.notify(json.dumps({"welcome": "ScriptO Studio"}))
 *   webrepl.notify(json.dumps({"eth_status": {"linkup": True, "ip": "192.168.1.100"}}))
 */
static mp_obj_t wbp_notify_mp(mp_obj_t payload_obj) {
    // Send to the active REPL client
    if (g_wbp_output_client_id >= 0) {
        wbp_send_info_json(g_wbp_output_client_id, payload_obj);
        return mp_const_true;
    }
    return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_1(wbp_notify_obj, wbp_notify_mp);

/**
 * Send LOG event: [0, 4, level, message, ?timestamp, ?source]
 * Updated to handle binary data (not just null-terminated strings)
 */
void wbp_send_log(int client_id, uint8_t level, const uint8_t *message, size_t message_len,
                  int64_t timestamp, const char *source) {
    // Increase buffer size for larger messages
    uint8_t buf[4096];
    CborEncoder encoder, arrayEncoder;
    CborError err;
    
    if (client_id < 0 || !message || message_len == 0) {
        return;
    }
    
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
    // Use message_len instead of strlen for binary-safe encoding
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
    wbp_send_cbor(client_id, buf, encoded_size);
}

/**
 * Send ACK packet: [23, 4, block#, ?tsize, ?mtime, ?mode]
 * ACK 0 for WRQ: [23, 4, 0, tsize, ?blksize]
 * ACK 0 for RRQ: [23, 4, 0, tsize, ?mtime, ?mode]
 * ACK N: [23, 4, block#]
 */
static void wbp_send_file_ack(int client_id, uint16_t block_num, size_t tsize, 
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
    wbp_send_cbor(client_id, buf, encoded_size);
}

/**
 * Send DATA packet: [23, 3, block#, data]
 */
static void wbp_send_file_data(int client_id, uint16_t block_num, const uint8_t *data, size_t data_len) {
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
    wbp_send_cbor(client_id, buf, encoded_size);
    free(buf);
}

/**
 * Send ERROR packet: [23, 5, error_code, error_msg]
 */
static void wbp_send_file_error(int client_id, uint8_t error_code, const char *error_msg) {
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
    wbp_send_cbor(client_id, buf, encoded_size);
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
    size_t head = g_output_ring.head;
    size_t tail = g_output_ring.tail;
    
    if (head >= tail) {
        return head - tail;
    } else {
        return WBP_OUTPUT_RING_SIZE - tail + head;
    }
}

static bool wbp_ring_init(void) {
    if (g_output_ring.mutex != NULL) {
        xSemaphoreTake(g_output_ring.mutex, portMAX_DELAY);
        g_output_ring.head = 0;
        g_output_ring.tail = 0;
        xSemaphoreGive(g_output_ring.mutex);
        ESP_LOGI(TAG, "Ring buffer already initialized, reset");
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
    
    ESP_LOGI(TAG, "Ring buffer initialized (%d bytes)", WBP_OUTPUT_RING_SIZE);
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
    
    ESP_LOGI(TAG, "Ring buffer deinitialized");
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
        xSemaphoreGive(g_output_ring.data_available);
    }
    
    return to_write;
}

static size_t wbp_ring_read(uint8_t *data, size_t max_len) {
    if (!g_output_ring.mutex) {
        return 0;
    }
    
    if (xSemaphoreTake(g_output_ring.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;
    }
    
    size_t available = wbp_ring_available();
    size_t to_read = (max_len < available) ? max_len : available;
    
    if (to_read == 0) {
        xSemaphoreGive(g_output_ring.mutex);
        return 0;
    }
    
    for (size_t i = 0; i < to_read; i++) {
        data[i] = g_output_ring.buffer[g_output_ring.tail];
        g_output_ring.tail = (g_output_ring.tail + 1) % WBP_OUTPUT_RING_SIZE;
    }
    
    xSemaphoreGive(g_output_ring.mutex);
    
    return to_read;
}

//=============================================================================
// Drain Task - Sends ring buffer data to WebSocket
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
        xSemaphoreTake(g_output_ring.data_available, pdMS_TO_TICKS(20));
        
        // Batching delay: Wait 20ms for more data to accumulate before sending
        // This prevents character-by-character transmission and improves network efficiency
        vTaskDelay(pdMS_TO_TICKS(20));
        
        while (g_output_ring.active) {
            size_t bytes_read = wbp_ring_read(chunk, DRAIN_CHUNK_SIZE);
            
            if (bytes_read == 0) {
                break;
            }
            
            int client_id = g_wbp_output_client_id;
            if (client_id >= 0) {
                // Pass current message ID if execution is active
                wbp_send_result(client_id, g_wbp_current_channel, chunk, bytes_read, g_wbp_current_id);
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
        "wbp_drain",
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
    
    if (!g_input_ring.active) {
        *errcode = MP_EAGAIN;
        return MP_STREAM_ERROR;
    }
    
    size_t read = wbp_input_ring_read((uint8_t *)buf, size);
    
    if (read == 0) {
        *errcode = MP_EAGAIN;
        return MP_STREAM_ERROR;
    }
    
    *errcode = 0;
    return read;
}

static mp_uint_t wbp_stream_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode) {
    (void)self_in;
    
    const uint8_t *data = (const uint8_t *)buf;
    
    if (!g_output_ring.active || g_wbp_output_client_id < 0) {
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
        
        if (!g_output_ring.active || g_wbp_output_client_id < 0) {
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
        if ((flags & MP_STREAM_POLL_RD) && g_input_ring.active && wbp_input_ring_available() > 0) {
            ret |= MP_STREAM_POLL_RD;
        }
        return ret;
    }
    
    *errcode = MP_EINVAL;
    return MP_STREAM_ERROR;
}

// Attach dupterm
static void wbp_dupterm_attach(int client_id) {
    if (g_wbp_stream == NULL) {
        g_wbp_stream = m_new_obj(wbp_stream_obj_t);
        g_wbp_stream->base.type = &wbp_stream_type;
        g_wbp_stream->client_id = client_id;
    }
    
    g_wbp_stream->client_id = client_id;
    
    // Call os.dupterm(stream, 2) using the function object
    // Slot 2 is for WSS (legacy), Slot 1 is for WebRTC, Slot 0 is for UART
    mp_obj_t dupterm_args[2] = {
        MP_OBJ_FROM_PTR(g_wbp_stream),
        MP_OBJ_NEW_SMALL_INT(2)
    };
    mp_call_function_n_kw(MP_OBJ_FROM_PTR(&mp_os_dupterm_obj), 2, 0, dupterm_args);
    
    ESP_LOGI(TAG, "Dupterm attached for client %d", client_id);
}

// Detach dupterm from slot 2
static void wbp_dupterm_detach(void) {
    // Call os.dupterm(None, 2) using the function object
    mp_obj_t dupterm_args[2] = {
        mp_const_none,
        MP_OBJ_NEW_SMALL_INT(2)
    };
    mp_call_function_n_kw(MP_OBJ_FROM_PTR(&mp_os_dupterm_obj), 2, 0, dupterm_args);
    
    ESP_LOGI(TAG, "Dupterm detached");
}

//=============================================================================
// File I/O Handlers (Direct GIL acquisition from HTTP worker thread)
//=============================================================================

extern bool httpserver_ensure_mp_thread_state(void);

static void wbp_close_transfer(void) {
    if (g_file_transfer.active) {
        if (g_file_transfer.file_obj != MP_OBJ_NULL) {
            if (httpserver_ensure_mp_thread_state()) {
                // Must ensure we are in a valid MP thread state to close
                mp_stream_close(g_file_transfer.file_obj);
            }
            g_file_transfer.file_obj = MP_OBJ_NULL;
        }
        g_file_transfer.active = false;
        g_file_transfer.client_id = -1;
        ESP_LOGI(TAG, "File transfer closed");
    }
}

// Handle WRQ (Client wants to upload)
static void wbp_handle_wrq(int client_id, const char *path, size_t tsize, size_t blksize, size_t timeout, int64_t mtime) {
    if (g_file_transfer.active) {
        wbp_send_file_error(client_id, WBP_ERR_ACCESS, "Transfer already in progress");
        return;
    }

    if (!httpserver_ensure_mp_thread_state()) {
        wbp_send_file_error(client_id, WBP_ERR_UNDEFINED, "Internal MP error");
        return;
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t file = mp_call_function_2(MP_OBJ_FROM_PTR(&mp_builtin_open_obj),
                                            mp_obj_new_str(path, strlen(path)),
                                            mp_obj_new_str("wb", 2));
        
        g_file_transfer.active = true;
        g_file_transfer.client_id = client_id;
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
        wbp_send_file_ack(client_id, 0, tsize, g_file_transfer.blksize, -1, 0);
        ESP_LOGI(TAG, "WRQ accepted: %s (%d bytes)", path, (int)tsize);
    } else {
        wbp_send_file_error(client_id, WBP_ERR_ACCESS, "Could not open file for writing");
    }
}

// Handle RRQ (Client wants to download)
static void wbp_handle_rrq(int client_id, const char *path, size_t blksize, size_t timeout) {
    if (g_file_transfer.active) {
        wbp_send_file_error(client_id, WBP_ERR_ACCESS, "Transfer already in progress");
        return;
    }

    if (!httpserver_ensure_mp_thread_state()) {
        wbp_send_file_error(client_id, WBP_ERR_UNDEFINED, "Internal MP error");
        return;
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        // Open file
        mp_obj_t file = mp_call_function_2(MP_OBJ_FROM_PTR(&mp_builtin_open_obj),
                                            mp_obj_new_str(path, strlen(path)),
                                            mp_obj_new_str("rb", 2));
        
        // Get file size using os.stat (or seek/tell which is expensive)
        // For simplicity, we'll try os.stat logic if available, or just read until EOF.
        // But RFC says we should send tsize in ACK 0.
        // Let's use `mp_os_stat` logic or similar. 
        // Simpler way: seek to end, tell, seek to start.
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
        g_file_transfer.client_id = client_id;
        g_file_transfer.opcode = WBP_FILE_RRQ;
        strncpy(g_file_transfer.path, path, sizeof(g_file_transfer.path)-1);
        g_file_transfer.file_obj = file;
        g_file_transfer.tsize = size;
        g_file_transfer.blksize = (blksize > 0 && blksize <= 16384) ? blksize : WBP_DEFAULT_BLKSIZE;
        g_file_transfer.next_block = 1; // Wait for ACK 0 from client before sending block 1?
        // Wait, RFC says: Server sends ACK 0 with metadata. Client sends ACK 0. THEN Server sends Block 1.
        // So we expect ACK 0 next.
        g_file_transfer.next_block = 0; 
        g_file_transfer.bytes_transferred = 0;
        g_file_transfer.last_activity = mp_hal_ticks_ms();

        nlr_pop();

        // Send ACK 0 with metadata
        wbp_send_file_ack(client_id, 0, size, 0, 0, 0); // TODO: mtime?
        ESP_LOGI(TAG, "RRQ accepted: %s (%d bytes)", path, (int)size);

    } else {
        wbp_send_file_error(client_id, WBP_ERR_NOT_FOUND, "File not found or unreadable");
    }
}

// Handle DATA block (from Client during Upload/WRQ)
static void wbp_handle_data(int client_id, uint16_t block_num, const uint8_t *data, size_t data_len) {
    if (!g_file_transfer.active || g_file_transfer.client_id != client_id || g_file_transfer.opcode != WBP_FILE_WRQ) {
        wbp_send_file_error(client_id, WBP_ERR_UNKNOWN_TID, "No active upload");
        return;
    }

    if (block_num != g_file_transfer.next_block) {
        // Resend ACK for previous block (lost ACK scenario) or error?
        // Helper: if block == next_block - 1, resend ACK.
        if (block_num == g_file_transfer.next_block - 1) {
             wbp_send_file_ack(client_id, block_num, 0, 0, -1, 0);
             return;
        }
        wbp_send_file_error(client_id, WBP_ERR_UNKNOWN_TID, "Invalid block number");
        return;
    }

    if (!httpserver_ensure_mp_thread_state()) {
        wbp_send_file_error(client_id, WBP_ERR_UNDEFINED, "Internal MP error");
        wbp_close_transfer();
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
        wbp_send_file_ack(client_id, block_num, 0, 0, -1, 0);
        
        g_file_transfer.next_block++;
        
        // Check for completion
        if (data_len < g_file_transfer.blksize) {
            ESP_LOGI(TAG, "Upload complete: %s (%d bytes)", g_file_transfer.path, (int)g_file_transfer.bytes_transferred);
            wbp_close_transfer();
        }

    } else {
        wbp_send_file_error(client_id, WBP_ERR_DISK_FULL, "Write failed");
        wbp_close_transfer();
    }
}

// Handle ACK (from Client during Download/RRQ)
static void wbp_handle_ack(int client_id, uint16_t block_num) {
    if (!g_file_transfer.active || g_file_transfer.client_id != client_id || g_file_transfer.opcode != WBP_FILE_RRQ) {
        // Ignore stray ACKs
        return;
    }

    // We expect ACK for the block we just sent (or ACK 0 to start)
    // Actually, g_file_transfer.next_block tracks what we should send NEXT.
    // If we sent block 1, next_block is 2. We wait for ACK 1.
    // So we expect block_num == g_file_transfer.next_block - 1.
    // Spec:
    // RRQ -> ACK 0 (meta).
    // Client sends ACK 0. -> Server sends Block 1.
    
    // Initial state: next_block = 0. We sent ACK 0 (Meta).
    // Expect ACK 0 from client.
    
    if (block_num != g_file_transfer.next_block) {
        ESP_LOGW(TAG, "Unexpected ACK %d (expected %d)", block_num, g_file_transfer.next_block);
        return;
    }

    g_file_transfer.last_activity = mp_hal_ticks_ms();

    // Send NEXT block (block_num + 1)
    uint16_t send_block = block_num + 1;
    
    if (!httpserver_ensure_mp_thread_state()) {
        wbp_send_file_error(client_id, WBP_ERR_UNDEFINED, "Internal MP error");
        wbp_close_transfer();
        return;
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t read_method = mp_load_attr(g_file_transfer.file_obj, MP_QSTR_read);
        mp_obj_t size_obj = MP_OBJ_NEW_SMALL_INT(g_file_transfer.blksize);
        mp_obj_t data_obj = mp_call_function_1(read_method, size_obj);
        
        mp_buffer_info_t bufinfo;
        mp_get_buffer_raise(data_obj, &bufinfo, MP_BUFFER_READ);
        
        wbp_send_file_data(client_id, send_block, bufinfo.buf, bufinfo.len);
        
        nlr_pop();
        
        g_file_transfer.next_block = send_block; // Wait for ACK of this block
        g_file_transfer.bytes_transferred += bufinfo.len;

        // Check EOF
        if (bufinfo.len < g_file_transfer.blksize) {
            ESP_LOGI(TAG, "Download complete sent: %s", g_file_transfer.path);
            wbp_close_transfer();
        }

    } else {
        wbp_send_file_error(client_id, WBP_ERR_ACCESS, "Read failed");
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

static void wbp_queue_message(wbp_msg_type_t type, int client_id, uint8_t channel, 
                                const char *data, size_t data_len, const char *id, bool is_bytecode) {

    wbp_queue_msg_t msg = {
        .type = type,
        .client_id = client_id,
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
    }
}

// Process queue (called from MP main task)
static mp_obj_t wbp_process_queue(void) {
    // Check if queue is initialized - fail gracefully if not
    if (!wbp_msg_queue) {
        // Queue not initialized - return 0 processed (no error, just nothing to do)
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
        
        switch (msg.type) {
            case WBP_MSG_RAW_EXEC: {
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
                                // Clear exception
                                mp_obj_t exc = MP_OBJ_FROM_PTR(nlr2.ret_val);
                                (void)exc; // Suppress unused warning
                                success = false;
                            }
                        }
                        
                        // If not successful (failed SINGLE attempt OR non-terminal channel), use FILE_INPUT
                        // FILE_INPUT supports multi-line scripts but does not auto-print last expression
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
                    
                    // Flush any remaining output to current channel before switching back
                    if (g_wbp_output_client_id >= 0) {
                        uint8_t flush_buf[256];
                        size_t available;
                        while ((available = wbp_ring_available()) > 0) {
                            size_t to_read = (available > sizeof(flush_buf)) ? sizeof(flush_buf) : available;
                            size_t read = wbp_ring_read(flush_buf, to_read);
                            if (read > 0) {
                                wbp_send_result(g_wbp_output_client_id, g_wbp_current_channel, 
                                                flush_buf, read, g_wbp_current_id);
                            } else {
                                break;
                            }
                        }
                    }
                    
                    // Send success with ID
                    wbp_send_progress(msg.client_id, msg.channel, 0, NULL, msg.id);
                } else {
                    mp_obj_t exc = MP_OBJ_FROM_PTR(nlr.ret_val);
                    
                    bool is_interrupt = mp_obj_exception_match(exc, &mp_type_KeyboardInterrupt);
                    
                    if (!is_interrupt) {
                        mp_obj_print_exception(&mp_plat_print, exc);
                    }
                    
                    // Flush output (including exception traceback)
                    if (g_wbp_output_client_id >= 0) {
                        uint8_t flush_buf[256];
                        size_t available;
                        while ((available = wbp_ring_available()) > 0) {
                            size_t to_read = (available > sizeof(flush_buf)) ? sizeof(flush_buf) : available;
                            size_t read = wbp_ring_read(flush_buf, to_read);
                            if (read > 0) {
                                wbp_send_result(g_wbp_output_client_id, g_wbp_current_channel, 
                                                flush_buf, read, g_wbp_current_id);
                            } else {
                                break;
                            }
                        }
                    }
                    
                    // Yield to allow drain task to send flush data BEFORE we send result
                    // This fixes the "Prompt before Output" race condition
                    vTaskDelay(pdMS_TO_TICKS(50));
                    
                    if (is_interrupt) {
                        // Treat KeyboardInterrupt as a successful "Stop" - no error message
                        wbp_send_progress(msg.client_id, msg.channel, 0, NULL, msg.id);
                    } else {
                        // Send error with ID
                        wbp_send_progress(msg.client_id, msg.channel, 1, "Execution failed", msg.id);
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
                    // This is critical so that recover() doesn't usage invalid pointers
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
static MP_DEFINE_CONST_FUN_OBJ_0(wbp_process_queue_obj, wbp_process_queue);

static mp_obj_t wbp_send(mp_obj_t data_obj) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_obj, &bufinfo, MP_BUFFER_READ);
    
    if (g_wbp_output_client_id >= 0) {
        // Send using current channel AND current ID if active
        wbp_send_result(g_wbp_output_client_id, g_wbp_current_channel, 
                        bufinfo.buf, bufinfo.len, g_wbp_current_id);
        return mp_const_true;
    }
    return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_1(wbp_send_obj, wbp_send);

/**
 * Python function: webrepl.log(message, level=1, source=None)
 * Send a LOG event from Python code
 */
static mp_obj_t wbp_log(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_message, ARG_level, ARG_source };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_message, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_level, MP_ARG_INT, {.u_int = 1} },
        { MP_QSTR_source, MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    
    if (g_wbp_output_client_id < 0) {
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
    wbp_send_log(g_wbp_output_client_id, level, (const uint8_t *)msg, msg_len,
                 mp_hal_ticks_ms() / 1000, source);
    
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(wbp_log_obj, 0, wbp_log);

/**
 * Handle channel message: [ch, op, ...fields]
 */
static void handle_channel_message(int client_id, uint8_t channel, const uint8_t *buf, size_t len) {
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
    
    // Check authentication
    int client_idx = find_client_by_id(client_id);
    if (client_idx < 0 || !wbp_clients[client_idx].authenticated) {
        ESP_LOGW(TAG, "Client not authenticated");
        wbp_send_progress(client_id, channel, 1, "Not authenticated", NULL);
        return;
    }
    
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
                wbp_send_progress(client_id, channel, 1, "Invalid data type", NULL);
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
                
                if (httpserver_ensure_mp_thread_state()) {
                    wbp_collect_completions(code, code_len - 1, &collector);
                }
                
                wbp_send_completions(client_id, channel, &collector);
                completion_collector_free(&collector);
                
                free((void *)code);
                if (id) free(id);
                break;
            }

            // Hybrid Dispatch Logic for Terminal Channel
            ESP_LOGI(TAG, "TRM Message: len=%d, executing=%d", (int)code_len, g_wbp_executing);
            
            if (channel == WBP_CH_TRM && !is_bytecode && g_wbp_executing) {
                // Scenario: Script is running (and possibly blocked on input()). 
                // We MUST feed this data to stdin via dupterm.
                
                size_t written = wbp_input_ring_write((const uint8_t *)code, code_len);
                (void)written; // Ignored
                
                // Trigger dupterm notification via Python call to os.dupterm_notify(None)
                // UNSAFE from HTTP Task! The Main Task is blocked in input(), so we cannot 
                // touch the VM state here.
                // Assuming mp_hal_readline polls efficiently, simply writing to the ring buffer 
                // (above) should be enough.
                /*
                if (httpserver_ensure_mp_thread_state()) {
                    nlr_buf_t nlr;
                    if (nlr_push(&nlr) == 0) {
                        // Lookup os via mp_module_os (if available) or generic lookup
                        // Safe approach: Use mp_import_name to load 'os' module
                        // mp_import_name(name, fromlist, level)
                        mp_obj_t os_module = mp_import_name(MP_QSTR_os, mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
                        mp_obj_t notify_func = mp_load_attr(os_module, MP_QSTR_dupterm_notify);
                        
                        if (notify_func != MP_OBJ_NULL) {
                            mp_call_function_1(notify_func, mp_const_none);
                        }
                        nlr_pop();
                    } else {
                        // Ignore exception during notify lookup (e.g. os module not init?)
                        MP_STATE_THREAD(mp_pending_exception) = MP_OBJ_NULL; // clear pending
                    }
                }
                */
                
                // Free data and done
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
            
            // Queue for GIL-safe execution (Standard path for M2M or Idle TRM)
            wbp_queue_message(WBP_MSG_RAW_EXEC, client_id, channel, code, code_len, id, is_bytecode);
            
            // Free duplicated strings
            free((void *)code);
            if (id) free(id);
            break;
        }
        
        case WBP_OP_INT: {
            ESP_LOGI(TAG, "Interrupt on ch=%d", channel);
            mp_sched_keyboard_interrupt();
            wbp_send_progress(client_id, channel, 1, "KeyboardInterrupt", NULL);
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
            // We use the 'channel' field of the message struct to store the reset mode (1=Hard, 0=Soft)
            wbp_queue_message(WBP_MSG_RESET, client_id, (uint8_t)mode, NULL, 0, NULL, false);
            break;
        }
        
        default:
            ESP_LOGW(TAG, "Unknown opcode: %d", opcode);
            break;
    }
}

/**
 * Handle file operation: [23, opcode, ...fields]
 */
static void handle_file_message(int client_id, const uint8_t *buf, size_t len) {
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
    
    // ESP_LOGD(TAG, "File op=%d", opcode);
    
    switch (opcode) {
        case WBP_FILE_WRQ: { // [23, 2, filename, tsize, ?blksize, ?timeout, ?mtime]
            char *path = NULL;
            size_t path_len;
            if (cbor_value_dup_text_string(&arrayValue, &path, &path_len, NULL) != CborNoError) {
                wbp_send_file_error(client_id, WBP_ERR_ILLEGAL_OP, "Invalid filename");
                return;
            }
            cbor_value_advance(&arrayValue);
            
            uint64_t tsize = 0;
            if (cbor_value_get_uint64(&arrayValue, &tsize) != CborNoError) {
                wbp_send_file_error(client_id, WBP_ERR_ILLEGAL_OP, "Invalid size");
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
            
            // Timeout and mtime ignored for now
            
            wbp_handle_wrq(client_id, path, (size_t)tsize, (size_t)blksize, 0, 0);
            free(path);
            break;
        }
        
        case WBP_FILE_RRQ: { // [23, 1, filename, ?blksize, ?timeout]
            char *path = NULL;
            size_t path_len;
            if (cbor_value_dup_text_string(&arrayValue, &path, &path_len, NULL) != CborNoError) {
                wbp_send_file_error(client_id, WBP_ERR_ILLEGAL_OP, "Invalid filename");
                return;
            }
            cbor_value_advance(&arrayValue);
            
            uint64_t blksize = 0;
            if (!cbor_value_at_end(&arrayValue)) {
                cbor_value_get_uint64(&arrayValue, &blksize);
            }
            
            wbp_handle_rrq(client_id, path, (size_t)blksize, 0);
            free(path);
            break;
        }
        
        case WBP_FILE_DATA: { // [23, 3, block#, data]
            uint64_t block_num;
            if (cbor_value_get_uint64(&arrayValue, &block_num) != CborNoError) return;
            cbor_value_advance(&arrayValue);
            
            uint8_t *data = NULL;
            size_t data_len;
            // Use helper to handle both plain byte strings and tagged typed arrays (cbor-web)
            CborError data_err = cbor_extract_byte_data(&arrayValue, &data, &data_len);
            if (data_err != CborNoError) {
                ESP_LOGE(TAG, "FILE_DATA: Invalid data format (CBOR error %d)", data_err);
                wbp_send_file_error(client_id, WBP_ERR_ILLEGAL_OP, "Invalid data format");
                return;
            }
            
            wbp_handle_data(client_id, (uint16_t)block_num, data, data_len);
            free(data);
            break;
        }
        
        case WBP_FILE_ACK: { // [23, 4, block#]
            uint64_t block_num;
            if (cbor_value_get_uint64(&arrayValue, &block_num) != CborNoError) return;
            
            wbp_handle_ack(client_id, (uint16_t)block_num);
            break;
        }
        
        case WBP_FILE_ERROR: {
            // Client sent error, abort transfer
            if (g_file_transfer.active && g_file_transfer.client_id == client_id) {
                ESP_LOGW(TAG, "Client sent error, aborting transfer");
                wbp_close_transfer();
            }
            break;
        }
        
        default:
            wbp_send_file_error(client_id, WBP_ERR_ILLEGAL_OP, "Unknown opcode");
            break;
    }
}

/**
 * Handle event message: [23, event, ...fields]
 */
static void handle_event_message(int client_id, const uint8_t *buf, size_t len) {
    CborParser parser;
    CborValue value, arrayValue;
    
    if (cbor_parser_init(buf, len, 0, &parser, &value) != CborNoError) {
        ESP_LOGE(TAG, "Failed to parse CBOR");
       return;
    }
    
    if (!cbor_value_is_array(&value)) {
        ESP_LOGE(TAG, "Invalid event message (not array)");
        return;
    }
    
    cbor_value_enter_container(&value, &arrayValue);
    
    // Read channel (23 for events)
    uint64_t channel;
    cbor_value_get_uint64(&arrayValue, &channel);
    cbor_value_advance(&arrayValue);
    
    // Read event type
    uint64_t event_u64;
    cbor_value_get_uint64(&arrayValue, &event_u64);
    cbor_value_advance(&arrayValue);
    uint8_t event = event_u64;
    
    ESP_LOGI(TAG, "Event type=%d", event);
    
    switch (event) {
        case WBP_EVT_AUTH: {
            // Read password
            char *password = NULL;
            size_t pwd_len;
            if (cbor_value_dup_text_string(&arrayValue, &password, &pwd_len, NULL) != CborNoError) {
                wbp_send_auth_fail(client_id, "Invalid password format");
                return;
            }
            
            ESP_LOGI(TAG, "Auth attempt with password: %s", password);
            
            // Check password
            if (strcmp(password, wbp_password) == 0) {
                int client_idx = find_client_by_id(client_id);
                if (client_idx >= 0) {
                    wbp_clients[client_idx].authenticated = true;
                    g_wbp_output_client_id = client_id;
                    
                    // Attach dupterm for stdout/print() capture
                    wbp_dupterm_attach(client_id);
                    
                    ESP_LOGI(TAG, "Client %d authenticated", client_id);
                    wbp_send_auth_ok(client_id);
                    
                    // Call Python authentication callback if registered
                    if (wbp_auth_callback != mp_const_none && mp_obj_is_callable(wbp_auth_callback)) {
                        ESP_LOGD(TAG, "Calling Python auth callback for client %d", client_id);
                        nlr_buf_t nlr;
                        if (nlr_push(&nlr) == 0) {
                            mp_obj_t args[1];
                            args[0] = mp_obj_new_int(client_id);
                            mp_call_function_n_kw(wbp_auth_callback, 1, 0, args);
                            nlr_pop();
                            ESP_LOGD(TAG, "Auth callback executed successfully for client %d", client_id);
                        } else {
                            mp_obj_t exc = MP_OBJ_FROM_PTR(nlr.ret_val);
                            mp_printf(&mp_plat_print, "[WEBREPL] ERROR: Exception in auth callback:\n");
                            mp_obj_print_exception(&mp_plat_print, exc);
                        }
                    } else {
                        ESP_LOGW(TAG, "No auth callback registered (callback=%p, is_callable=%d)", 
                                 wbp_auth_callback, mp_obj_is_callable(wbp_auth_callback));
                    }
                }
            } else {
                ESP_LOGW(TAG, "Authentication failed");
                wbp_send_auth_fail(client_id, "Access denied");
            }
            
            free(password);
            break;
        }
        
        default:
            ESP_LOGW(TAG, "Unknown event type: %d", event);
            break;
    }
}

/**
 * Main WBP message dispatcher - discriminates by channel number
 */
static void wbp_handle_message(int client_id, const uint8_t *data, size_t len) {
    if (len < 2) {
        ESP_LOGE(TAG, "Message too short");
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
    if (channel == WBP_CH_FILE) {
        // Channel 0: File operations
            handle_file_message(client_id, data, len);
    } else if (channel == WBP_CH_EVENT) {
        // Channel 23: Events
            handle_event_message(client_id, data, len);
    } else if (channel >= WBP_CH_TRM && channel <= 22) {
        // Channels 1-22: Execution channels
        handle_channel_message(client_id, channel, data, len);
    } else {
        ESP_LOGW(TAG, "Unknown channel: %d", channel);
    }
}

//=============================================================================
// WebSocket Event Callbacks
//=============================================================================

static void wbp_on_connect(int client_id) {
    ESP_LOGI(TAG, "Client connected: %d", client_id);
    
    xSemaphoreTake(g_wbp_mutex, portMAX_DELAY);
    
    int slot = find_free_client_slot();
    if (slot >= 0) {
        wbp_clients[slot].client_id = client_id;
        wbp_clients[slot].active = true;
        wbp_clients[slot].authenticated = false;
        wbp_clients[slot].current_channel = WBP_CH_TRM;
        
        ESP_LOGI(TAG, "Client %d registered in slot %d", client_id, slot);
        
        // Note: Welcome INFO message is optional per spec
        // Client should proceed with authentication
    } else {
        ESP_LOGW(TAG, "No free client slots");
    }
    
    xSemaphoreGive(g_wbp_mutex);
}

static void wbp_on_disconnect(int client_id) {
    ESP_LOGI(TAG, "Client disconnected: %d", client_id);
    
    xSemaphoreTake(g_wbp_mutex, portMAX_DELAY);
    
    int slot = find_client_by_id(client_id);
    if (slot >= 0) {
        wbp_clients[slot].active = false;
        wbp_clients[slot].authenticated = false;
        
        if (g_wbp_output_client_id == client_id) {
            g_wbp_output_client_id = -1;
        }
    }
    
    xSemaphoreGive(g_wbp_mutex);
}

static void wbp_on_message(int client_id, const uint8_t *data, size_t len, bool is_binary) {
    if (!is_binary) {
        ESP_LOGW(TAG, "Received text frame (expected binary CBOR)");
        return;
    }
    
    ESP_LOGI(TAG, "WBP Message: client=%d, len=%d", client_id, (int)len);
    
    wbp_handle_message(client_id, data, len);
}

#ifndef WSSERVER_EVENT_PREQUEUE_FILTER
#define WSSERVER_EVENT_PREQUEUE_FILTER 3
#endif

static bool wbp_msg_filter(int client_id, const uint8_t *data, size_t len, bool is_binary) {
    if (!is_binary || len < 3) return true; // Let queue handle it
    
    // Check for Interrupt packet: [1, 1]
    // We use full parsing to be safe
    CborParser parser;
    CborValue value, arrayValue;
    
    if (cbor_parser_init(data, len, 0, &parser, &value) != CborNoError) return true;
    if (!cbor_value_is_array(&value)) return true;
    
    cbor_value_enter_container(&value, &arrayValue);
    
    uint64_t channel_u64;
    if (cbor_value_get_uint64(&arrayValue, &channel_u64) != CborNoError) return true;
    
    if (channel_u64 != WBP_CH_TRM) return true;
    
    cbor_value_advance(&arrayValue);
    uint64_t opcode_u64;
    if (cbor_value_get_uint64(&arrayValue, &opcode_u64) != CborNoError) return true;
    
    // Fast-path: Interrupt
    if (opcode_u64 == WBP_OP_INT) {
        ESP_LOGI(TAG, "Fast-path Interrupt detected! Triggering KeyboardInterrupt.");
        mp_sched_keyboard_interrupt();
        return false; // Don't queue
    }
    
    // Fast-path: Input during Execution
    // If we are executing code (e.g. input()), we MUST handle incoming TRM data immediately
    // because the main task is blocked awaiting this data, so it cannot process the queue.
    if (opcode_u64 == WBP_OP_EXE && g_wbp_executing) {
        cbor_value_advance(&arrayValue);
        
        // Data format: [TRM, EXE, code_string, ?fmt, ?id]
        if (!cbor_value_at_end(&arrayValue)) {
            char *code_str = NULL;
            size_t code_len = 0;
            
            // Should be text string
            if (cbor_value_is_text_string(&arrayValue)) {
                if (cbor_value_dup_text_string(&arrayValue, &code_str, &code_len, NULL) == CborNoError) {
                    // Write directly to ring buffer
                    size_t written = wbp_input_ring_write((const uint8_t *)code_str, code_len);
                    (void)written; 
                    
                    free(code_str);
                    return false; // Don't queue (we handled it)
                }
            }
        }
    }
    
    return true; // Queue everything else
}

//=============================================================================
// Module Initialization
//=============================================================================

/**
 * Start WebREPL WBP server
 * 
 * Usage: webrepl.start(password="secret", path="/webrepl")
 * Both arguments are optional keyword arguments for compatibility with legacy API.
 */
static mp_obj_t wbp_start(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_password, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_path,     MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    
    // Extract password if provided
    const char *path = "/webrepl";  // default path
    
    if (args[0].u_obj != mp_const_none) {
        const char *password = mp_obj_str_get_str(args[0].u_obj);
        strncpy(wbp_password, password, sizeof(wbp_password) - 1);
        wbp_password[sizeof(wbp_password) - 1] = '\0';
        ESP_LOGI(TAG, "Password set from start()");
    }
    
    if (args[1].u_obj != mp_const_none) {
        path = mp_obj_str_get_str(args[1].u_obj);
    }
    
    ESP_LOGI(TAG, "Starting WBP on path: %s", path);
    
    // Initialize mutex
    if (!g_wbp_mutex) {
        g_wbp_mutex = xSemaphoreCreateMutex();
        if (!g_wbp_mutex) {
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to create mutex"));
        }
    }
    
    // Initialize message queue
    if (!wbp_queue_init()) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to init queue"));
    }
    
    // Initialize ring buffer
    if (!wbp_ring_init()) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to init ring buffer"));
    }
    
    // Initialize input ring buffer
    if (!wbp_input_ring_init()) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to init input ring buffer"));
    }
    
    // Start drain task
    if (!wbp_drain_task_start()) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to start drain task"));
    }
    
    // Clear client state
    memset(wbp_clients, 0, sizeof(wbp_clients));

    // Reset file transfer state (important for soft reset as heap is cleared)
    g_file_transfer.active = false;
    g_file_transfer.file_obj = MP_OBJ_NULL; 
    g_file_transfer.client_id = -1;
    
    // Reset execution state
    g_wbp_executing = false;
    g_wbp_current_channel = WBP_CH_TRM;
    if (g_wbp_current_id) {
         free(g_wbp_current_id);
         g_wbp_current_id = NULL;
    }
    
    // Register callbacks with wsserver
    wsserver_register_c_callback(WSSERVER_EVENT_CONNECT, (void *)wbp_on_connect);
    wsserver_register_c_callback(WSSERVER_EVENT_DISCONNECT, (void *)wbp_on_disconnect);
    wsserver_register_c_callback(WSSERVER_EVENT_MESSAGE, (void *)wbp_on_message);
    wsserver_register_c_callback(WSSERVER_EVENT_PREQUEUE_FILTER, (void *)wbp_msg_filter);
    
    // Start wsserver if not already running
    // Start wsserver if not already running
    if (!wsserver_is_running()) {
        if (!wsserver_start_c(path, 20, 60)) {
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to start wsserver"));
        }
    }
    
    ESP_LOGI(TAG, "WBP started successfully");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(wbp_start_obj, 0, wbp_start);

static mp_obj_t wbp_stop(void) {
    ESP_LOGI(TAG, "Stopping WBP...");
    
    // Detach dupterm
    wbp_dupterm_detach();
    
    // Stop drain task
    wbp_drain_task_stop();
    
    // Cleanup ring buffer
    wbp_ring_deinit();
    wbp_input_ring_deinit();
    
    // Disconnect all clients
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (wbp_clients[i].active) {
            wbp_clients[i].active = false;
            wbp_clients[i].authenticated = false;
        }
    }
    
    g_wbp_output_client_id = -1;
    
    ESP_LOGI(TAG, "WBP stopped");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(wbp_stop_obj, wbp_stop);

static mp_obj_t wbp_set_password(mp_obj_t password_obj) {
    const char *password = mp_obj_str_get_str(password_obj);
    strncpy(wbp_password, password, sizeof(wbp_password) - 1);
    wbp_password[sizeof(wbp_password) - 1] = '\0';
    ESP_LOGI(TAG, "Password updated");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(wbp_set_password_obj, wbp_set_password);

/**
 * Register Python callback for authentication events
 * Usage: webrepl.on_auth(callback_function)
 * Callback signature: callback(client_id)
 */
static mp_obj_t wbp_on_auth(mp_obj_t callback_obj) {
    if (!mp_obj_is_callable(callback_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("on_auth() argument must be callable"));
    }
    wbp_auth_callback = callback_obj;
    ESP_LOGD(TAG, "Auth callback registered via on_auth()");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(wbp_on_auth_obj, wbp_on_auth);

static mp_obj_t wbp_running(void) {
    return mp_obj_new_bool(wsserver_is_running());
}
static MP_DEFINE_CONST_FUN_OBJ_0(wbp_running_obj, wbp_running);

static mp_obj_t wbp_recover(void) {
    if (g_wbp_output_client_id >= 0) {
        // Active client exists, restore dupterm attachment
        wbp_dupterm_attach(g_wbp_output_client_id);
        ESP_LOGI(TAG, "Restored session for client %d", g_wbp_output_client_id);
        return mp_const_true;
    }
    return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_0(wbp_recover_obj, wbp_recover);

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
    
    if (g_wbp_output_client_id < 0) {
        ESP_LOGD(TAG, "logHandler: not connected (client_id=%d)", g_wbp_output_client_id);
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
    wbp_send_log(g_wbp_output_client_id, wbp_level, (const uint8_t *)msg, msg_len,
                 timestamp, source);
    
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(wbp_log_handler_emit_obj, wbp_log_handler_emit);

/**
 * Handler.setLevel(level)
 */
static mp_obj_t wbp_log_handler_set_level(mp_obj_t self_in, mp_obj_t level_obj) {
    wbp_log_handler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->level = mp_obj_get_int(level_obj);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(wbp_log_handler_set_level_obj, wbp_log_handler_set_level);

/**
 * Handler.setFormatter(formatter)
 */
static mp_obj_t wbp_log_handler_set_formatter(mp_obj_t self_in, mp_obj_t formatter_obj) {
    wbp_log_handler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->formatter = formatter_obj;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(wbp_log_handler_set_formatter_obj, wbp_log_handler_set_formatter);

/**
 * Handler.close()
 */
static mp_obj_t wbp_log_handler_close(mp_obj_t self_in) {
    (void)self_in;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(wbp_log_handler_close_obj, wbp_log_handler_close);

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
    wbp_log_handler_type,
    MP_QSTR_logHandler,
    MP_TYPE_FLAG_NONE,
    make_new, wbp_log_handler_make_new,
    locals_dict, &wbp_log_handler_locals_dict
);

//=============================================================================
// Module Definition
//=============================================================================

static const mp_rom_map_elem_t webrepl_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_webrepl_binary) },
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&wbp_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&wbp_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_running), MP_ROM_PTR(&wbp_running_obj) },
    { MP_ROM_QSTR(MP_QSTR_recover), MP_ROM_PTR(&wbp_recover_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_password), MP_ROM_PTR(&wbp_set_password_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_auth), MP_ROM_PTR(&wbp_on_auth_obj) },
    { MP_ROM_QSTR(MP_QSTR_process_queue), MP_ROM_PTR(&wbp_process_queue_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&wbp_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_log), MP_ROM_PTR(&wbp_log_obj) },
    { MP_ROM_QSTR(MP_QSTR_notify), MP_ROM_PTR(&wbp_notify_obj) },
    // Log level constants
    { MP_ROM_QSTR(MP_QSTR_LOG_DEBUG), MP_ROM_INT(0) },
    { MP_ROM_QSTR(MP_QSTR_LOG_INFO), MP_ROM_INT(1) },
    { MP_ROM_QSTR(MP_QSTR_LOG_WARN), MP_ROM_INT(2) },
    { MP_ROM_QSTR(MP_QSTR_LOG_ERROR), MP_ROM_INT(3) },
    // Logging handler for Python logging module
    { MP_ROM_QSTR(MP_QSTR_logHandler), MP_ROM_PTR(&wbp_log_handler_type) },
};
static MP_DEFINE_CONST_DICT(webrepl_module_globals, webrepl_module_globals_table);

const mp_obj_module_t webrepl_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&webrepl_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_webrepl_binary, webrepl_module);
