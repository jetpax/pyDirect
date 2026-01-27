/*
 * wbp_common.c - WebREPL Binary Protocol Common Implementation
 *
 * This file contains the shared protocol logic used by the WebREPL Binary
 * Protocol (WBP) implementation. It is transport-agnostic and routes all
 * sends through the g_wbp_active_transport interface.
 *
 * Copyright (c) 2025-2026 Jonathan E. Peace
 * SPDX-License-Identifier: MIT
 */

#include "wbp_common.h"

#include "py/runtime.h"
#include "py/obj.h"
#include "py/mphal.h"
#include "py/stream.h"
#include "py/repl.h"
#include "py/gc.h"
#include "py/compile.h"
#include "py/persistentcode.h"
#include "shared/readline/readline.h"
#include "extmod/vfs.h"
#include "extmod/misc.h"

#include "esp_log.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "WBP";

//=============================================================================
// Global State
//=============================================================================

wbp_transport_t *g_wbp_active_transport = NULL;

uint8_t g_wbp_current_channel = WBP_CH_TRM;
char *g_wbp_current_id = NULL;
bool g_wbp_executing = false;

wbp_file_transfer_t g_file_transfer = {
    .active = false,
    .file_obj = MP_OBJ_NULL
};

QueueHandle_t g_wbp_queue = NULL;

wbp_output_ring_t g_output_ring = {
    .head = 0,
    .tail = 0,
    .mutex = NULL,
    .data_available = NULL,
    .drain_task = NULL,
    .active = false
};

wbp_input_ring_t g_input_ring = {
    .head = 0,
    .tail = 0,
    .mutex = NULL,
    .active = false
};

wbp_stream_obj_t *g_wbp_stream = NULL;

//=============================================================================
// CBOR Helper: Extract Byte Data
//=============================================================================

CborError cbor_extract_byte_data(CborValue *value, uint8_t **data_out, size_t *len_out) {
    *data_out = NULL;
    *len_out = 0;
    
    // Check for Tag 64 (Uint8Array from cbor-web)
    if (cbor_value_is_tag(value)) {
        CborTag tag;
        CborError err = cbor_value_get_tag(value, &tag);
        if (err != CborNoError) return err;
        
        if (tag == 64) {
            // Tag 64 = typed byte array, skip the tag
            err = cbor_value_skip_tag(value);
            if (err != CborNoError) return err;
        }
    }
    
    // Now should be a byte string
    if (!cbor_value_is_byte_string(value)) {
        ESP_LOGE(TAG, "cbor_extract_byte_data: not a byte string after tag handling");
        return CborErrorIllegalType;
    }
    
    return cbor_value_dup_byte_string(value, data_out, len_out, NULL);
}

//=============================================================================
// Send via Active Transport
//=============================================================================

bool wbp_send_cbor(const uint8_t *data, size_t len) {
    if (!g_wbp_active_transport || !g_wbp_active_transport->send) {
        ESP_LOGW(TAG, "No active transport for send");
        return false;
    }
    if (!g_wbp_active_transport->is_connected(g_wbp_active_transport->context)) {
        ESP_LOGD(TAG, "Transport not connected");
        return false;
    }
    return g_wbp_active_transport->send(data, len, g_wbp_active_transport->context);
}

//=============================================================================
// Message Builders
//=============================================================================

void wbp_send_result(uint8_t channel, const uint8_t *data, size_t len, const char *id) {
    ESP_LOGI(TAG, "wbp_send_result: ch=%d len=%d id=%s", channel, (int)len, id ? id : "(null)");
    
    // Calculate buffer size: [channel, 0, data, ?id]
    size_t buf_size = 64 + len + (id ? strlen(id) + 16 : 0);
    uint8_t *buf = malloc(buf_size);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate result buffer");
        return;
    }
    
    CborEncoder encoder, arrayEncoder;
    cbor_encoder_init(&encoder, buf, buf_size, 0);
    
    size_t array_len = 3;
    if (id) array_len++;
    
    cbor_encoder_create_array(&encoder, &arrayEncoder, array_len);
    cbor_encode_uint(&arrayEncoder, channel);
    cbor_encode_uint(&arrayEncoder, WBP_OP_RES);
    
    // Send as text string (REPL output is text)
    cbor_encode_text_string(&arrayEncoder, (const char *)data, len);
    
    if (id) {
        cbor_encode_text_string(&arrayEncoder, id, strlen(id));
    }
    
    cbor_encoder_close_container(&encoder, &arrayEncoder);
    
    size_t encoded_len = cbor_encoder_get_buffer_size(&encoder, buf);
    wbp_send_cbor(buf, encoded_len);
    free(buf);
}

