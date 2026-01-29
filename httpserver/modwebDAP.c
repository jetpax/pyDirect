/*
 * modwebDAP.c - MicroPython Debug Adapter Protocol over WebSocket
 *
 * Implements DAP (Debug Adapter Protocol) for MicroPython on ESP32
 * using WebSocket TEXT frames (opcode 0x01) for protocol multiplexing.
 * 
 * Coexists with WebREPL CB (binary frames, opcode 0x02) on the same
 * WebSocket connection via opcode-based discrimination.
 *
 * Protocol: Debug Adapter Protocol (DAP)
 * Transport: WebSocket TEXT frames with Content-Length headers
 * Specification: https://microsoft.github.io/debug-adapter-protocol/
 *
 * Copyright (c) 2026 Jonathan Elliot Peace
 * SPDX-License-Identifier: MIT
 */

#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "esp_log.h"

#include "py/runtime.h"
#include "py/compile.h"
#include "py/mphal.h"
#include "py/obj.h"

// cJSON for DAP JSON parsing (ESP-IDF component)
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "webDAP";

// External wsserver C API
extern bool wsserver_register_c_callback(int event_type, void *callback_func);
extern bool wsserver_send_to_client(int client_id, const uint8_t *data, 
                                     size_t len, bool is_binary);
extern bool wsserver_is_running(void);

// wsserver event types
#define WSSERVER_EVENT_CONNECT    0
#define WSSERVER_EVENT_DISCONNECT 1
#define WSSERVER_EVENT_MESSAGE    2

//=============================================================================
// DAP State Management
//=============================================================================

// Breakpoint structure
typedef struct breakpoint_s {
    char *file;
    int line;
    bool verified;
    struct breakpoint_s *next;
} breakpoint_t;

// DAP client state
typedef struct {
    int client_id;
    bool active;
    bool initialized;
    uint32_t seq;
    
    // Breakpoint tracking
    breakpoint_t *breakpoints;
    
    // Debug state
    bool stopped;
    const char *stop_reason;
    int thread_id;
    
    // Future: frame inspection via sys.settrace()
    // mp_code_state_t *current_frame;
} dap_client_t;

static dap_client_t dap_client = {
    .client_id = -1,
    .active = false,
    .initialized = false,
    .seq = 1,
    .breakpoints = NULL,
    .stopped = false,
    .stop_reason = NULL,
    .thread_id = 1
};

//=============================================================================
// Breakpoint Management
//=============================================================================

static void dap_add_breakpoint(const char *file, int line) {
    breakpoint_t *bp = malloc(sizeof(breakpoint_t));
    if (!bp) {
        ESP_LOGE(TAG, "Out of memory for breakpoint");
        return;
    }
    
    bp->file = strdup(file);
    bp->line = line;
    bp->verified = true; // TODO: Verify against actual code
    bp->next = dap_client.breakpoints;
    dap_client.breakpoints = bp;
    
    ESP_LOGI(TAG, "Breakpoint added: %s:%d", file, line);
}

static void dap_clear_breakpoints_for_file(const char *file) {
    breakpoint_t **current = &dap_client.breakpoints;
    
    while (*current) {
        breakpoint_t *bp = *current;
        if (strcmp(bp->file, file) == 0) {
            *current = bp->next;
            free(bp->file);
            free(bp);
        } else {
            current = &bp->next;
        }
    }
}

static void dap_clear_all_breakpoints(void) {
    breakpoint_t *bp = dap_client.breakpoints;
    while (bp) {
        breakpoint_t *next = bp->next;
        free(bp->file);
        free(bp);
        bp = next;
    }
    dap_client.breakpoints = NULL;
}

//=============================================================================
// DAP Message Parsing
//=============================================================================

/**
 * Parse DAP message with Content-Length header
 * Format: "Content-Length: 123\r\n\r\n{...json...}"
 */
