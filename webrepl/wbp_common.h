/*
 * wbp_common.h - WebREPL Binary Protocol Common Definitions
 *
 * This header defines the shared protocol constants, types, and functions
 * used by the WebREPL Binary Protocol (WBP) implementation.
 *
 * The WBP protocol uses CBOR-encoded arrays for all messages, with channel
 * numbers discriminating message types.
 *
 * Copyright (c) 2025-2026 Jonathan E. Peace
 * SPDX-License-Identifier: MIT
 */

#ifndef WBP_COMMON_H
#define WBP_COMMON_H

#include "py/runtime.h"
#include "py/obj.h"
#include "py/mphal.h"
#include "py/stream.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "cbor.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

//=============================================================================
// Protocol Constants
//=============================================================================

// Channel numbers
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

// Event opcodes (Channel 0)
#define WBP_EVT_AUTH      0   // Authentication request
#define WBP_EVT_AUTH_OK   1   // Authentication success
#define WBP_EVT_AUTH_FAIL 2   // Authentication failure
#define WBP_EVT_INFO      3   // Informational message
#define WBP_EVT_LOG       4   // Structured log message

// File opcodes (Channel 23)
#define WBP_FILE_RRQ    1   // Read Request (download)
#define WBP_FILE_WRQ    2   // Write Request (upload)
#define WBP_FILE_DATA   3   // Data block
#define WBP_FILE_ACK    4   // Acknowledgment
#define WBP_FILE_ERROR  5   // Error response

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

// Ring buffer sizes
#define WBP_OUTPUT_RING_SIZE  16384  // 16KB output buffer
#define WBP_INPUT_RING_SIZE   1024   // 1KB input buffer

// Message queue
#define WBP_QUEUE_SIZE    50
#define WBP_MAX_CMD_LEN   4096

// Dupterm slot (unified: slot 1, UART uses slot 0)
#define WBP_DUPTERM_SLOT  1

//=============================================================================
// Transport Abstraction
//=============================================================================

/**
 * Transport interface for sending WBP messages.
 * Each transport (WebSocket, WebRTC, etc.) implements this interface.
 */
typedef struct {
    bool (*send)(const uint8_t *data, size_t len, void *ctx);
    bool (*is_connected)(void *ctx);
    void *context;
} wbp_transport_t;

// Global active transport (set by start() functions)
extern wbp_transport_t *g_wbp_active_transport;

// Current output state
extern uint8_t g_wbp_current_channel;
extern char *g_wbp_current_id;
extern bool g_wbp_executing;

//=============================================================================
// File Transfer State
//=============================================================================

typedef struct {
    bool active;
    bool is_write;           // true = upload (WRQ), false = download (RRQ)
    mp_obj_t file_obj;       // Open file object
    uint16_t block_num;      // Current block number
    size_t blksize;          // Block size
    size_t tsize;            // Total size (for progress)
    size_t transferred;      // Bytes transferred so far
} wbp_file_transfer_t;

extern wbp_file_transfer_t g_file_transfer;

//=============================================================================
// Message Queue Types
//=============================================================================

typedef enum {
    WBP_MSG_RAW_EXEC,
    WBP_MSG_RESET
} wbp_msg_type_t;

typedef struct {
    wbp_msg_type_t type;
    uint8_t channel;
    char *data;
    size_t data_len;
    char *id;           // Message ID
    bool is_bytecode;   // For .mpy execution
} wbp_queue_msg_t;

extern QueueHandle_t g_wbp_queue;

//=============================================================================
// Output Ring Buffer
//=============================================================================

typedef struct {
    uint8_t buffer[WBP_OUTPUT_RING_SIZE];
    volatile size_t head;
    volatile size_t tail;
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t data_available;
    TaskHandle_t drain_task;
    volatile bool active;
} wbp_output_ring_t;

extern wbp_output_ring_t g_output_ring;

//=============================================================================
// Input Ring Buffer
//=============================================================================

typedef struct {
    uint8_t buffer[WBP_INPUT_RING_SIZE];
    volatile size_t head;
    volatile size_t tail;
    SemaphoreHandle_t mutex;
    volatile bool active;
} wbp_input_ring_t;

extern wbp_input_ring_t g_input_ring;

//=============================================================================
// Completion Collector
//=============================================================================

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} completion_collector_t;

//=============================================================================
// dupterm Stream Type
//=============================================================================

typedef struct _wbp_stream_obj_t {
    mp_obj_base_t base;
} wbp_stream_obj_t;

