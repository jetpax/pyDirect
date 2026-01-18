/*
 * modhttpserver.c - HTTP server module for MicroPython using ESP-IDF HTTP server
 *
 * Provides HTTP server core functionality with:
 * - Dynamic URI handler registration
 * - Message queue for thread-safe Python callbacks
 * - Shared server handle for other modules
 *
 * Copyright (c) 2025 Jonathan Peace
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

// ESP-IDF includes
#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"

// MicroPython includes
#include "py/runtime.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/mphal.h"
#include "py/stream.h"
#include "py/builtin.h"  // For mp_vfs_open()
#include "extmod/vfs.h"  // For mp_vfs_stat()
#include "py/compile.h"
#include "py/persistentcode.h"

// Module is registered as part of esp32 module (see modesp32.c)

// External functions from webfiles module
extern const char* webfiles_get_mime_type(const char *path);
extern void webfiles_set_content_type_from_file(httpd_req_t *req, const char *filepath);
extern void webfiles_resolve_gzip(char *filepath, size_t filepath_size, const char *accept_encoding, bool *use_compression);

// File request structure (defined in modwebfiles.c)
// Contains: filepath[128], accept_encoding[64], use_compression
#define FILE_PATH_MAX 128
typedef struct {
    char filepath[FILE_PATH_MAX];
    char accept_encoding[64];
    bool use_compression;
} webfiles_request_t;

// Note: Scratch buffer moved to modwebfiles.c for better modularity

// Message types for queue
typedef enum {
    HTTP_MSG_WEBSOCKET = 1,
    HTTP_MSG_FILE = 2,
    HTTP_MSG_WEB = 3
} http_msg_type_t;

// Message structure for queue
typedef struct {
    http_msg_type_t type;
    int client_id;
    void *data;
    size_t data_len;
    void *user_data;
    mp_obj_t func;  // MicroPython function object
    httpd_req_t *req;  // HTTP request handle for async processing
} http_queue_msg_t;

// Forward declarations for internal functions
bool httpserver_queue_message(http_msg_type_t type, int client_id, const void *data, size_t data_len, void *user_data);
bool httpserver_queue_web_request(int handler_id, httpd_req_t *req, mp_obj_t func);

// Forward declaration for WebSocket message processing (from modwsserver.c)
extern void wsserver_process_websocket_msg(int client_id, const char *data, size_t data_len, bool is_binary);

// Simple callback for socket closures
static void (*external_close_callback)(int sockfd) = NULL;

// Function for external modules to register their cleanup function
void httpserver_register_external_close_cb(void (*func)(int sockfd)) {
    external_close_callback = func;
}

// Logger tag
static const char *TAG = "HTTPSERVER";

// Global queue for handling messages in the main task context
static QueueHandle_t http_msg_queue = NULL;
static SemaphoreHandle_t http_queue_mutex = NULL;
static bool http_queue_initialized = false;

// Maximum number of HTTP handlers
#define HTTP_HANDLER_MAX 12 // Enough for multiple modules (httpserver + wsserver + cwebrepl)

// Handler storage
typedef struct {
    bool active;          // Whether this handler is in use
    mp_obj_t func;        // MicroPython function to call for requests
    char *uri;            // URI pattern (dynamically allocated)
    httpd_method_t method; // HTTP method (GET, POST, etc.)
} http_handler_t;

static http_handler_t http_handlers[HTTP_HANDLER_MAX];

// Handler function pointers for ESP-IDF
#define HTTP_HANDLER_FUNC(n) \
    static esp_err_t httpserver_http_handler_##n(httpd_req_t *req) { \
        return httpserver_http_handler_impl(req); \
    }

// Forward declaration for handler implementation
static esp_err_t httpserver_http_handler_impl(httpd_req_t *req);

HTTP_HANDLER_FUNC(0)
HTTP_HANDLER_FUNC(1)
HTTP_HANDLER_FUNC(2)
HTTP_HANDLER_FUNC(3)
HTTP_HANDLER_FUNC(4)
HTTP_HANDLER_FUNC(5)
HTTP_HANDLER_FUNC(6)
HTTP_HANDLER_FUNC(7)
HTTP_HANDLER_FUNC(8)
HTTP_HANDLER_FUNC(9)
HTTP_HANDLER_FUNC(10)
HTTP_HANDLER_FUNC(11)

typedef esp_err_t (*http_handler_func_t)(httpd_req_t *req);
static const http_handler_func_t httpserver_handlers[HTTP_HANDLER_MAX] = {
    httpserver_http_handler_0,
    httpserver_http_handler_1,
    httpserver_http_handler_2,
    httpserver_http_handler_3,
    httpserver_http_handler_4,
    httpserver_http_handler_5,
    httpserver_http_handler_6,
    httpserver_http_handler_7,
    httpserver_http_handler_8,
    httpserver_http_handler_9,
    httpserver_http_handler_10,
    httpserver_http_handler_11
};

// Maximum concurrent HTTP server connections
#define HTTPD_MAX_CONNECTIONS 10 // Match max_open_sockets in config

// Handle to HTTP server
static httpd_handle_t http_server = NULL;

// Handle to HTTPS server (optional, runs alongside HTTP)
static httpd_handle_t https_server = NULL;

// SSL/TLS configuration
static bool use_https = false;
static uint8_t *server_cert = NULL;
static size_t server_cert_len = 0;
static uint8_t *server_key = NULL;
static size_t server_key_len = 0;

// Export function for webrepl module
httpd_handle_t mp_httpserver_get_handle(void) {
    return http_server;
}

// Current HTTP request being processed (for MicroPython access)
static httpd_req_t *current_request = NULL;

// Connection tracking for WebSocket clients
static struct {
    int count;
    SemaphoreHandle_t mutex;
} connection_tracking = {0, NULL};


// Get MIME type function from webfiles module
extern const char* webfiles_get_mime_type(const char *path);

//=============================================================================
// Shared HTTP Worker Thread State Initialization
//=============================================================================
// HTTP worker threads are created by ESP-IDF and don't have MicroPython
// thread state. This function initializes MP state on-demand so the thread
// can safely call MicroPython APIs with GIL acquisition.
//
// Used by both webfiles and webrepl modules for direct GIL file operations.
//
// EXPERIMENTAL - potential issues:
// 1. Thread state allocated on heap (persists for thread lifetime)
// 2. Thread not in MP's thread linked list (GC might miss it)
// 3. Stack bounds may not be perfect
bool httpserver_ensure_mp_thread_state() {
    // Check if thread already has state
    mp_state_thread_t *existing = mp_thread_get_state();
    if (existing != NULL) {
        ESP_LOGD(TAG, "Thread already has MP state: %p", existing);
        return true;  // Already initialized
    }
    
    // Allocate thread state on heap (will persist for worker thread lifetime)
    mp_state_thread_t *ts = heap_caps_malloc(sizeof(mp_state_thread_t), MALLOC_CAP_8BIT);
    if (!ts) {
        ESP_LOGE(TAG, "Failed to allocate thread state");
        return false;
    }
    
    // Initialize thread state (inherit globals from main thread)
    // Stack size is just a hint - worker threads have their own stacks
    mp_thread_init_state(ts, 8192, NULL, NULL);
    
    ESP_LOGD(TAG, "Initialized MP thread state for HTTP worker %p (task %p)", 
             ts, xTaskGetCurrentTaskHandle());
    
    return true;
}

// Queue a message for processing in the main task
// CONTEXT: Any Task (typically ESP-IDF HTTP Server Task)
// This function is called to queue messages for processing in the main task
// TRANSITION: Current Task → Main MicroPython Task
bool httpserver_queue_message(http_msg_type_t type, int client_id, const void *data, size_t data_len, void *user_data) {
    if (!http_queue_initialized) {
        ESP_LOGE(TAG, "Queue not initialized");
        return false;
    }

    if (!data && data_len > 0) {
        ESP_LOGE(TAG, "Invalid data pointer with non-zero length");
        return false;
    }

    // Take mutex to protect queue
    if (xSemaphoreTake(http_queue_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex");
        return false;
    }

    // Create a message to queue
    http_queue_msg_t msg = {0};

    msg.type = type;
    msg.client_id = client_id;
    msg.user_data = user_data;

    // Special case for HTTP_MSG_WEB is handled by httpserver_queue_web_request
    if (type == HTTP_MSG_WEB) {
        ESP_LOGE(TAG, "HTTP_MSG_WEB must use httpserver_queue_web_request");
        xSemaphoreGive(http_queue_mutex);
        return false;
    }

    // For other message types, copy the data if needed
    if (data_len > 0) {
        char *data_copy = malloc(data_len + 1);
        if (!data_copy) {
            ESP_LOGE(TAG, "Failed to allocate memory for data copy");
            xSemaphoreGive(http_queue_mutex);
            return false;
        }

        memcpy(data_copy, data, data_len);
        data_copy[data_len] = '\0';  // Ensure null termination

        msg.data = data_copy;
        msg.data_len = data_len;
    }

    // Queue the message
    if (xQueueSend(http_msg_queue, &msg, 0) != pdTRUE) {
        // Queue is full, free the data if we allocated it
        if (msg.data) {
            free(msg.data);
        }
        ESP_LOGE(TAG, "Failed to queue message - queue is full");
        xSemaphoreGive(http_queue_mutex);
        return false;
    }

    // Message successfully queued
    ESP_LOGD(TAG, "Message queued successfully (type %d, client %d)", type, client_id);
    ESP_LOGD(TAG, "DIAGNOSTIC: Queue has %d messages waiting", uxQueueMessagesWaiting(http_msg_queue));

    if (msg.data) {
        ESP_LOGD(TAG, "QUEUE ITEM: type=%d, client=%d, data_len=%d, data_ptr=%p, user_data=%p",
                msg.type, msg.client_id, (int)msg.data_len, msg.data, msg.user_data);
        ESP_LOGD(TAG, "QUEUE DATA: '%s'", (char*)msg.data);
    }

    xSemaphoreGive(http_queue_mutex);
    return true;
}

// Specialized function for queuing HTTP web requests with a MicroPython function
bool httpserver_queue_web_request(int handler_id, httpd_req_t *req, mp_obj_t func) {
    if (!http_queue_initialized) {
        ESP_LOGE(TAG, "Queue not initialized");
        return false;
    }

    if (!req) {
        ESP_LOGE(TAG, "NULL request for web request");
        return false;
    }

    // Create a copy of the request that we can process asynchronously
    httpd_req_t *req_copy = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &req_copy);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create async request: %d", err);
        return false;
    }

    // Take mutex to protect queue
    if (xSemaphoreTake(http_queue_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex");
        // Release the request copy since we won't be using it
        httpd_req_async_handler_complete(req_copy);
        return false;
    }

    // Create a message to queue
    http_queue_msg_t msg = {0};

    msg.type = HTTP_MSG_WEB;
    msg.client_id = handler_id;
    msg.user_data = NULL;
    msg.func = func;     // Store the function reference
    msg.req = req_copy;  // Store the COPY of the request handle

    // Queue the message
    if (xQueueSend(http_msg_queue, &msg, 0) != pdTRUE) {
        // Queue is full
        ESP_LOGE(TAG, "Failed to queue web request - queue is full");
        // Release the request copy since we won't be using it
        httpd_req_async_handler_complete(req_copy);
        xSemaphoreGive(http_queue_mutex);
        return false;
    }

    // Message successfully queued
    ESP_LOGD(TAG, "HTTP request queued successfully (type %d, handler %d)", msg.type, msg.client_id);
    ESP_LOGD(TAG, "DIAGNOSTIC: Queue has %d messages waiting", uxQueueMessagesWaiting(http_msg_queue));
    ESP_LOGD(TAG, "QUEUE ITEM: type=%d, client=%d, data_len=%d, data_ptr=%p, user_data=%p, req=%p",
            msg.type, msg.client_id, (int)msg.data_len, msg.data, msg.user_data, msg.req);

    xSemaphoreGive(http_queue_mutex);
    return true;
}

// ------------------------------------------------------------------------
// Web request processing function
// CONTEXT: Main MicroPython Task
// This function processes queued web requests in the main task context
// ------------------------------------------------------------------------

void httpserver_process_web_request(http_queue_msg_t *msg) {
    ESP_LOGD(TAG, "Processing web request: msg=%p", msg);

    if (!msg) {
        ESP_LOGE(TAG, "NULL message handle");
        return;
    }

    if (!msg->req) {
        ESP_LOGE(TAG, "NULL request handle");
        return;
    }

    // Get handler ID (passed in client_id field)
    int handler_id = msg->client_id;
    if (handler_id < 0 || handler_id >= HTTP_HANDLER_MAX || !http_handlers[handler_id].active) {
        ESP_LOGE(TAG, "Invalid handler ID: %d", handler_id);
        httpd_resp_set_status(msg->req, "500 Internal Server Error");
        httpd_resp_sendstr(msg->req, "Invalid handler ID");
        httpd_req_async_handler_complete(msg->req);
        return;
    }

    ESP_LOGI(TAG, "Processing request: %s (handler %d)", msg->req->uri, handler_id);

    // Get the MicroPython function
    mp_obj_t func = http_handlers[handler_id].func;

    ESP_LOGI(TAG, "Retrieved function %p for handler %d", func, handler_id);

    if (func == MP_OBJ_NULL) {
        ESP_LOGE(TAG, "MicroPython function is NULL for handler %d", handler_id);
        httpd_resp_set_status(msg->req, "500 Internal Server Error");
        httpd_resp_sendstr(msg->req, "Function error");
        // Complete the async request
        httpd_req_async_handler_complete(msg->req);
        return;
    }
    
    if (!mp_obj_is_callable(func)) {
        ESP_LOGE(TAG, "Stored object is not callable for handler %d", handler_id);
        httpd_resp_set_status(msg->req, "500 Internal Server Error");
        httpd_resp_sendstr(msg->req, "Handler not callable");
        // Complete the async request
        httpd_req_async_handler_complete(msg->req);
        return;
    }
    

    // Set current request for access in MicroPython functions
    current_request = msg->req;

    // Prepare arguments for the MicroPython function call
    mp_obj_t args[3];  // URI, POST data, Remote IP
    int arg_count = 1;

    // Push URI as first argument
    args[0] = mp_obj_new_str(msg->req->uri, strlen(msg->req->uri));

    // Get Client IP
    char client_ip[64] = "0.0.0.0";
    int sockfd = httpd_req_to_sockfd(msg->req);
    if (sockfd >= 0) {
        struct sockaddr_storage addr;
        socklen_t len = sizeof(addr);
        if (getpeername(sockfd, (struct sockaddr *)&addr, &len) == 0) {
             if (addr.ss_family == AF_INET) {
                struct sockaddr_in *s = (struct sockaddr_in *)&addr;
                inet_ntop(AF_INET, &s->sin_addr, client_ip, sizeof(client_ip));
            } else if (addr.ss_family == AF_INET6) {
                struct sockaddr_in6 *s = (struct sockaddr_in6 *)&addr;
                inet_ntop(AF_INET6, &s->sin6_addr, client_ip, sizeof(client_ip));
            }
        }
    }
    ESP_LOGI(TAG, "Client IP: %s", client_ip);

    // Check if this is a POST request and handle POST data
    if (msg->req->method == HTTP_POST) {
        ESP_LOGI(TAG, "Processing POST request data");

        // Get content length
        int content_len = msg->req->content_len;
        ESP_LOGI(TAG, "POST content length: %d", content_len);

        if (content_len > 0) {
            // Allocate buffer for POST data
            char *post_data = malloc(content_len + 1);
            if (post_data) {
                // Read POST data
                int received = httpd_req_recv(msg->req, post_data, content_len);
                if (received >= 0) {
                    // Null-terminate the data (safe even if received == 0)
                    post_data[received] = '\0';
                    ESP_LOGI(TAG, "Received POST data (%d bytes): %s", received, post_data);

                    // Push POST data as second argument
                    args[1] = mp_obj_new_str(post_data, received);
                    arg_count = 2;  // Now we have 2 arguments
                } else {
                    ESP_LOGE(TAG, "Failed to read POST data, error: %d", received);
                    // Push empty string as second argument
                    args[1] = mp_obj_new_str("", 0);
                    arg_count = 2;
                }
                free(post_data);
            } else {
                ESP_LOGE(TAG, "Failed to allocate memory for POST data");
                // Push empty string as second argument
                args[1] = mp_obj_new_str("", 0);
                arg_count = 2;
            }
        } else {
            // No content, push empty string as second argument
            args[1] = mp_obj_new_str("", 0);
            arg_count = 2;
        }
        
    } else {
         // Not a POST request, push empty pointer
         args[1] = mp_const_none;
         arg_count = 2;
    }

    // Push Client IP as third argument
    args[2] = mp_obj_new_str(client_ip, strlen(client_ip));
    arg_count = 3;

    // Call the MicroPython function
    nlr_buf_t nlr;
    mp_obj_t result = MP_OBJ_NULL;

    if (nlr_push(&nlr) == 0) {
        result = mp_call_function_n_kw(func, arg_count, 0, args);
        nlr_pop();
    } else {
        // Handle exception
        mp_obj_t exc = MP_OBJ_FROM_PTR(nlr.ret_val);
        mp_obj_print_exception(&mp_plat_print, exc);

        // Send error response
        httpd_resp_set_type(msg->req, "text/html");
        httpd_resp_send(msg->req, "Internal Server Error", strlen("Internal Server Error"));
        // Clear current_request BEFORE completing async handler
        current_request = NULL;
        // Complete the async request
        httpd_req_async_handler_complete(msg->req);
        return;
    }

    // Check the result
    if (result != MP_OBJ_NULL && mp_obj_is_str_or_bytes(result)) {
        // Function returned a string, send it as response
        const char *response_str = mp_obj_str_get_str(result);
        size_t response_len = strlen(response_str);
        httpd_resp_set_type(msg->req, "application/json");
        // Enable CORS for browser access
        httpd_resp_set_hdr(msg->req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(msg->req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        httpd_resp_set_hdr(msg->req, "Access-Control-Allow-Headers", "Content-Type");
        // Force connection close to prevent keep-alive issues
        httpd_resp_set_hdr(msg->req, "Connection", "close");
        // Use httpd_resp_send with explicit length for async responses
        httpd_resp_send(msg->req, response_str, response_len);
    } else {
        // Function didn't return a string - assume httpserver.send() was used
        // If neither return value nor httpserver.send() provided a response,
        // send a default empty response to avoid hanging the connection
        httpd_resp_set_type(msg->req, "text/html");
        // Enable CORS
        httpd_resp_set_hdr(msg->req, "Access-Control-Allow-Origin", "*");
        // Force connection close
        httpd_resp_set_hdr(msg->req, "Connection", "close");
        httpd_resp_send(msg->req, "", 0);
    }

    // Clear current_request BEFORE completing async handler
    current_request = NULL;

    // Complete the async request - ALWAYS call this to release the request
    httpd_req_async_handler_complete(msg->req);
}


// ------------------------------------------------------------------------
// MicroPython module interface functions
// ------------------------------------------------------------------------

// Process queued messages (called from MicroPython)
static mp_obj_t httpserver_process_queue(void) {
    if (!http_msg_queue) {
        ESP_LOGW(TAG, "Queue not initialized");
        return mp_obj_new_int(0);
    }

    // Count of messages processed in this call
    int processed = 0;

    // Process up to 10 messages in a single call to avoid blocking
    for (int i = 0; i < 10; i++) {
        // Take mutex before accessing queue
        if (xSemaphoreTake(http_queue_mutex, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Failed to take mutex, will retry");
            break;
        }

        // Process one message from the queue
        http_queue_msg_t msg;
        if (xQueueReceive(http_msg_queue, &msg, 0) == pdTRUE) {
            // Release mutex while processing message
            xSemaphoreGive(http_queue_mutex);

            // Count this message
            processed++;

            // Diagnostic logging for queue state
            ESP_LOGD(TAG, "QUEUE ITEM: type=%d, client=%d, data_len=%d, data_ptr=%p, user_data=%p, req=%p",
                    msg.type, msg.client_id, msg.data_len, msg.data, msg.user_data, msg.req);

            // Process message based on type
            switch (msg.type) {
                case HTTP_MSG_WEBSOCKET:
                    if (msg.data) {
                        ESP_LOGD(TAG, "QUEUE DATA: '%.*s'", msg.data_len, (char*)msg.data);
                    } else {
                        ESP_LOGI(TAG, "QUEUE DATA: '' (connect/disconnect event)");
                    }
                    // Extract is_binary flag from user_data
                    bool is_binary = false;
                    if (msg.user_data) {
                        is_binary = (*((char *)msg.user_data)) != 0;
                        ESP_LOGD(TAG, "Extracted is_binary=%d from user_data=%p", is_binary, msg.user_data);
                        free(msg.user_data);  // Free the flag storage
                    } else {
                        ESP_LOGI(TAG, "No user_data in message (is_binary=0)");
                    }
                    // WebSocket handling - call processing function from modwsserver.c
                    ESP_LOGD(TAG, "Processing WebSocket message: client=%d, len=%d, is_binary=%d", 
                            msg.client_id, (int)msg.data_len, is_binary);
                    wsserver_process_websocket_msg(msg.client_id, msg.data, msg.data_len, is_binary);
                    // Free the data buffer we allocated
                    if (msg.data) {
                        free(msg.data);
                    }
                    break;

                case HTTP_MSG_WEB:
                    ESP_LOGD(TAG, "Processing web request from queue");
                    if (msg.req == NULL) {
                        ESP_LOGE(TAG, "CRITICAL ERROR: HTTP request pointer is NULL, skipping processing");
                        break;
                    }
                    httpserver_process_web_request(&msg);
                    // Note: httpserver_process_web_request handles async completion internally
                    break;

                default:
                    ESP_LOGW(TAG, "Unknown message type: %d", msg.type);
                    // Free data if it was allocated
                    if (msg.data) {
                        free(msg.data);
                    }
                    // If it's a request that wasn't processed, complete it
                    if (msg.req) {
                        httpd_req_async_handler_complete(msg.req);
                    }
                    break;
            }
        } else {
            // No messages in queue
            xSemaphoreGive(http_queue_mutex);
            break;
        }
    }

    // Return the number of messages processed
    return mp_obj_new_int(processed);
}
static MP_DEFINE_CONST_FUN_OBJ_0(httpserver_process_queue_obj, httpserver_process_queue);

// Start the HTTP server (and optionally HTTPS)
static mp_obj_t httpserver_start(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    // Define allowed keyword arguments
    // Note: Using numbered args instead of 'port' to avoid QSTR conflicts
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_, MP_ARG_INT, {.u_int = 80} },  // First positional arg (port)
        { MP_QSTR_cert_file, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_key_file, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (http_server != NULL) {
        ESP_LOGW(TAG, "HTTP server already running");
        return mp_obj_new_bool(true);  // Server already running
    }

    // Initialize esp_netif if not already done (required for HTTP server)
    // This is safe to call multiple times
    esp_err_t netif_err = esp_netif_init();
    if (netif_err != ESP_OK && netif_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize esp_netif: %d (0x%x)", netif_err, netif_err);
        return mp_obj_new_bool(false);
    }
    ESP_LOGI(TAG, "Network interface initialized (status: %d)", netif_err);
    
    // Initialize connection tracking
    connection_tracking.mutex = xSemaphoreCreateMutex();
    if (!connection_tracking.mutex) {
        ESP_LOGE(TAG, "Failed to create connection tracking mutex");
        return mp_obj_new_bool(false);
    }

    // Check if cert and key files are provided for HTTPS
    bool has_cert = args[1].u_obj != mp_const_none && mp_obj_is_str(args[1].u_obj);
    bool has_key = args[2].u_obj != mp_const_none && mp_obj_is_str(args[2].u_obj);

    // Load certificates if both are provided
    if (has_cert && has_key) {
        const char *cert_path = mp_obj_str_get_str(args[1].u_obj);
        const char *key_path = mp_obj_str_get_str(args[2].u_obj);

        ESP_LOGI(TAG, "Loading SSL certificate from: %s", cert_path);
        ESP_LOGI(TAG, "Loading SSL key from: %s", key_path);

        // Read certificate file using MicroPython VFS (like webrepl does)
        mp_obj_t cert_args[2] = {
            args[1].u_obj,  // filename
            MP_OBJ_NEW_QSTR(MP_QSTR_rb)  // mode
        };
        
        mp_obj_t cert_file_obj = mp_builtin_open(2, cert_args, (mp_map_t*)&mp_const_empty_map);
        if (cert_file_obj == MP_OBJ_NULL) {
            ESP_LOGE(TAG, "Failed to open certificate file: %s", cert_path);
            vSemaphoreDelete(connection_tracking.mutex);
            connection_tracking.mutex = NULL;
            return mp_obj_new_bool(false);
        }

        // Get file size using seek
        const mp_stream_p_t *cert_stream = mp_get_stream(cert_file_obj);
        struct mp_stream_seek_t seek_s;
        seek_s.offset = 0;
        seek_s.whence = 2; // SEEK_END
        int cert_err;
        cert_stream->ioctl(cert_file_obj, MP_STREAM_SEEK, (mp_uint_t)(uintptr_t)&seek_s, &cert_err);
        long cert_size = seek_s.offset;
        
        // Seek back to beginning
        seek_s.offset = 0;
        seek_s.whence = 0; // SEEK_SET
        cert_stream->ioctl(cert_file_obj, MP_STREAM_SEEK, (mp_uint_t)(uintptr_t)&seek_s, &cert_err);

        server_cert = malloc(cert_size + 1);
        if (!server_cert) {
            ESP_LOGE(TAG, "Failed to allocate memory for certificate (%ld bytes)", cert_size);
            mp_stream_close(cert_file_obj);
            vSemaphoreDelete(connection_tracking.mutex);
            connection_tracking.mutex = NULL;
            return mp_obj_new_bool(false);
        }

        size_t read_size = cert_stream->read(cert_file_obj, server_cert, cert_size, &cert_err);
        mp_stream_close(cert_file_obj);
        server_cert[read_size] = '\0';
        server_cert_len = read_size + 1;

        // Read key file using MicroPython VFS (like webrepl does)
        mp_obj_t key_args[2] = {
            args[2].u_obj,  // filename
            MP_OBJ_NEW_QSTR(MP_QSTR_rb)  // mode
        };
        
        mp_obj_t key_file_obj = mp_builtin_open(2, key_args, (mp_map_t*)&mp_const_empty_map);
        if (key_file_obj == MP_OBJ_NULL) {
            ESP_LOGE(TAG, "Failed to open key file: %s", key_path);
            free(server_cert);
            server_cert = NULL;
            vSemaphoreDelete(connection_tracking.mutex);
            connection_tracking.mutex = NULL;
            return mp_obj_new_bool(false);
        }

        // Get file size using seek
        const mp_stream_p_t *key_stream = mp_get_stream(key_file_obj);
        seek_s.offset = 0;
        seek_s.whence = 2; // SEEK_END
        int key_err;
        key_stream->ioctl(key_file_obj, MP_STREAM_SEEK, (mp_uint_t)(uintptr_t)&seek_s, &key_err);
        long key_size = seek_s.offset;
        
        // Seek back to beginning
        seek_s.offset = 0;
        seek_s.whence = 0; // SEEK_SET
        key_stream->ioctl(key_file_obj, MP_STREAM_SEEK, (mp_uint_t)(uintptr_t)&seek_s, &key_err);

        server_key = malloc(key_size + 1);
        if (!server_key) {
            ESP_LOGE(TAG, "Failed to allocate memory for key (%ld bytes)", key_size);
            mp_stream_close(key_file_obj);
            free(server_cert);
            server_cert = NULL;
            vSemaphoreDelete(connection_tracking.mutex);
            connection_tracking.mutex = NULL;
            return mp_obj_new_bool(false);
        }

        read_size = key_stream->read(key_file_obj, server_key, key_size, &key_err);
        mp_stream_close(key_file_obj);
        server_key[read_size] = '\0';
        server_key_len = read_size + 1;

        use_https = true;
        ESP_LOGI(TAG, "SSL certificates loaded (cert: %zu bytes, key: %zu bytes)",
                 server_cert_len, server_key_len);
    } else if (has_cert || has_key) {
        ESP_LOGE(TAG, "Both cert_file and key_file required for HTTPS");
        vSemaphoreDelete(connection_tracking.mutex);
        connection_tracking.mutex = NULL;
        return mp_obj_new_bool(false);
    }

    // Log current memory status
    size_t free_heap = esp_get_free_heap_size();
    size_t min_free_heap = esp_get_minimum_free_heap_size();
    size_t largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    ESP_LOGI(TAG, "Memory status: free_heap=%u, min_free_heap=%u, largest_block=%u", 
             free_heap, min_free_heap, largest_free_block);

    // Initialize the queue for thread-safe message passing BEFORE starting servers
    if (!http_queue_initialized) {
        http_msg_queue = xQueueCreate(50, sizeof(http_queue_msg_t));
        http_queue_mutex = xSemaphoreCreateMutex();

        if (http_msg_queue != NULL && http_queue_mutex != NULL) {
            http_queue_initialized = true;
            ESP_LOGI(TAG, "HTTP queue initialized");
        } else {
            ESP_LOGE(TAG, "Failed to create HTTP queue");
            vSemaphoreDelete(connection_tracking.mutex);
            connection_tracking.mutex = NULL;
            return mp_obj_new_bool(false);
        }
    }

    // Initialize handler array on first start
    static bool handlers_initialized = false;
    if (!handlers_initialized) {
        for (int i = 0; i < HTTP_HANDLER_MAX; i++) {
            http_handlers[i].active = false;
            http_handlers[i].func = MP_OBJ_NULL;
            http_handlers[i].uri = NULL;
            http_handlers[i].method = HTTP_GET;
        }
        handlers_initialized = true;
        ESP_LOGI(TAG, "HTTP handlers initialized");
    }

    // Start HTTP server on specified port
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;
    config.max_uri_handlers = 12;
    config.max_open_sockets = 4;  // Reduced from 10 - LWIP default allows max 7 total
    config.lru_purge_enable = true;
    config.backlog_conn = 5;
    config.server_port = args[0].u_int;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    esp_err_t ret = httpd_start(&http_server, &config);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %d (0x%x)", ret, ret);
        // Cleanup
        if (server_cert) {
            free(server_cert);
            server_cert = NULL;
        }
        if (server_key) {
            free(server_key);
            server_key = NULL;
        }
        use_https = false;
        vSemaphoreDelete(connection_tracking.mutex);
        connection_tracking.mutex = NULL;
        return mp_obj_new_bool(false);
    }

    ESP_LOGI(TAG, "HTTP server started successfully on port %d", config.server_port);

    // Start HTTPS server if certificates were loaded
    if (use_https) {
        httpd_ssl_config_t https_conf = HTTPD_SSL_CONFIG_DEFAULT();
        
        // Configure HTTPS parameters
        https_conf.httpd.uri_match_fn = httpd_uri_match_wildcard;
        https_conf.httpd.stack_size = 10240;  // HTTPS needs more stack
        https_conf.httpd.max_uri_handlers = 12;
        https_conf.httpd.max_open_sockets = 3;  // SSL uses more memory per socket, keep total under 7
        https_conf.httpd.lru_purge_enable = true;
        https_conf.httpd.backlog_conn = 5;
        https_conf.port_secure = 443;
        https_conf.port_insecure = 80;
        
        // Disable session tickets to avoid session resumption issues
        https_conf.session_tickets = false;
        
        // Set certificates
        https_conf.servercert = server_cert;
        https_conf.servercert_len = server_cert_len;
        https_conf.prvtkey_pem = server_key;
        https_conf.prvtkey_len = server_key_len;

        ESP_LOGI(TAG, "Starting HTTPS server on port 443");
        
        ret = httpd_ssl_start(&https_server, &https_conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start HTTPS server: %d (0x%x)", ret, ret);
            // Continue with HTTP only - don't fail completely
            use_https = false;
            free(server_cert);
            free(server_key);
            server_cert = NULL;
            server_key = NULL;
        } else {
            ESP_LOGI(TAG, "HTTPS server started successfully on port 443");
        }
    }

    return mp_obj_new_bool(true);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(httpserver_start_obj, 0, httpserver_start);

// Register a URI handler
static mp_obj_t httpserver_on(size_t n_args, const mp_obj_t *args) {
    httpd_method_t http_method = HTTP_GET; // Default method

    if (n_args < 2 || http_server == NULL || !http_queue_initialized) {
        mp_raise_ValueError(n_args < 2 ? MP_ERROR_TEXT("Missing arguments") :
                           http_server == NULL ? MP_ERROR_TEXT("Server not started") :
                           MP_ERROR_TEXT("Queue not initialized"));
    }

    if (!mp_obj_is_str(args[0]) || !mp_obj_is_callable(args[1])) {
        mp_raise_TypeError(MP_ERROR_TEXT("String and callable required"));
    }

    // Check for optional method argument
    if (n_args >= 3) {
        if (!mp_obj_is_str(args[2])) {
            mp_raise_TypeError(MP_ERROR_TEXT("Method must be a string"));
        }
        const char *method_str = mp_obj_str_get_str(args[2]);
        if (strcasecmp(method_str, "POST") == 0) {
            http_method = HTTP_POST;
        } else if (strcasecmp(method_str, "GET") != 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("Method must be 'GET' or 'POST'"));
        }
    }

    const char *uri = mp_obj_str_get_str(args[0]);
    ESP_LOGI(TAG, "Registering handler for URI: %s, Method: %s", uri,
             http_method == HTTP_GET ? "GET" : "POST");
    

    // Find a free handler slot
    int slot = -1;
    for (int i = 0; i < HTTP_HANDLER_MAX; i++) {
        if (!http_handlers[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("No more handler slots available"));
    }

    // Store handler info
    http_handlers[slot].active = true;
    http_handlers[slot].func = args[1];  // Store the function reference
    http_handlers[slot].uri = strdup(uri);  // Allocate and store URI
    http_handlers[slot].method = http_method;  // Store HTTP method
    
    if (http_handlers[slot].uri == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for URI");
        http_handlers[slot].active = false;
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("Failed to allocate URI memory"));
    }
    
    // Prevent garbage collection of the function object
    // by making it a root pointer (persistent)
    // NOTE: This keeps the function alive for the lifetime of the handler
    // TODO: May need to handle cleanup in stop() to avoid memory leaks

    ESP_LOGI(TAG, "Stored function %p for handler slot %d (URI: %s, method: %d)", 
             http_handlers[slot].func, slot, http_handlers[slot].uri, http_method);

    // Register the handler with ESP-IDF HTTP server
    httpd_uri_t http_uri = {
        .uri       = uri,
        .method    = http_method,
        .handler   = httpserver_handlers[slot],  // Use the appropriate handler function
        .user_ctx  = (void*)(intptr_t)slot
    };

    esp_err_t ret = httpd_register_uri_handler(http_server, &http_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register URI handler on HTTP: %d", ret);
        http_handlers[slot].active = false;
        free(http_handlers[slot].uri);  // Free allocated URI memory
        http_handlers[slot].uri = NULL;
        return mp_obj_new_bool(false);
    }

    // Register OPTIONS handler for CORS preflight (for all handlers, not just POST)
    // This enables browser cross-origin requests
    httpd_uri_t options_uri = {
        .uri       = uri,
        .method    = HTTP_OPTIONS,
        .handler   = httpserver_handlers[slot],  // Uses same handler which checks for OPTIONS
        .user_ctx  = (void*)(intptr_t)slot
    };
    ret = httpd_register_uri_handler(http_server, &options_uri);
    if (ret != ESP_OK && ret != ESP_ERR_HTTPD_HANDLER_EXISTS) {
        ESP_LOGW(TAG, "Failed to register OPTIONS handler on HTTP: %d (may already exist)", ret);
    }

    // Also register on HTTPS server if available
    if (https_server != NULL) {
        ret = httpd_register_uri_handler(https_server, &http_uri);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register URI handler on HTTPS: %d", ret);
            // Don't fail completely - HTTP handler is registered
        } else {
            ESP_LOGI(TAG, "Handler also registered on HTTPS server");
        }
        
        // Register OPTIONS on HTTPS too
        ret = httpd_register_uri_handler(https_server, &options_uri);
        if (ret != ESP_OK && ret != ESP_ERR_HTTPD_HANDLER_EXISTS) {
            ESP_LOGW(TAG, "Failed to register OPTIONS handler on HTTPS: %d", ret);
        }
    }

    // Return the handler slot
    return mp_obj_new_int(slot);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(httpserver_on_obj, 2, 3, httpserver_on);

// Unregister a URI handler
static mp_obj_t httpserver_off(size_t n_args, const mp_obj_t *args) {
    httpd_method_t http_method = HTTP_GET; // Default method

    if (n_args < 1 || http_server == NULL) {
        mp_raise_ValueError(n_args < 1 ? MP_ERROR_TEXT("Missing URI argument") :
                           MP_ERROR_TEXT("Server not started"));
    }

    if (!mp_obj_is_str(args[0])) {
        mp_raise_TypeError(MP_ERROR_TEXT("URI must be a string"));
    }

    // Check for optional method argument
    if (n_args >= 2) {
        if (!mp_obj_is_str(args[1])) {
            mp_raise_TypeError(MP_ERROR_TEXT("Method must be a string"));
        }
        const char *method_str = mp_obj_str_get_str(args[1]);
        if (strcasecmp(method_str, "POST") == 0) {
            http_method = HTTP_POST;
        } else if (strcasecmp(method_str, "GET") != 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("Method must be 'GET' or 'POST'"));
        }
    }

    const char *uri = mp_obj_str_get_str(args[0]);
    ESP_LOGI(TAG, "Unregistering handler for URI: %s, Method: %s", uri,
             http_method == HTTP_GET ? "GET" : "POST");


    // Find the matching handler slot first
    int slot = -1;
    for (int i = 0; i < HTTP_HANDLER_MAX; i++) {
        if (http_handlers[i].active && 
            http_handlers[i].uri != NULL &&
            strcmp(http_handlers[i].uri, uri) == 0 &&
            http_handlers[i].method == http_method) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        ESP_LOGW(TAG, "Handler not found for URI: %s, method: %d", uri, http_method);
        return mp_obj_new_bool(false);
    }

    // Unregister from ESP-IDF HTTP server
    esp_err_t ret = httpd_unregister_uri_handler(http_server, uri, http_method);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unregister URI handler from HTTP: %d", ret);
        // Still free our slot even if ESP-IDF unregister failed
    }

    // Also unregister from HTTPS server if available
    if (https_server != NULL) {
        ret = httpd_unregister_uri_handler(https_server, uri, http_method);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to unregister URI handler from HTTPS: %d", ret);
        }
    }

    // Free the handler slot and URI memory
    http_handlers[slot].active = false;
    http_handlers[slot].func = MP_OBJ_NULL;
    if (http_handlers[slot].uri != NULL) {
        free(http_handlers[slot].uri);
        http_handlers[slot].uri = NULL;
    }

    ESP_LOGI(TAG, "Handler unregistered successfully for URI: %s (slot %d)", uri, slot);

    return mp_obj_new_bool(true);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(httpserver_off_obj, 1, 2, httpserver_off);

// Stop the HTTP server (and HTTPS if running)
static mp_obj_t httpserver_stop(void) {
    if (http_server == NULL) {
        return mp_obj_new_bool(false);  // Server not running
    }

    // Clean up handler registrations
    for (int i = 0; i < HTTP_HANDLER_MAX; i++) {
        if (http_handlers[i].active) {
            http_handlers[i].active = false;
            http_handlers[i].func = MP_OBJ_NULL;
            // Free URI memory
            if (http_handlers[i].uri != NULL) {
                free(http_handlers[i].uri);
                http_handlers[i].uri = NULL;
            }
        }
    }

    // Stop HTTPS server if running
    esp_err_t ret = ESP_OK;
    if (https_server != NULL) {
        ESP_LOGI(TAG, "Stopping HTTPS server");
        esp_err_t https_ret = httpd_ssl_stop(https_server);
        if (https_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop HTTPS server: %d", https_ret);
        }
        https_server = NULL;
        
        // Free certificates
        if (server_cert) {
            free(server_cert);
            server_cert = NULL;
            server_cert_len = 0;
        }
        if (server_key) {
            free(server_key);
            server_key = NULL;
            server_key_len = 0;
        }
        use_https = false;
    }

    // Stop HTTP server
    ret = httpd_stop(http_server);
    http_server = NULL;

    // Clean up connection tracking
    if (connection_tracking.mutex) {
        vSemaphoreDelete(connection_tracking.mutex);
        connection_tracking.mutex = NULL;
    }

    return mp_obj_new_bool(ret == ESP_OK);
}
static MP_DEFINE_CONST_FUN_OBJ_0(httpserver_stop_obj, httpserver_stop);

// Simple wrapper around httpd_resp_send for async responses
static mp_obj_t httpserver_send(mp_obj_t content_obj) {
    if (!mp_obj_is_str_or_bytes(content_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("Content must be a string"));
    }

    const char* content = mp_obj_str_get_str(content_obj);
    size_t content_len = strlen(content);

    // Get the current request
    httpd_req_t* req = current_request;
    if (!req) {
        mp_raise_ValueError(MP_ERROR_TEXT("No active request"));
    }

    // Set content type and send the response with explicit length
    httpd_resp_set_type(req, "text/html");
    esp_err_t ret = httpd_resp_send(req, content, content_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send response: %d", ret);
        return mp_obj_new_bool(false);
    }

    return mp_obj_new_bool(true);
}
static MP_DEFINE_CONST_FUN_OBJ_1(httpserver_send_obj, httpserver_send);

// Get HTTP server handle (for advanced usage)
httpd_handle_t httpserver_get_handle(void) {
    return http_server;
}

// Get HTTPS server handle (for WebSocket on HTTPS)
httpd_handle_t httpserver_get_https_handle(void) {
    return https_server;
}

// ------------------------------------------------------------------------
// HTTP Handler implementation
// ------------------------------------------------------------------------

// Handle OPTIONS preflight requests for CORS
static esp_err_t httpserver_options_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "OPTIONS preflight request for: %s", req->uri);
    
    // Set CORS headers for preflight
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Authorization");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");  // Cache preflight for 24 hours
    httpd_resp_set_hdr(req, "Connection", "close");
    
    // Send empty 204 No Content response
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    
    return ESP_OK;
}

// Implementation of HTTP handler dispatched by each numbered handler
static esp_err_t httpserver_http_handler_impl(httpd_req_t *req) {
    // Handle OPTIONS preflight requests for CORS
    if (req->method == HTTP_OPTIONS) {
        return httpserver_options_handler(req);
    }
    
    // Get handler ID from user context
    int handler_id = (int)(intptr_t)req->user_ctx;

    ESP_LOGI(TAG, "HTTP handler called: URI=%s, method=%d, handler_id=%d", 
             req->uri, req->method, handler_id);
    
    if (handler_id < 0 || handler_id >= HTTP_HANDLER_MAX || !http_handlers[handler_id].active) {
        ESP_LOGE(TAG, "Invalid or inactive handler ID: %d", handler_id);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid handler");
        return ESP_FAIL;
    }

    // Get the MicroPython function
    mp_obj_t func = http_handlers[handler_id].func;

    if (func == MP_OBJ_NULL) {
        ESP_LOGE(TAG, "MicroPython function is NULL for handler %d", handler_id);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Function error");
        return ESP_FAIL;
    }

    // Queue the request for MicroPython processing
    if (httpserver_queue_web_request(handler_id, req, func)) {
        ESP_LOGI(TAG, "Request queued successfully for async processing");
        // Note: We don't send a response here - that will be done asynchronously
        // The async copy will be completed in httpserver_process_web_request
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to queue request");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to queue message");
        return ESP_FAIL;
    }
}

// ------------------------------------------------------------------------
// Module definition
// ------------------------------------------------------------------------

// Check if HTTPS server is running
static mp_obj_t httpserver_https_running(void) {
    return mp_obj_new_bool(https_server != NULL);
}
static MP_DEFINE_CONST_FUN_OBJ_0(httpserver_https_running_obj, httpserver_https_running);

// Module globals table
static const mp_rom_map_elem_t httpserver_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_httpserver) },
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&httpserver_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_on), MP_ROM_PTR(&httpserver_on_obj) },
    { MP_ROM_QSTR(MP_QSTR_off), MP_ROM_PTR(&httpserver_off_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&httpserver_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&httpserver_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_process_queue), MP_ROM_PTR(&httpserver_process_queue_obj) },
    { MP_ROM_QSTR(MP_QSTR_https_running), MP_ROM_PTR(&httpserver_https_running_obj) },
};

// Module dict
static MP_DEFINE_CONST_DICT(httpserver_module_globals, httpserver_module_globals_table);

// Register the module
const mp_obj_module_t httpserver_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&httpserver_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_httpserver, httpserver_module);