void wbp_send_progress(uint8_t channel, int status, const char *error, const char *id) {
    uint8_t buf[256];
    CborEncoder encoder, arrayEncoder;
    
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    
    // Array: [channel, opcode, status, ?error_or_null, ?id]
    // Base length is 3 (channel, opcode, status)
    // If error exists, add 1 for error
    // If id exists, add 1 for id (and if no error, add 1 for null placeholder)
    size_t array_len = 3;
    if (error) {
        array_len++;  // error field
    } else if (id) {
        array_len++;  // null placeholder for error when id exists
    }
    if (id) {
        array_len++;  // id field
    }
    
    cbor_encoder_create_array(&encoder, &arrayEncoder, array_len);
    cbor_encode_uint(&arrayEncoder, channel);
    cbor_encode_uint(&arrayEncoder, WBP_OP_PRO);
    cbor_encode_uint(&arrayEncoder, status);
    
    if (error) {
        cbor_encode_text_string(&arrayEncoder, error, strlen(error));
    } else if (id) {
        cbor_encode_null(&arrayEncoder);
    }
    
    if (id) {
        cbor_encode_text_string(&arrayEncoder, id, strlen(id));
    }
    
    cbor_encoder_close_container(&encoder, &arrayEncoder);
    
    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    wbp_send_cbor(buf, len);
}

void wbp_send_continuation(uint8_t channel) {
    uint8_t buf[16];
    CborEncoder encoder, arrayEncoder;
    
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    cbor_encoder_create_array(&encoder, &arrayEncoder, 2);
    cbor_encode_uint(&arrayEncoder, channel);
    cbor_encode_uint(&arrayEncoder, WBP_OP_CON);
    cbor_encoder_close_container(&encoder, &arrayEncoder);
    
    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    wbp_send_cbor(buf, len);
}

void wbp_send_auth_ok(void) {
    uint8_t buf[16];
    CborEncoder encoder, arrayEncoder;
    
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    cbor_encoder_create_array(&encoder, &arrayEncoder, 2);
    cbor_encode_uint(&arrayEncoder, WBP_CH_EVENT);
    cbor_encode_uint(&arrayEncoder, WBP_EVT_AUTH_OK);
    cbor_encoder_close_container(&encoder, &arrayEncoder);
    
    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    wbp_send_cbor(buf, len);
}

void wbp_send_auth_fail(const char *error) {
    uint8_t buf[128];
    CborEncoder encoder, arrayEncoder;
    
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    cbor_encoder_create_array(&encoder, &arrayEncoder, 3);
    cbor_encode_uint(&arrayEncoder, WBP_CH_EVENT);
    cbor_encode_uint(&arrayEncoder, WBP_EVT_AUTH_FAIL);
    cbor_encode_text_string(&arrayEncoder, error, strlen(error));
    cbor_encoder_close_container(&encoder, &arrayEncoder);
    
    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    wbp_send_cbor(buf, len);
}

void wbp_send_info_json(mp_obj_t payload_obj) {
    if (!mp_obj_is_str(payload_obj)) {
        ESP_LOGE(TAG, "INFO payload must be a JSON string");
        return;
    }
    
    size_t json_len;
    const char *json_str = mp_obj_str_get_data(payload_obj, &json_len);
    
    uint8_t buf[2048];
    CborEncoder encoder, arrayEncoder;
    
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    cbor_encoder_create_array(&encoder, &arrayEncoder, 3);
    cbor_encode_uint(&arrayEncoder, WBP_CH_EVENT);
    cbor_encode_uint(&arrayEncoder, WBP_EVT_INFO);
    cbor_encode_text_string(&arrayEncoder, json_str, json_len);
    cbor_encoder_close_container(&encoder, &arrayEncoder);
    
    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    wbp_send_cbor(buf, len);
}

void wbp_send_log(uint8_t level, const uint8_t *message, size_t message_len,
                  int64_t timestamp, const char *source) {
    size_t buf_size = 64 + message_len + (source ? strlen(source) + 16 : 0);
    uint8_t *buf = malloc(buf_size);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate LOG buffer");
        return;
    }
    
    CborEncoder encoder, arrayEncoder;
    cbor_encoder_init(&encoder, buf, buf_size, 0);
    
    // [0, 4, level, message, ?timestamp, ?source]
    size_t array_len = 4;
    if (timestamp > 0) array_len++;
    if (source) array_len++;
    
    cbor_encoder_create_array(&encoder, &arrayEncoder, array_len);
    cbor_encode_uint(&arrayEncoder, WBP_CH_EVENT);
    cbor_encode_uint(&arrayEncoder, WBP_EVT_LOG);
    cbor_encode_uint(&arrayEncoder, level);
    cbor_encode_text_string(&arrayEncoder, (const char *)message, message_len);
    
    if (timestamp > 0) {
        cbor_encode_int(&arrayEncoder, timestamp);
    }
    
    if (source) {
        cbor_encode_text_string(&arrayEncoder, source, strlen(source));
    }
    
    cbor_encoder_close_container(&encoder, &arrayEncoder);
    
    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    wbp_send_cbor(buf, len);
    free(buf);
}

//=============================================================================
// File Message Builders
//=============================================================================