static cJSON *dap_parse_message(const char *data, size_t len, size_t *json_offset) {
    // Find Content-Length header
    const char *cl_start = strstr(data, "Content-Length: ");
    if (!cl_start || cl_start > data + 100) { // Header should be near start
        ESP_LOGE(TAG, "No Content-Length header");
        return NULL;
    }
    
    // Parse content length
    size_t content_len = atoi(cl_start + 16);
    
    // Find \r\n\r\n separator
    const char *json_start = strstr(cl_start, "\r\n\r\n");
    if (!json_start) {
        ESP_LOGE(TAG, "No header separator");
        return NULL;
    }
    json_start += 4; // Skip \r\n\r\n
    
    *json_offset = json_start - data;
    
    // Verify we have enough data
    if (*json_offset + content_len > len) {
        ESP_LOGE(TAG, "Incomplete message: expected %zu bytes, got %zu", 
                 *json_offset + content_len, len);
        return NULL;
    }
    
    // Parse JSON
    cJSON *json = cJSON_ParseWithLength(json_start, content_len);
    if (!json) {
        ESP_LOGE(TAG, "JSON parse failed: %s", cJSON_GetErrorPtr());
        return NULL;
    }
    
    return json;
}

//=============================================================================
// DAP Response Builder
//=============================================================================

/**
 * Send DAP response with Content-Length header
 */
static void dap_send_response(int client_id, cJSON *response_json) {
    char *json_str = cJSON_PrintUnformatted(response_json);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to serialize response");
        return;
    }
    
    size_t json_len = strlen(json_str);
    
    // Build message with Content-Length header
    char header[128];
    int header_len = snprintf(header, sizeof(header),
                             "Content-Length: %zu\r\n\r\n", json_len);
    
    // Allocate combined buffer
    size_t total_len = header_len + json_len;
    char *message = malloc(total_len);
    if (!message) {
        ESP_LOGE(TAG, "Out of memory for DAP response");
        free(json_str);
        return;
    }
    
    memcpy(message, header, header_len);
    memcpy(message + header_len, json_str, json_len);
    
    // Send as TEXT frame (is_binary=false)
    if (!wsserver_send_to_client(client_id, (uint8_t *)message, total_len, false)) {
        ESP_LOGE(TAG, "Failed to send DAP response");
    }
    
    free(message);
    free(json_str);
}

/**
 * Send DAP event (async, not in response to request)
 */
static void dap_send_event(int client_id, const char *event_type, cJSON *body) {
    cJSON *event = cJSON_CreateObject();
    cJSON_AddNumberToObject(event, "seq", dap_client.seq++);
    cJSON_AddStringToObject(event, "type", "event");
    cJSON_AddStringToObject(event, "event", event_type);
    if (body) {
        cJSON_AddItemToObject(event, "body", body);
    }
    
    dap_send_response(client_id, event);
    cJSON_Delete(event);
}

//=============================================================================
// DAP Request Handlers
//=============================================================================

/**
 * Handle 'initialize' request
 */
static void dap_handle_initialize(int client_id, cJSON *request) {
    cJSON *seq_obj = cJSON_GetObjectItem(request, "seq");
    int req_seq = seq_obj ? seq_obj->valueint : 0;
    
    ESP_LOGI(TAG, "Initialize request (seq=%d)", req_seq);
    
    // Build capabilities
    cJSON *capabilities = cJSON_CreateObject();
    cJSON_AddBoolToObject(capabilities, "supportsConfigurationDoneRequest", true);
    cJSON_AddBoolToObject(capabilities, "supportsFunctionBreakpoints", false);
    cJSON_AddBoolToObject(capabilities, "supportsConditionalBreakpoints", false);
    cJSON_AddBoolToObject(capabilities, "supportsHitConditionalBreakpoints", false);
    cJSON_AddBoolToObject(capabilities, "supportsEvaluateForHovers", false);
    cJSON_AddBoolToObject(capabilities, "supportsStepBack", false);
    cJSON_AddBoolToObject(capabilities, "supportsSetVariable", false);
    cJSON_AddBoolToObject(capabilities, "supportsRestartFrame", false);
    cJSON_AddBoolToObject(capabilities, "supportsGotoTargetsRequest", false);
    cJSON_AddBoolToObject(capabilities, "supportsStepInTargetsRequest", false);
    cJSON_AddBoolToObject(capabilities, "supportsCompletionsRequest", false);
    cJSON_AddBoolToObject(capabilities, "supportsModulesRequest", false);
    cJSON_AddBoolToObject(capabilities, "supportsRestartRequest", false);
    cJSON_AddBoolToObject(capabilities, "supportsExceptionOptions", false);
    cJSON_AddBoolToObject(capabilities, "supportsValueFormattingOptions", false);
    cJSON_AddBoolToObject(capabilities, "supportsExceptionInfoRequest", false);
    cJSON_AddBoolToObject(capabilities, "supportTerminateDebuggee", true);
    cJSON_AddBoolToObject(capabilities, "supportsDelayedStackTraceLoading", false);
    cJSON_AddBoolToObject(capabilities, "supportsLoadedSourcesRequest", false);
    cJSON_AddBoolToObject(capabilities, "supportsLogPoints", false);
    cJSON_AddBoolToObject(capabilities, "supportsTerminateThreadsRequest", false);
    cJSON_AddBoolToObject(capabilities, "supportsSetExpression", false);
    cJSON_AddBoolToObject(capabilities, "supportsTerminateRequest", true);
    cJSON_AddBoolToObject(capabilities, "supportsDataBreakpoints", false);
    cJSON_AddBoolToObject(capabilities, "supportsReadMemoryRequest", false);
    cJSON_AddBoolToObject(capabilities, "supportsDisassembleRequest", false);
    
    // Build response
    cJSON *response = cJSON_CreateObject();
    cJSON_AddNumberToObject(response, "seq", dap_client.seq++);
    cJSON_AddStringToObject(response, "type", "response");
    cJSON_AddNumberToObject(response, "request_seq", req_seq);
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "command", "initialize");
    cJSON_AddItemToObject(response, "body", capabilities);
    
    dap_send_response(client_id, response);
    cJSON_Delete(response);
    
    dap_client.initialized = true;
    
    // Send initialized event
    dap_send_event(client_id, "initialized", NULL);
    
    ESP_LOGI(TAG, "DAP initialized successfully");
}