extern const mp_obj_type_t wbp_stream_type;
extern wbp_stream_obj_t *g_wbp_stream;

//=============================================================================
// Log Handler Type
//=============================================================================

typedef struct _wbp_log_handler_obj_t {
    mp_obj_base_t base;
    mp_int_t level;       // Handler level threshold
    mp_obj_t formatter;   // Optional formatter (can be None)
} wbp_log_handler_obj_t;

extern const mp_obj_type_t wbp_log_handler_type;

//=============================================================================
// CBOR Helper Functions
//=============================================================================

/**
 * Extract byte data from a CBOR value, handling tagged typed arrays.
 * Caller must free() the returned data.
 */
CborError cbor_extract_byte_data(CborValue *value, uint8_t **data_out, size_t *len_out);

//=============================================================================
// Message Builder Functions
//=============================================================================

// Send via active transport
bool wbp_send_cbor(const uint8_t *data, size_t len);

// Channel messages
void wbp_send_result(uint8_t channel, const uint8_t *data, size_t len, const char *id);
void wbp_send_progress(uint8_t channel, int status, const char *error, const char *id);
void wbp_send_continuation(uint8_t channel);
void wbp_send_completions(uint8_t channel, completion_collector_t *collector);

// Event messages
void wbp_send_auth_ok(void);
void wbp_send_auth_fail(const char *error);
void wbp_send_info_json(mp_obj_t payload_obj);
void wbp_send_log(uint8_t level, const uint8_t *message, size_t message_len,
                  int64_t timestamp, const char *source);

// File messages
void wbp_send_file_ack(uint16_t block_num, size_t tsize, 
                       size_t blksize, int64_t mtime, uint16_t mode);
void wbp_send_file_data(uint16_t block_num, const uint8_t *data, size_t data_len);
void wbp_send_file_error(uint8_t error_code, const char *error_msg);

//=============================================================================
// Completion Functions
//=============================================================================

void completion_collector_init(completion_collector_t *collector);
void completion_collector_add(completion_collector_t *collector, const char *str, size_t len);
void completion_collector_free(completion_collector_t *collector);
void completion_print_strn(void *data, const char *str, size_t len);
int wbp_collect_completions(const char *str, size_t len, completion_collector_t *collector);

//=============================================================================
// File Transfer Functions
//=============================================================================

void wbp_close_transfer(void);
void wbp_handle_wrq(const char *path, size_t tsize, size_t blksize);
void wbp_handle_rrq(const char *path, size_t blksize);
void wbp_handle_data(uint16_t block_num, const uint8_t *data, size_t data_len);
void wbp_handle_ack(uint16_t block_num);

//=============================================================================
// Message Dispatch Functions
//=============================================================================

void wbp_handle_message(const uint8_t *data, size_t len);

//=============================================================================
// Queue Functions
//=============================================================================

bool wbp_queue_init(void);
void wbp_queue_message(wbp_msg_type_t type, uint8_t channel,
                       const char *data, size_t data_len, const char *id, bool is_bytecode);
mp_obj_t wbp_process_queue(void);

//=============================================================================
// Ring Buffer Functions
//=============================================================================

// Output ring
bool wbp_ring_init(void);
void wbp_ring_deinit(void);
size_t wbp_ring_write(const uint8_t *data, size_t len);
size_t wbp_ring_read(uint8_t *data, size_t max_len);
size_t wbp_ring_space(void);
size_t wbp_ring_available(void);

// Input ring
bool wbp_input_ring_init(void);
void wbp_input_ring_deinit(void);
size_t wbp_input_ring_write(const uint8_t *data, size_t len);
size_t wbp_input_ring_read(uint8_t *data, size_t max_len);
size_t wbp_input_ring_space(void);
size_t wbp_input_ring_available(void);

//=============================================================================
// Drain Task Functions
//=============================================================================

void wbp_drain_task(void *pvParameters);
bool wbp_drain_task_start(void);
void wbp_drain_task_stop(void);

//=============================================================================
// dupterm Stream Functions
//=============================================================================

mp_uint_t wbp_stream_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode);
mp_uint_t wbp_stream_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode);
mp_uint_t wbp_stream_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode);
void wbp_dupterm_attach(void);
void wbp_dupterm_detach(void);

//=============================================================================
// Utility Functions
//=============================================================================

uint8_t wbp_map_log_level(mp_int_t python_level);

#endif // WBP_COMMON_H