void wbp_send_file_ack(uint16_t block_num, size_t tsize, 
                       size_t blksize, int64_t mtime, uint16_t mode) {
    uint8_t buf[64];
    CborEncoder encoder, arrayEncoder;
    
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    
    // ACK 0 has optional fields, ACK N is just [23, 4, block#]
    size_t array_len = 3;
    if (block_num == 0 && tsize > 0) {
        array_len++;  // tsize
        if (blksize > 0) array_len++;
        if (mtime > 0) array_len++;
        if (mode > 0) array_len++;
    }
    
    cbor_encoder_create_array(&encoder, &arrayEncoder, array_len);
    cbor_encode_uint(&arrayEncoder, WBP_CH_FILE);
    cbor_encode_uint(&arrayEncoder, WBP_FILE_ACK);
    cbor_encode_uint(&arrayEncoder, block_num);
    
    if (block_num == 0 && tsize > 0) {
        cbor_encode_uint(&arrayEncoder, tsize);
        if (blksize > 0) cbor_encode_uint(&arrayEncoder, blksize);
        if (mtime > 0) cbor_encode_int(&arrayEncoder, mtime);
        if (mode > 0) cbor_encode_uint(&arrayEncoder, mode);
    }
    
    cbor_encoder_close_container(&encoder, &arrayEncoder);
    
    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    wbp_send_cbor(buf, len);
}

void wbp_send_file_data(uint16_t block_num, const uint8_t *data, size_t data_len) {
    size_t buf_size = 32 + data_len;
    uint8_t *buf = malloc(buf_size);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate DATA buffer");
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
    
    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    wbp_send_cbor(buf, len);
    free(buf);
}

void wbp_send_file_error(uint8_t error_code, const char *error_msg) {
    uint8_t buf[256];
    CborEncoder encoder, arrayEncoder;
    
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    cbor_encoder_create_array(&encoder, &arrayEncoder, 4);
    cbor_encode_uint(&arrayEncoder, WBP_CH_FILE);
    cbor_encode_uint(&arrayEncoder, WBP_FILE_ERROR);
    cbor_encode_uint(&arrayEncoder, error_code);
    cbor_encode_text_string(&arrayEncoder, error_msg, strlen(error_msg));
    cbor_encoder_close_container(&encoder, &arrayEncoder);
    
    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    wbp_send_cbor(buf, len);
}

//=============================================================================
// Completion Collector
//=============================================================================

void completion_collector_init(completion_collector_t *collector) {
    collector->items = NULL;
    collector->count = 0;
    collector->capacity = 0;
}

void completion_collector_add(completion_collector_t *collector, const char *str, size_t len) {
    if (collector->count >= collector->capacity) {
        size_t new_capacity = collector->capacity == 0 ? 8 : collector->capacity * 2;
        char **new_items = realloc(collector->items, new_capacity * sizeof(char*));
        if (!new_items) return;
        collector->items = new_items;
        collector->capacity = new_capacity;
    }
    
    char *copy = malloc(len + 1);
    if (!copy) return;
    memcpy(copy, str, len);
    copy[len] = '\0';
    
    collector->items[collector->count++] = copy;
}

void completion_collector_free(completion_collector_t *collector) {
    for (size_t i = 0; i < collector->count; i++) {
        free(collector->items[i]);
    }
    free(collector->items);
    collector->items = NULL;
    collector->count = 0;
    collector->capacity = 0;
}

void completion_print_strn(void *data, const char *str, size_t len) {
    completion_collector_t *collector = (completion_collector_t *)data;
    
    // Skip empty strings and whitespace-only
    if (len == 0) return;
    
    const char *p = str;
    const char *end = str + len;
    
    while (p < end) {
        // Skip leading whitespace
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            p++;
        }
        if (p >= end) break;
        
        // Find end of token
        const char *token_start = p;
        while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
            p++;
        }
        
        size_t token_len = p - token_start;
        if (token_len > 0) {
            completion_collector_add(collector, token_start, token_len);
        }
    }
}

int wbp_collect_completions(const char *str, size_t len, completion_collector_t *collector) {
#if MICROPY_HELPER_REPL
    // Set up custom print for collecting completions
    mp_print_t print = {collector, completion_print_strn};
    
    // Use MicroPython's readline completion
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

void wbp_send_completions(uint8_t channel, completion_collector_t *collector) {
    size_t buf_size = 64;
    for (size_t i = 0; i < collector->count; i++) {
        buf_size += strlen(collector->items[i]) + 8;
    }
    
    uint8_t *buf = malloc(buf_size);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate completions buffer");
        return;
    }
    
    CborEncoder encoder, arrayEncoder, compArrayEncoder;
    cbor_encoder_init(&encoder, buf, buf_size, 0);
    
    // [channel, 3, [completion1, completion2, ...]]
    cbor_encoder_create_array(&encoder, &arrayEncoder, 3);
    cbor_encode_uint(&arrayEncoder, channel);
    cbor_encode_uint(&arrayEncoder, WBP_OP_COM);
    
    cbor_encoder_create_array(&arrayEncoder, &compArrayEncoder, collector->count);
    for (size_t i = 0; i < collector->count; i++) {
        cbor_encode_text_string(&compArrayEncoder, collector->items[i], strlen(collector->items[i]));
    }
    cbor_encoder_close_container(&arrayEncoder, &compArrayEncoder);
    
    cbor_encoder_close_container(&encoder, &arrayEncoder);
    
    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    wbp_send_cbor(buf, len);
    free(buf);
}