/**
 * Handle 'setBreakpoints' request
 */
static void dap_handle_set_breakpoints(int client_id, cJSON *request) {
    cJSON *seq_obj = cJSON_GetObjectItem(request, "seq");
    int req_seq = seq_obj ? seq_obj->valueint : 0;
    
    cJSON *arguments = cJSON_GetObjectItem(request, "arguments");
    if (!arguments) {
        ESP_LOGE(TAG, "setBreakpoints: no arguments");
        return;
    }
    
    cJSON *source = cJSON_GetObjectItem(arguments, "source");
    cJSON *path_obj = cJSON_GetObjectItem(source, "path");
    cJSON *breakpoints_array = cJSON_GetObjectItem(arguments, "breakpoints");
    
    const char *source_path = path_obj ? path_obj->valuestring : "";
    
    ESP_LOGI(TAG, "setBreakpoints: %s (%d breakpoints)", 
             source_path, 
             breakpoints_array ? cJSON_GetArraySize(breakpoints_array) : 0);
    
    // Clear existing breakpoints for this file
    dap_clear_breakpoints_for_file(source_path);
    
    // Build response with verified breakpoints
    cJSON *verified_breakpoints = cJSON_CreateArray();
    
    if (breakpoints_array) {
        cJSON *bp;
        cJSON_ArrayForEach(bp, breakpoints_array) {
            cJSON *line_obj = cJSON_GetObjectItem(bp, "line");
            int line = line_obj ? line_obj->valueint : 0;
            
            // Store breakpoint
            dap_add_breakpoint(source_path, line);
            
            // Create verified breakpoint response
            cJSON *verified = cJSON_CreateObject();
            cJSON_AddBoolToObject(verified, "verified", true);
            cJSON_AddNumberToObject(verified, "line", line);
            
            cJSON_AddItemToArray(verified_breakpoints, verified);
        }
    }
    
    cJSON *body = cJSON_CreateObject();
    cJSON_AddItemToObject(body, "breakpoints", verified_breakpoints);
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddNumberToObject(response, "seq", dap_client.seq++);
    cJSON_AddStringToObject(response, "type", "response");
    cJSON_AddNumberToObject(response, "request_seq", req_seq);
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "command", "setBreakpoints");
    cJSON_AddItemToObject(response, "body", body);
    
    dap_send_response(client_id, response);
    cJSON_Delete(response);
}

/**
 * Handle 'continue' request
 */
static void dap_handle_continue(int client_id, cJSON *request) {
    cJSON *seq_obj = cJSON_GetObjectItem(request, "seq");
    int req_seq = seq_obj ? seq_obj->valueint : 0;
    
    ESP_LOGI(TAG, "Continue request");
    
    dap_client.stopped = false;
    dap_client.stop_reason = NULL;
    
    // Build response
    cJSON *body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "allThreadsContinued", true);
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddNumberToObject(response, "seq", dap_client.seq++);
    cJSON_AddStringToObject(response, "type", "response");
    cJSON_AddNumberToObject(response, "request_seq", req_seq);
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "command", "continue");
    cJSON_AddItemToObject(response, "body", body);
    
    dap_send_response(client_id, response);
    cJSON_Delete(response);
}

/**
 * Handle 'threads' request
 */
static void dap_handle_threads(int client_id, cJSON *request) {
    cJSON *seq_obj = cJSON_GetObjectItem(request, "seq");
    int req_seq = seq_obj ? seq_obj->valueint : 0;
    
    // MicroPython typically runs single-threaded
    cJSON *threads = cJSON_CreateArray();
    cJSON *thread = cJSON_CreateObject();
    cJSON_AddNumberToObject(thread, "id", dap_client.thread_id);
    cJSON_AddStringToObject(thread, "name", "main");
    cJSON_AddItemToArray(threads, thread);
    
    cJSON *body = cJSON_CreateObject();
    cJSON_AddItemToObject(body, "threads", threads);
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddNumberToObject(response, "seq", dap_client.seq++);
    cJSON_AddStringToObject(response, "type", "response");
    cJSON_AddNumberToObject(response, "request_seq", req_seq);
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "command", "threads");
    cJSON_AddItemToObject(response, "body", body);
    
    dap_send_response(client_id, response);
    cJSON_Delete(response);
}

/**
 * Handle 'stackTrace' request
 */
static void dap_handle_stack_trace(int client_id, cJSON *request) {
    cJSON *seq_obj = cJSON_GetObjectItem(request, "seq");
    int req_seq = seq_obj ? seq_obj->valueint : 0;
    
    ESP_LOGI(TAG, "stackTrace request");
    
    // TODO: Build actual stack trace from sys.settrace() frame info
    // For now, return empty stack when not stopped
    
    cJSON *stack_frames = cJSON_CreateArray();
    cJSON *body = cJSON_CreateObject();
    cJSON_AddItemToObject(body, "stackFrames", stack_frames);
    cJSON_AddNumberToObject(body, "totalFrames", 0);
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddNumberToObject(response, "seq", dap_client.seq++);
    cJSON_AddStringToObject(response, "type", "response");
    cJSON_AddNumberToObject(response, "request_seq", req_seq);
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "command", "stackTrace");
    cJSON_AddItemToObject(response, "body", body);
    
    dap_send_response(client_id, response);
    cJSON_Delete(response);
}

/**
 * Handle 'scopes' request
 */
static void dap_handle_scopes(int client_id, cJSON *request) {
    cJSON *seq_obj = cJSON_GetObjectItem(request, "seq");
    int req_seq = seq_obj ? seq_obj->valueint : 0;
    
    // Return empty scopes for now
    cJSON *scopes = cJSON_CreateArray();
    
    cJSON *body = cJSON_CreateObject();
    cJSON_AddItemToObject(body, "scopes", scopes);
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddNumberToObject(response, "seq", dap_client.seq++);
    cJSON_AddStringToObject(response, "type", "response");
    cJSON_AddNumberToObject(response, "request_seq", req_seq);
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "command", "scopes");
    cJSON_AddItemToObject(response, "body", body);
    
    dap_send_response(client_id, response);
    cJSON_Delete(response);
}

/**
 * Handle 'launch' request
 * Launches a Python script with debugging enabled
 */
static void dap_handle_launch(int client_id, cJSON *request) {
    cJSON *seq_obj = cJSON_GetObjectItem(request, "seq");
    int req_seq = seq_obj ? seq_obj->valueint : 0;
    
    cJSON *arguments = cJSON_GetObjectItem(request, "arguments");
    
    // Extract launch parameters
    cJSON *program_obj = cJSON_GetObjectItem(arguments, "program");
    cJSON *no_debug_obj = cJSON_GetObjectItem(arguments, "noDebug");
    cJSON *stop_on_entry_obj = cJSON_GetObjectItem(arguments, "stopOnEntry");
    
    const char *program = program_obj ? program_obj->valuestring : "/main.py";
    bool no_debug = no_debug_obj ? no_debug_obj->valueint : false;
    bool stop_on_entry = stop_on_entry_obj ? stop_on_entry_obj->valueint : false;
    
    ESP_LOGI(TAG, "Launch request: program=%s, noDebug=%d, stopOnEntry=%d", 
             program, no_debug, stop_on_entry);
    
    // TODO: Integration with MicroPython execution
    // 1. If noDebug=false, install sys.settrace() callback
    // 2. If stopOnEntry=true, break at first line
    // 3. Execute the program file using mp_import_execute()
    
    // For now, acknowledge success
    cJSON *response = cJSON_CreateObject();
    cJSON_AddNumberToObject(response, "seq", dap_client.seq++);
    cJSON_AddStringToObject(response, "type", "response");
    cJSON_AddNumberToObject(response, "request_seq", req_seq);
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "command", "launch");
    
    dap_send_response(client_id, response);
    cJSON_Delete(response);
    
    // Send stopped event if stopOnEntry
    if (stop_on_entry && !no_debug) {
        dap_client.stopped = true;
        dap_client.stop_reason = "entry";
        
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "reason", "entry");
        cJSON_AddNumberToObject(body, "threadId", dap_client.thread_id);
        cJSON_AddStringToObject(body, "description", "Paused on entry");
        cJSON_AddBoolToObject(body, "preserveFocusHint", false);
        cJSON_AddBoolToObject(body, "allThreadsStopped", true);
        
        dap_send_event(client_id, "stopped", body);
    }
}