//=============================================================================
// File Transfer Handlers
//=============================================================================

void wbp_close_transfer(void) {
    if (g_file_transfer.active && g_file_transfer.file_obj != MP_OBJ_NULL) {
        // Close the file
        mp_stream_close(g_file_transfer.file_obj);
    }
    g_file_transfer.active = false;
    g_file_transfer.file_obj = MP_OBJ_NULL;
    g_file_transfer.block_num = 0;
    g_file_transfer.transferred = 0;
}

void wbp_handle_wrq(const char *path, size_t tsize, size_t blksize) {
    ESP_LOGI(TAG, "WRQ: path=%s, tsize=%d, blksize=%d", path, (int)tsize, (int)blksize);
    
    // Close any existing transfer
    if (g_file_transfer.active) {
        wbp_close_transfer();
    }
    
    // Use default block size if not specified
    if (blksize == 0) blksize = WBP_DEFAULT_BLKSIZE;
    
    // Open file for writing
    mp_obj_t args[2] = {
        mp_obj_new_str(path, strlen(path)),
        mp_obj_new_str("wb", 2)
    };
    
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        g_file_transfer.file_obj = mp_vfs_open(2, args, (mp_map_t *)&mp_const_empty_map);
        nlr_pop();
    } else {
        ESP_LOGE(TAG, "Failed to open file for writing");
        wbp_send_file_error(WBP_ERR_ACCESS, "Failed to open file");
        return;
    }
    
    g_file_transfer.active = true;
    g_file_transfer.is_write = true;
    g_file_transfer.block_num = 0;
    g_file_transfer.blksize = blksize;
    g_file_transfer.tsize = tsize;
    g_file_transfer.transferred = 0;
    
    // Send ACK 0 with confirmed options
    wbp_send_file_ack(0, tsize, blksize, 0, 0);
}

void wbp_handle_rrq(const char *path, size_t blksize) {
    ESP_LOGI(TAG, "RRQ: path=%s, blksize=%d", path, (int)blksize);
    
    // Close any existing transfer
    if (g_file_transfer.active) {
        wbp_close_transfer();
    }
    
    // Use default block size if not specified
    if (blksize == 0) blksize = WBP_DEFAULT_BLKSIZE;
    
    // Open file for reading
    mp_obj_t args[2] = {
        mp_obj_new_str(path, strlen(path)),
        mp_obj_new_str("rb", 2)
    };
    
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        g_file_transfer.file_obj = mp_vfs_open(2, args, (mp_map_t *)&mp_const_empty_map);
        nlr_pop();
    } else {
        ESP_LOGE(TAG, "File not found: %s", path);
        wbp_send_file_error(WBP_ERR_NOT_FOUND, "File not found");
        return;
    }
    
    // Get file size using seek/tell Python methods
    size_t file_size = 0;
    nlr_buf_t nlr2;
    if (nlr_push(&nlr2) == 0) {
        mp_obj_t seek_method = mp_load_attr(g_file_transfer.file_obj, MP_QSTR_seek);
        mp_obj_t tell_method = mp_load_attr(g_file_transfer.file_obj, MP_QSTR_tell);
        
        // seek(0, 2) -> end
        mp_obj_t seek_args[2] = { MP_OBJ_NEW_SMALL_INT(0), MP_OBJ_NEW_SMALL_INT(2) };
        mp_call_function_n_kw(seek_method, 2, 0, seek_args);
        
        // tell()
        mp_obj_t size_obj = mp_call_function_0(tell_method);
        file_size = mp_obj_get_int(size_obj);
        
        // seek(0, 0) -> start
        seek_args[1] = MP_OBJ_NEW_SMALL_INT(0);
        mp_call_function_n_kw(seek_method, 2, 0, seek_args);
        
        nlr_pop();
    } else {
        ESP_LOGE(TAG, "Failed to get file size");
        wbp_close_transfer();
        wbp_send_file_error(WBP_ERR_ACCESS, "Failed to get file size");
        return;
    }
    
    g_file_transfer.active = true;
    g_file_transfer.is_write = false;
    g_file_transfer.block_num = 0;
    g_file_transfer.blksize = blksize;
    g_file_transfer.tsize = file_size;
    g_file_transfer.transferred = 0;
    
    // Get modification time if available
    int64_t mtime = 0;
    // TODO: Get mtime from os.stat()
    
    // Send ACK 0 with file info
    wbp_send_file_ack(0, file_size, blksize, mtime, 0);
}

void wbp_handle_data(uint16_t block_num, const uint8_t *data, size_t data_len) {
    if (!g_file_transfer.active || !g_file_transfer.is_write) {
        ESP_LOGE(TAG, "DATA received but no active upload");
        wbp_send_file_error(WBP_ERR_UNKNOWN_TID, "No active upload");
        return;
    }
    
    // Check block number
    if (block_num != g_file_transfer.block_num + 1) {
        ESP_LOGW(TAG, "Unexpected block: got %d, expected %d", 
                 block_num, g_file_transfer.block_num + 1);
        // Re-ACK last block
        wbp_send_file_ack(g_file_transfer.block_num, 0, 0, 0, 0);
        return;
    }
    
    // Write data to file using Python method call
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t write_method = mp_load_attr(g_file_transfer.file_obj, MP_QSTR_write);
        mp_obj_t data_bytes = mp_obj_new_bytes(data, data_len);
        mp_call_function_1(write_method, data_bytes);
        nlr_pop();
    } else {
        ESP_LOGE(TAG, "Write failed");
        wbp_send_file_error(WBP_ERR_DISK_FULL, "Write failed");
        wbp_close_transfer();
        return;
    }
    
    g_file_transfer.block_num = block_num;
    g_file_transfer.transferred += data_len;
    
    // ACK this block
    wbp_send_file_ack(block_num, 0, 0, 0, 0);
    
    // Check if transfer complete (last block is smaller than blksize)
    if (data_len < g_file_transfer.blksize) {
        ESP_LOGI(TAG, "Upload complete: %d bytes", (int)g_file_transfer.transferred);
        wbp_close_transfer();
    }
}

void wbp_handle_ack(uint16_t block_num) {
    if (!g_file_transfer.active || g_file_transfer.is_write) {
        ESP_LOGD(TAG, "ACK received but no active download");
        return;
    }
    
    // ACK 0 = client ready for first block
    if (block_num == 0 && g_file_transfer.block_num == 0) {
        // Send first block
        g_file_transfer.block_num = 1;
    } else if (block_num != g_file_transfer.block_num) {
        ESP_LOGW(TAG, "Unexpected ACK: got %d, expected %d", 
                 block_num, g_file_transfer.block_num);
        return;
    } else {
        g_file_transfer.block_num++;
    }
    
    // Read and send next block using Python method call
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t read_method = mp_load_attr(g_file_transfer.file_obj, MP_QSTR_read);
        mp_obj_t size_obj = MP_OBJ_NEW_SMALL_INT(g_file_transfer.blksize);
        mp_obj_t data_obj = mp_call_function_1(read_method, size_obj);
        
        mp_buffer_info_t bufinfo;
        mp_get_buffer_raise(data_obj, &bufinfo, MP_BUFFER_READ);
        
        // Send DATA block
        wbp_send_file_data(g_file_transfer.block_num, bufinfo.buf, bufinfo.len);
        g_file_transfer.transferred += bufinfo.len;
        
        nlr_pop();
        
        // Check if this was the last block
        if (bufinfo.len < g_file_transfer.blksize) {
            ESP_LOGI(TAG, "Download complete: %d bytes", (int)g_file_transfer.transferred);
            wbp_close_transfer();
        }
    } else {
        ESP_LOGE(TAG, "Read failed");
        wbp_send_file_error(WBP_ERR_ACCESS, "Read failed");
        wbp_close_transfer();
    }
}

//=============================================================================
// Queue Functions
//=============================================================================

bool wbp_queue_init(void) {
    if (g_wbp_queue == NULL) {
        g_wbp_queue = xQueueCreate(WBP_QUEUE_SIZE, sizeof(wbp_queue_msg_t));
        if (g_wbp_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create message queue");
            return false;
        }
    }
    return true;
}

void wbp_queue_message(wbp_msg_type_t type, uint8_t channel,
                       const char *data, size_t data_len, const char *id, bool is_bytecode) {
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
        if (msg.data) {
            memcpy(msg.data, data, data_len);
            msg.data[data_len] = '\0';
        }
    }
    
    if (id) {
        msg.id = strdup(id);
    }
    
    if (xQueueSend(g_wbp_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Queue full, dropping message");
        if (msg.data) free(msg.data);
        if (msg.id) free(msg.id);
    }
}