/**
 * Handle 'attach' request
 * Attaches to already-running MicroPython REPL
 */
static void dap_handle_attach(int client_id, cJSON *request) {
    cJSON *seq_obj = cJSON_GetObjectItem(request, "seq");
    int req_seq = seq_obj ? seq_obj->valueint : 0;
    
    ESP_LOGI(TAG, "Attach request");
    
    // TODO: Install sys.settrace() callback on running interpreter
    // This allows debugging of already-executing code
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddNumberToObject(response, "seq", dap_client.seq++);
    cJSON_AddStringToObject(response, "type", "response");
    cJSON_AddNumberToObject(response, "request_seq", req_seq);
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "command", "attach");
    
    dap_send_response(client_id, response);
    cJSON_Delete(response);
}

/**
 * Handle simple acknowledgment commands
 */
static void dap_handle_simple_ack(int client_id, cJSON *request, const char *command) {
    cJSON *seq_obj = cJSON_GetObjectItem(request, "seq");
    int req_seq = seq_obj ? seq_obj->valueint : 0;
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddNumberToObject(response, "seq", dap_client.seq++);
    cJSON_AddStringToObject(response, "type", "response");
    cJSON_AddNumberToObject(response, "request_seq", req_seq);
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "command", command);
    
    dap_send_response(client_id, response);
    cJSON_Delete(response);
}

/**
 * Dispatch DAP command
 */
static void dap_dispatch_command(int client_id, cJSON *request) {
    cJSON *command_obj = cJSON_GetObjectItem(request, "command");
    if (!command_obj || !command_obj->valuestring) {
        ESP_LOGE(TAG, "No command in request");
        return;
    }
    
    const char *command = command_obj->valuestring;
    ESP_LOGD(TAG, "DAP command: %s", command);
    
    if (strcmp(command, "initialize") == 0) {
        dap_handle_initialize(client_id, request);
    } else if (strcmp(command, "setBreakpoints") == 0) {
        dap_handle_set_breakpoints(client_id, request);
    } else if (strcmp(command, "continue") == 0) {
        dap_handle_continue(client_id, request);
    } else if (strcmp(command, "threads") == 0) {
        dap_handle_threads(client_id, request);
    } else if (strcmp(command, "stackTrace") == 0) {
        dap_handle_stack_trace(client_id, request);
    } else if (strcmp(command, "scopes") == 0) {
        dap_handle_scopes(client_id, request);
    } else if (strcmp(command, "launch") == 0) {
        dap_handle_launch(client_id, request);
    } else if (strcmp(command, "attach") == 0) {
        dap_handle_attach(client_id, request);
    } else if (strcmp(command, "configurationDone") == 0 ||
               strcmp(command, "disconnect") == 0 ||
               strcmp(command, "pause") == 0 ||
               strcmp(command, "next") == 0 ||
               strcmp(command, "stepIn") == 0 ||
               strcmp(command, "stepOut") == 0) {
        // Simple acknowledgment for commands not yet implemented
        dap_handle_simple_ack(client_id, request, command);
    } else {
        ESP_LOGW(TAG, "Unhandled DAP command: %s", command);
        dap_handle_simple_ack(client_id, request, command);
    }
}

//=============================================================================
// WebSocket Callbacks (registered with wsserver)
//=============================================================================