mp_obj_t wbp_process_queue(void) {
    if (g_wbp_queue == NULL) {
        return MP_OBJ_NEW_SMALL_INT(0);
    }
    
    wbp_queue_msg_t msg;
    int processed = 0;
    
    while (xQueueReceive(g_wbp_queue, &msg, 0) == pdTRUE) {
        processed++;
        
        switch (msg.type) {
            case WBP_MSG_RAW_EXEC: {
                uint8_t channel = msg.channel;
                const char *code = msg.data;
                size_t code_len = msg.data_len;
                const char *id = msg.id;
                bool is_bytecode = msg.is_bytecode;
                
                g_wbp_executing = true;
                g_wbp_current_channel = channel;
                g_wbp_current_id = msg.id ? strdup(msg.id) : NULL;
                
                // Execute the code
                nlr_buf_t nlr;
                if (nlr_push(&nlr) == 0) {
                    if (is_bytecode) {
                        // TODO: Execute .mpy bytecode - not yet implemented
                        ESP_LOGW(TAG, "Bytecode execution not yet implemented");
                        wbp_send_progress(channel, 1, "Bytecode not supported", id);
                    } else {
                        // Execute Python source
                        bool success = false;
                        
                        // For terminal channel, try SINGLE_INPUT first to support auto-printing (REPL behavior)
                        if (channel == WBP_CH_TRM) {
                            nlr_buf_t nlr2;
                            if (nlr_push(&nlr2) == 0) {
                                mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_,
                                                                              code, code_len, false);
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
                                                                        code, code_len, 0);
                            mp_parse_tree_t pt = mp_parse(lex, MP_PARSE_FILE_INPUT);
                            mp_obj_t module = mp_compile(&pt, MP_QSTR__lt_stdin_gt_, false);
                            mp_call_function_0(module);
                        }
                        
                        // Success
                        wbp_send_progress(channel, 0, NULL, id);
                    }
                    nlr_pop();
                } else {
                    // Exception occurred
                    mp_obj_t exc = MP_OBJ_FROM_PTR(nlr.ret_val);
                    
                    // Print exception to ring buffer (will be sent as result)
                    mp_obj_print_exception(&mp_plat_print, exc);
                    
                    // Send error progress
                    wbp_send_progress(channel, 1, "Exception", id);
                }

                // Yield to allow drain task to send output with correct ID
                // BEFORE we clear g_wbp_current_id (fixes "missing ID" race)
                vTaskDelay(pdMS_TO_TICKS(50));
                
                g_wbp_executing = false;
                g_wbp_current_channel = WBP_CH_TRM;
                if (g_wbp_current_id) {
                    free(g_wbp_current_id);
                    g_wbp_current_id = NULL;
                }
                break;
            }
            
            case WBP_MSG_RESET: {
                bool hard = (msg.channel == 1);
                
                if (hard) {
                    ESP_LOGW(TAG, "Executing HARD RESET");
                    vTaskDelay(pdMS_TO_TICKS(200));
                    esp_restart();
                } else {
                    ESP_LOGI(TAG, "Executing SOFT RESET");
                    
                    // Cleanup
                    g_wbp_stream = NULL;
                    if (g_file_transfer.active) {
                        g_file_transfer.active = false;
                        g_file_transfer.file_obj = MP_OBJ_NULL;
                    }
                    
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

//=============================================================================
// Ring Buffer Implementation
//=============================================================================

bool wbp_ring_init(void) {
    if (g_output_ring.mutex == NULL) {
        g_output_ring.mutex = xSemaphoreCreateMutex();
        if (g_output_ring.mutex == NULL) {
            return false;
        }
    }
    
    if (g_output_ring.data_available == NULL) {
        g_output_ring.data_available = xSemaphoreCreateBinary();
        if (g_output_ring.data_available == NULL) {
            return false;
        }
    }
    
    g_output_ring.head = 0;
    g_output_ring.tail = 0;
    g_output_ring.active = true;
    
    return true;
}

void wbp_ring_deinit(void) {
    g_output_ring.active = false;
    
    if (g_output_ring.data_available) {
        xSemaphoreGive(g_output_ring.data_available);  // Wake drain task
    }
}

size_t wbp_ring_space(void) {
    size_t head = g_output_ring.head;
    size_t tail = g_output_ring.tail;
    
    if (head >= tail) {
        return WBP_OUTPUT_RING_SIZE - (head - tail) - 1;
    } else {
        return tail - head - 1;
    }
}

size_t wbp_ring_available(void) {
    size_t head = g_output_ring.head;
    size_t tail = g_output_ring.tail;
    
    if (head >= tail) {
        return head - tail;
    } else {
        return WBP_OUTPUT_RING_SIZE - tail + head;
    }
}

size_t wbp_ring_write(const uint8_t *data, size_t len) {
    if (!g_output_ring.active || !g_output_ring.mutex) return 0;
    
    xSemaphoreTake(g_output_ring.mutex, portMAX_DELAY);
    
    size_t space = wbp_ring_space();
    if (len > space) len = space;
    
    for (size_t i = 0; i < len; i++) {
        g_output_ring.buffer[g_output_ring.head] = data[i];
        g_output_ring.head = (g_output_ring.head + 1) % WBP_OUTPUT_RING_SIZE;
    }
    
    xSemaphoreGive(g_output_ring.mutex);
    
    if (len > 0 && g_output_ring.data_available) {
        xSemaphoreGive(g_output_ring.data_available);
    }
    
    return len;
}

size_t wbp_ring_read(uint8_t *data, size_t max_len) {
    if (!g_output_ring.active || !g_output_ring.mutex) return 0;
    
    xSemaphoreTake(g_output_ring.mutex, portMAX_DELAY);
    
    size_t available = wbp_ring_available();
    if (max_len > available) max_len = available;
    
    for (size_t i = 0; i < max_len; i++) {
        data[i] = g_output_ring.buffer[g_output_ring.tail];
        g_output_ring.tail = (g_output_ring.tail + 1) % WBP_OUTPUT_RING_SIZE;
    }
    
    xSemaphoreGive(g_output_ring.mutex);
    
    return max_len;
}

//=============================================================================
// Input Ring Buffer
//=============================================================================

bool wbp_input_ring_init(void) {
    if (g_input_ring.mutex == NULL) {
        g_input_ring.mutex = xSemaphoreCreateMutex();
        if (g_input_ring.mutex == NULL) {
            return false;
        }
    }
    
    g_input_ring.head = 0;
    g_input_ring.tail = 0;
    g_input_ring.active = true;
    
    return true;
}

void wbp_input_ring_deinit(void) {
    g_input_ring.active = false;
}

size_t wbp_input_ring_space(void) {
    size_t head = g_input_ring.head;
    size_t tail = g_input_ring.tail;
    
    if (head >= tail) {
        return WBP_INPUT_RING_SIZE - (head - tail) - 1;
    } else {
        return tail - head - 1;
    }
}

size_t wbp_input_ring_available(void) {
    size_t head = g_input_ring.head;
    size_t tail = g_input_ring.tail;
    
    if (head >= tail) {
        return head - tail;
    } else {
        return WBP_INPUT_RING_SIZE - tail + head;
    }
}

size_t wbp_input_ring_write(const uint8_t *data, size_t len) {
    if (!g_input_ring.active || !g_input_ring.mutex) return 0;
    
    xSemaphoreTake(g_input_ring.mutex, portMAX_DELAY);
    
    size_t space = wbp_input_ring_space();
    if (len > space) len = space;
    
    for (size_t i = 0; i < len; i++) {
        g_input_ring.buffer[g_input_ring.head] = data[i];
        g_input_ring.head = (g_input_ring.head + 1) % WBP_INPUT_RING_SIZE;
    }
    
    xSemaphoreGive(g_input_ring.mutex);
    
    return len;
}

size_t wbp_input_ring_read(uint8_t *data, size_t max_len) {
    if (!g_input_ring.active || !g_input_ring.mutex) return 0;
    
    xSemaphoreTake(g_input_ring.mutex, portMAX_DELAY);
    
    size_t available = wbp_input_ring_available();
    if (max_len > available) max_len = available;
    
    for (size_t i = 0; i < max_len; i++) {
        data[i] = g_input_ring.buffer[g_input_ring.tail];
        g_input_ring.tail = (g_input_ring.tail + 1) % WBP_INPUT_RING_SIZE;
    }
    
    xSemaphoreGive(g_input_ring.mutex);
    
    return max_len;
}

//=============================================================================
// Drain Task
//=============================================================================

#define DRAIN_CHUNK_SIZE 4096

void wbp_drain_task(void *pvParameters) {
    uint8_t *chunk = malloc(DRAIN_CHUNK_SIZE);
    if (!chunk) {
        ESP_LOGE(TAG, "Failed to allocate drain chunk buffer");
        vTaskDelete(NULL);
        return;
    }
    
    while (g_output_ring.active) {
        // Wait for data
        if (xSemaphoreTake(g_output_ring.data_available, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Drain all available data
            while (g_output_ring.active) {
                size_t available = wbp_ring_available();
                if (available == 0) break;
                
                size_t to_read = (available > DRAIN_CHUNK_SIZE) ? DRAIN_CHUNK_SIZE : available;
                size_t bytes_read = wbp_ring_read(chunk, to_read);
                
                if (bytes_read > 0) {
                    // Send as result on current channel
                    wbp_send_result(g_wbp_current_channel, chunk, bytes_read, g_wbp_current_id);
                }
            }
        }
    }
    
    free(chunk);
    vTaskDelete(NULL);
}

bool wbp_drain_task_start(void) {
    if (g_output_ring.drain_task != NULL) {
        return true;  // Already running
    }
    
    BaseType_t result = xTaskCreate(
        wbp_drain_task,
        "wbp_drain",
        4096,
        NULL,
        5,
        &g_output_ring.drain_task
    );
    
    return (result == pdPASS);
}

void wbp_drain_task_stop(void) {
    if (g_output_ring.drain_task != NULL) {
        g_output_ring.active = false;
        if (g_output_ring.data_available) {
            xSemaphoreGive(g_output_ring.data_available);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        g_output_ring.drain_task = NULL;
    }
}

//=============================================================================
// dupterm Stream Implementation
//=============================================================================

mp_uint_t wbp_stream_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode) {
    (void)self_in;
    
    // Read from input ring buffer
    size_t available = wbp_input_ring_available();
    if (available == 0) {
        *errcode = MP_EAGAIN;
        return MP_STREAM_ERROR;
    }
    
    return wbp_input_ring_read(buf, size);
}

mp_uint_t wbp_stream_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode) {
    (void)self_in;
    (void)errcode;
    
    // Write to output ring buffer
    return wbp_ring_write(buf, size);
}

mp_uint_t wbp_stream_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    (void)self_in;
    (void)arg;
    (void)errcode;
    
    if (request == MP_STREAM_POLL) {
        mp_uint_t flags = 0;
        if (wbp_input_ring_available() > 0) {
            flags |= MP_STREAM_POLL_RD;
        }
        if (wbp_ring_space() > 0) {
            flags |= MP_STREAM_POLL_WR;
        }
        return flags;
    }
    
    return 0;
}

// Stream protocol
static const mp_stream_p_t wbp_stream_p = {
    .read = wbp_stream_read,
    .write = wbp_stream_write,
    .ioctl = wbp_stream_ioctl,
};

// Stream type definition
MP_DEFINE_CONST_OBJ_TYPE(
    wbp_stream_type,
    MP_QSTR_WBPStream,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    protocol, &wbp_stream_p
);

void wbp_dupterm_attach(void) {
    if (g_wbp_stream == NULL) {
        g_wbp_stream = m_new_obj(wbp_stream_obj_t);
        g_wbp_stream->base.type = &wbp_stream_type;
    }
    
    // Call os.dupterm(stream, WBP_DUPTERM_SLOT)
    extern const mp_obj_fun_builtin_var_t mp_os_dupterm_obj;
    mp_obj_t dupterm_args[2] = {
        MP_OBJ_FROM_PTR(g_wbp_stream),
        MP_OBJ_NEW_SMALL_INT(WBP_DUPTERM_SLOT)
    };
    mp_call_function_n_kw(MP_OBJ_FROM_PTR(&mp_os_dupterm_obj), 2, 0, dupterm_args);
    
    ESP_LOGI(TAG, "Dupterm attached to slot %d", WBP_DUPTERM_SLOT);
}

void wbp_dupterm_detach(void) {
    // Call os.dupterm(None, WBP_DUPTERM_SLOT)
    extern const mp_obj_fun_builtin_var_t mp_os_dupterm_obj;
    mp_obj_t dupterm_args[2] = {
        mp_const_none,
        MP_OBJ_NEW_SMALL_INT(WBP_DUPTERM_SLOT)
    };
    mp_call_function_n_kw(MP_OBJ_FROM_PTR(&mp_os_dupterm_obj), 2, 0, dupterm_args);
    
    ESP_LOGI(TAG, "Dupterm detached from slot %d", WBP_DUPTERM_SLOT);
}

//=============================================================================
// Log Level Mapping
//=============================================================================

uint8_t wbp_map_log_level(mp_int_t python_level) {
    // Python logging levels: DEBUG=10, INFO=20, WARNING=30, ERROR=40, CRITICAL=50
    if (python_level <= 10) return 0;      // DEBUG
    if (python_level <= 20) return 1;      // INFO
    if (python_level <= 30) return 2;      // WARNING
    return 3;                               // ERROR/CRITICAL
}

//=============================================================================
// Log Handler Implementation
//=============================================================================

static mp_obj_t wbp_log_handler_emit(mp_obj_t self_in, mp_obj_t record) {
    wbp_log_handler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    // Check if transport is connected
    if (!g_wbp_active_transport || 
        !g_wbp_active_transport->is_connected(g_wbp_active_transport->context)) {
        return mp_const_none;  // Not connected, silently ignore
    }
    
    // Extract levelno from record
    mp_obj_t levelno_obj = mp_load_attr(record, MP_QSTR_levelno);
    mp_int_t levelno = mp_obj_get_int(levelno_obj);
    
    // Check level threshold
    if (levelno < self->level) {
        return mp_const_none;
    }
    
    // Format the message
    mp_obj_t msg_obj;
    if (self->formatter != mp_const_none && self->formatter != NULL) {
        mp_obj_t format_method = mp_load_attr(self->formatter, MP_QSTR_format);
        msg_obj = mp_call_function_1(format_method, record);
    } else {
        msg_obj = mp_load_attr(record, MP_QSTR_message);
    }
    
    size_t msg_len;
    const char *msg = mp_obj_str_get_data(msg_obj, &msg_len);
    
    // Extract source
    mp_obj_t name_obj = mp_load_attr(record, MP_QSTR_name);
    const char *source = NULL;
    if (name_obj != mp_const_none) {
        const char *name_str = mp_obj_str_get_str(name_obj);
        if (strcmp(name_str, "root") != 0) {
            source = name_str;
        }
    }
    
    // Map level and send
    uint8_t wbp_level = wbp_map_log_level(levelno);
    int64_t timestamp = mp_hal_ticks_ms() / 1000;
    
    wbp_send_log(wbp_level, (const uint8_t *)msg, msg_len, timestamp, source);
    
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(wbp_log_handler_emit_obj, wbp_log_handler_emit);

static mp_obj_t wbp_log_handler_set_level(mp_obj_t self_in, mp_obj_t level_obj) {
    wbp_log_handler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->level = mp_obj_get_int(level_obj);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(wbp_log_handler_set_level_obj, wbp_log_handler_set_level);

static mp_obj_t wbp_log_handler_set_formatter(mp_obj_t self_in, mp_obj_t formatter_obj) {
    wbp_log_handler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->formatter = formatter_obj;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(wbp_log_handler_set_formatter_obj, wbp_log_handler_set_formatter);

static mp_obj_t wbp_log_handler_close(mp_obj_t self_in) {
    (void)self_in;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(wbp_log_handler_close_obj, wbp_log_handler_close);

static mp_obj_t wbp_log_handler_make_new(const mp_obj_type_t *type, size_t n_args, 
                                          size_t n_kw, const mp_obj_t *args) {
    mp_arg_val_t vals[1];
    mp_arg_parse_all_kw_array(n_args, n_kw, args, MP_ARRAY_SIZE(vals),
        (mp_arg_t[]) {
            { MP_QSTR_level, MP_ARG_INT, {.u_int = 0} },
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