static void dap_on_connect(int client_id) {
    ESP_LOGI(TAG, "DAP client connected: %d", client_id);
    
    // Note: We don't overwrite if WebREPL is already connected
    // Multiple handlers can coexist
    if (!dap_client.active) {
        dap_client.client_id = client_id;
        dap_client.active = true;
        dap_client.initialized = false;
        dap_client.seq = 1;
        dap_client.stopped = false;
        dap_client.stop_reason = NULL;
    }
}

static void dap_on_disconnect(int client_id) {
    if (dap_client.client_id == client_id && dap_client.active) {
        ESP_LOGI(TAG, "DAP client disconnected: %d", client_id);
        dap_client.active = false;
        dap_client.initialized = false;
        
        // Clean up breakpoints
        dap_clear_all_breakpoints();
    }
}

/**
 * Handle incoming DAP message (TEXT frame)
 */
static void dap_on_message(int client_id, const uint8_t *data, size_t len, 
                           bool is_binary) {
    // Only handle TEXT frames (DAP protocol)
    // Binary frames are handled by WebREPL CB
    if (is_binary) {
        return; // Let WebREPL CB handle binary frames
    }
    
    ESP_LOGD(TAG, "DAP message received (%zu bytes)", len);
    
    // Parse DAP message
    size_t json_offset;
    cJSON *request = dap_parse_message((const char *)data, len, &json_offset);
    if (!request) {
        ESP_LOGE(TAG, "Failed to parse DAP message");
        return;
    }
    
    // Dispatch command
    dap_dispatch_command(client_id, request);
    
    cJSON_Delete(request);
}

//=============================================================================
// Module Initialization
//=============================================================================

/**
 * Start WebDAP server
 * 
 * Usage: webdap.start()
 * Registers callbacks with wsserver to handle TEXT frames as DAP messages
 */
static mp_obj_t webdap_start(void) {
    ESP_LOGI(TAG, "Starting webDAP...");
    
    if (!wsserver_is_running()) {
        ESP_LOGW(TAG, "wsserver not running - DAP will not work until wsserver starts");
    }
    
    // Register callbacks with wsserver
    // These callbacks coexist with WebREPL callbacks
    // Discrimination happens via is_binary flag
    if (!wsserver_register_c_callback(WSSERVER_EVENT_CONNECT, 
                                      (void *)dap_on_connect)) {
        ESP_LOGE(TAG, "Failed to register CONNECT callback");
        return mp_const_false;
    }
    
    if (!wsserver_register_c_callback(WSSERVER_EVENT_DISCONNECT, 
                                      (void *)dap_on_disconnect)) {
        ESP_LOGE(TAG, "Failed to register DISCONNECT callback");
        return mp_const_false;
    }
    
    if (!wsserver_register_c_callback(WSSERVER_EVENT_MESSAGE, 
                                      (void *)dap_on_message)) {
        ESP_LOGE(TAG, "Failed to register MESSAGE callback");
        return mp_const_false;
    }
    
    ESP_LOGI(TAG, "webDAP started successfully");
    ESP_LOGI(TAG, "DAP protocol available on TEXT frames (opcode 0x01)");
    ESP_LOGI(TAG, "WebREPL CB available on BINARY frames (opcode 0x02)");
    
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_0(webdap_start_obj, webdap_start);

/**
 * Stop WebDAP server
 */
static mp_obj_t webdap_stop(void) {
    ESP_LOGI(TAG, "Stopping webDAP...");
    
    if (dap_client.active) {
        dap_client.active = false;
        dap_client.initialized = false;
        dap_clear_all_breakpoints();
    }
    
    // Note: We don't unregister callbacks because they filter by is_binary
    // and don't interfere with WebREPL
    
    ESP_LOGI(TAG, "webDAP stopped");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(webdap_stop_obj, webdap_stop);

/**
 * Check if DAP client is connected
 */
static mp_obj_t webdap_is_connected(void) {
    return mp_obj_new_bool(dap_client.active && dap_client.initialized);
}
static MP_DEFINE_CONST_FUN_OBJ_0(webdap_is_connected_obj, webdap_is_connected);

//=============================================================================
// Module Definition
//=============================================================================

static const mp_rom_map_elem_t webdap_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_webdap) },
    
    // Functions
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&webdap_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&webdap_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_connected), MP_ROM_PTR(&webdap_is_connected_obj) },
};

static MP_DEFINE_CONST_DICT(webdap_module_globals, webdap_module_globals_table);

const mp_obj_module_t webdap_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&webdap_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_webdap, webdap_module);
