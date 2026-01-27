/*
 * modwsserver.c - WebSocket server module for MicroPython using ESP-IDF HTTP server
 *
 * Generic WebSocket transport layer with:
 * - Connection management with ping/pong keep-alive
 * - Activity tracking with automatic disconnect
 * - C callbacks for high-performance protocol implementations
 * - Pre-queue filter callbacks for urgent message handling
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
#include "esp_timer.h"
#include "esp_event.h"

// Socket-related includes
#include <sys/socket.h>
#include <netinet/in.h>

// MicroPython includes
#include "py/runtime.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/mphal.h"

// External functions from httpserver module
extern httpd_handle_t httpserver_get_handle(void);
extern httpd_handle_t httpserver_get_https_handle(void);
extern bool httpserver_queue_message(int msg_type, int client_id, 
                                     const void *data, size_t data_len, void *user_data);

// Logger tag
static const char *TAG = "WSSERVER";

// Max number of concurrent clients
#define MAX_WS_CLIENTS 5

// Event types
#define WSSERVER_EVENT_CONNECT    0
#define WSSERVER_EVENT_DISCONNECT 1
#define WSSERVER_EVENT_MESSAGE    2

// Message types for queue - match those in modhttpserver.c
#define HTTP_MSG_WEBSOCKET 1

// Client tracking structure
typedef struct {
    int sockfd;
    bool active;
    int64_t last_activity;  // Timestamp of any client activity (microseconds)
    httpd_handle_t server_handle;  // Which server this client is connected to (HTTP or HTTPS)
} ws_client_t;

// Callback structure for MicroPython callbacks
typedef struct {
    bool active;
    mp_obj_t func;  // MicroPython function object
} wsserver_callback_t;

// C callback function types
typedef void (*wsserver_connect_cb_t)(int client_id);
typedef void (*wsserver_disconnect_cb_t)(int client_id);
typedef void (*wsserver_message_cb_t)(int client_id, const uint8_t *data, size_t len, bool is_binary);
// Pre-queue filter: return true to process normally, false to skip queuing
typedef bool (*wsserver_prequeue_filter_cb_t)(int client_id, const uint8_t *data, size_t len, bool is_binary);

// C callback structure
typedef struct {
    bool active;
    union {
        wsserver_connect_cb_t connect;
        wsserver_disconnect_cb_t disconnect;
        wsserver_message_cb_t message;
        wsserver_prequeue_filter_cb_t prequeue_filter;
    } func;
} wsserver_c_callback_t;

// Forward declarations
static void init_clients(void);
static int find_free_client_slot(void);
static int add_client(int sockfd, httpd_handle_t server_handle);
static int find_client_by_fd(int sockfd);
static void remove_client(int slot);
static bool is_client_valid(int client_id);
static void handle_ws_message(int client_id, const char *message, size_t len, bool is_binary);
static void handle_client_disconnect(int client_slot);
static void call_mp_ws_callback(int client_id, const char *event_name, const char *payload, size_t payload_len);
static esp_err_t ws_handler(httpd_req_t *req);
static void ws_ping_timer_callback(void* arg);

// Globals
static httpd_handle_t ws_server = NULL;
static bool wsserver_running = false;
static uint32_t ping_interval_s;  // Ping interval in seconds
static uint32_t ping_timeout_s;   // Activity timeout in seconds
static esp_timer_handle_t ping_timer = NULL;

// Client status and context
static ws_client_t ws_clients[MAX_WS_CLIENTS] = {0};

// Storage for MicroPython callback functions
static wsserver_callback_t wsserver_callbacks[3]; // CONNECT, DISCONNECT, MESSAGE

// Storage for C callback functions (used by webrepl and other C modules)
// Event types - need to add PREQUEUE_FILTER
#define WSSERVER_EVENT_PREQUEUE_FILTER 3

static wsserver_c_callback_t wsserver_c_callbacks[4]; // CONNECT, DISCONNECT, MESSAGE, PREQUEUE_FILTER

// ------------------------------------------------------------------------
// Client Management Functions
// ------------------------------------------------------------------------

static void init_clients(void) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        ws_clients[i].active = false;
        ws_clients[i].sockfd = -1;
        ws_clients[i].last_activity = 0;
        ws_clients[i].server_handle = NULL;
    }
}

static int find_free_client_slot(void) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (!ws_clients[i].active) {
            return i;
        }
    }
    return -1; // No free slots
}

static int add_client(int sockfd, httpd_handle_t server_handle) {
    int slot = find_free_client_slot();
    if (slot >= 0) {
        ws_clients[slot].sockfd = sockfd;
        ws_clients[slot].active = true;
        ws_clients[slot].last_activity = esp_timer_get_time() / 1000;  // Store in milliseconds
        ws_clients[slot].server_handle = server_handle;
        ESP_LOGI(TAG, "Added client %d with socket %d on server %p", slot, sockfd, server_handle);
        return slot;
    }
    ESP_LOGE(TAG, "No free client slots available");
    return -1;
}

static int find_client_by_fd(int sockfd) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_clients[i].active && ws_clients[i].sockfd == sockfd) {
            return i;
        }
    }
    return -1;
}

static void remove_client(int slot) __attribute__((unused));
static void remove_client(int slot) {
    if (slot >= 0 && slot < MAX_WS_CLIENTS) {
        ESP_LOGI(TAG, "Removing client %d with socket %d", slot, ws_clients[slot].sockfd);
        ws_clients[slot].active = false;
        ws_clients[slot].sockfd = -1;
    }
}

// Check if a client is valid and connected
static bool is_client_valid(int client_id) {
    // Check for valid client ID range
    if (client_id < 0 || client_id >= MAX_WS_CLIENTS) {
        return false;
    }
    
    // Check if client is active and has a valid socket
    if (!ws_clients[client_id].active || ws_clients[client_id].sockfd < 0) {
        return false;
    }
    
    return true;
}

// ------------------------------------------------------------------------
// Callback and Message Processing
// ------------------------------------------------------------------------

// Call a MicroPython callback function
// CONTEXT: Main MicroPython Task
void call_mp_ws_callback(int client_id, const char *event_name, const char *payload, size_t payload_len) {
    if (client_id < 0 || client_id >= MAX_WS_CLIENTS || !event_name) {
        ESP_LOGE(TAG, "Invalid parameters in call_mp_ws_callback");
        return;
    }
    
    ESP_LOGD(TAG, "Calling MicroPython callback for event '%s', client %d", event_name, client_id);
    
    // Map event name to event type
    int event_type = -1;
    if (strcmp(event_name, "connect") == 0) {
        event_type = WSSERVER_EVENT_CONNECT;
    } else if (strcmp(event_name, "disconnect") == 0) {
        event_type = WSSERVER_EVENT_DISCONNECT;
    } else if (strcmp(event_name, "message") == 0) {
        event_type = WSSERVER_EVENT_MESSAGE;
    } else {
        ESP_LOGE(TAG, "Unknown event type: %s", event_name);
        return;
    }
    
    // Check if we have a registered callback for this event type
    if (!wsserver_callbacks[event_type].active) {
        // This is expected when using C callbacks (like WebREPL) - log at DEBUG level
        ESP_LOGD(TAG, "No MicroPython callback registered for event type %d (%s) - using C callback instead", event_type, event_name);
        return;
    }
    
    mp_obj_t callback_func = wsserver_callbacks[event_type].func;
    
    if (!mp_obj_is_callable(callback_func)) {
        ESP_LOGE(TAG, "Callback for event '%s' is not callable", event_name);
        return;
    }
    
    // Prepare arguments for callback
    mp_obj_t args[3];
    int arg_count = 2;  // Default: client_id and event_name
    
    args[0] = mp_obj_new_int(client_id);
    args[1] = mp_obj_new_str(event_name, strlen(event_name));
    
    // Add payload for message events
    if (payload && payload_len > 0) {
        args[2] = mp_obj_new_str(payload, payload_len);
        arg_count = 3;
    }
    
    // Call the callback with error handling
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_call_function_n_kw(callback_func, arg_count, 0, args);
        nlr_pop();
        ESP_LOGD(TAG, "Callback executed successfully for event '%s'", event_name);
    } else {
        // Handle exception
        mp_obj_t exc = MP_OBJ_FROM_PTR(nlr.ret_val);
        mp_printf(&mp_plat_print, "[WSSERVER] ERROR: Exception in callback for '%s':\n", event_name);
        mp_obj_print_exception(&mp_plat_print, exc);
    }
}

// Process WebSocket message in main task context
// CONTEXT: Main MicroPython Task
// Called from httpserver.process_queue() when HTTP_MSG_WEBSOCKET is dequeued
void wsserver_process_websocket_msg(int client_id, const char *data, size_t data_len, bool is_binary) {
    if (!data) {
        // This is either a connect or disconnect event (no data)
        // Determine event type based on client state
        if (is_client_valid(client_id)) {
            // Client exists and is active - this is a connect event
            ESP_LOGI(TAG, "Processing WebSocket connect event in main task: client=%d", client_id);
            
            // Call C callback first (if registered)
            if (wsserver_c_callbacks[WSSERVER_EVENT_CONNECT].active) {
                wsserver_c_callbacks[WSSERVER_EVENT_CONNECT].func.connect(client_id);
            }
            // Then call Python callback (if registered)
            call_mp_ws_callback(client_id, "connect", NULL, 0);
        } else {
            // Client no longer active - this is a disconnect event
            ESP_LOGI(TAG, "Processing WebSocket disconnect event in main task: client=%d", client_id);
            
            // Call C callback first (if registered)
            if (wsserver_c_callbacks[WSSERVER_EVENT_DISCONNECT].active) {
                wsserver_c_callbacks[WSSERVER_EVENT_DISCONNECT].func.disconnect(client_id);
            }
            // Then call Python callback (if registered)
            call_mp_ws_callback(client_id, "disconnect", NULL, 0);
            
            // Ensure cleanup
            if (client_id >= 0 && client_id < MAX_WS_CLIENTS) {
                ws_clients[client_id].sockfd = -1;
                ws_clients[client_id].active = false;
            }
        }
    } else {
        // Normal message event with data
        ESP_LOGD(TAG, "Processing WebSocket message in main task: client=%d, len=%d, is_binary=%d", 
                client_id, (int)data_len, is_binary);
        
        // Call C callback first (if registered)
        ESP_LOGD(TAG, "C callback check: active=%d, func=%p", 
                wsserver_c_callbacks[WSSERVER_EVENT_MESSAGE].active,
                wsserver_c_callbacks[WSSERVER_EVENT_MESSAGE].func.message);
        
        if (wsserver_c_callbacks[WSSERVER_EVENT_MESSAGE].active) {
            ESP_LOGD(TAG, "Calling C callback for message (is_binary=%d)", is_binary);
            // Pass the actual is_binary flag
            wsserver_c_callbacks[WSSERVER_EVENT_MESSAGE].func.message(client_id, (const uint8_t *)data, data_len, is_binary);
        } else {
            ESP_LOGW(TAG, "No C callback registered for MESSAGE event");
        }
        // Then call Python callback (if registered)
        call_mp_ws_callback(client_id, "message", data, data_len);
    }
}

// Handle a WebSocket message from a client
// CONTEXT: ESP-IDF HTTP Server Task
static void handle_ws_message(int client_id, const char *message, size_t len, bool is_binary) {
    if (!message || len == 0) {
        ESP_LOGE(TAG, "Received empty message from client %d", client_id);
        return;
    }
    
    // Update activity timestamp
    if (client_id >= 0 && client_id < MAX_WS_CLIENTS && ws_clients[client_id].active) {
        ws_clients[client_id].last_activity = esp_timer_get_time() / 1000;  // Store in milliseconds
    }
    
    // Call pre-queue filter if registered (runs in HTTP thread)
    // This allows protocols to handle urgent messages (like Ctrl+C) before queuing
    // Return value: true = continue with normal queuing, false = message was handled, skip queue
    if (wsserver_c_callbacks[WSSERVER_EVENT_PREQUEUE_FILTER].active) {
        bool should_queue = wsserver_c_callbacks[WSSERVER_EVENT_PREQUEUE_FILTER].func.prequeue_filter(
            client_id, (const uint8_t *)message, len, is_binary
        );
        if (!should_queue) {
            ESP_LOGD(TAG, "Pre-queue filter handled message, skipping queue");
            return;  // Message was handled by filter, don't queue it
        }
    }
    
    ESP_LOGD(TAG, "Queueing %s WebSocket message from client %d (%d bytes)", 
             is_binary ? "BINARY" : "TEXT", client_id, (int)len);
    
    // Store is_binary flag in the queue message
    // We'll use the 'extra' field to pass the is_binary flag
    char *extra_data = malloc(1);
    if (extra_data) {
        *extra_data = is_binary ? 1 : 0;
    }
    
    // Queue the message for processing in main task
    if (!httpserver_queue_message(HTTP_MSG_WEBSOCKET, client_id, message, len, extra_data)) {
        ESP_LOGE(TAG, "Failed to queue WebSocket message");
        if (extra_data) free(extra_data);
    }
}

// Handle client disconnection
// CONTEXT: ESP-IDF HTTP Server Task or disconnect callback
static void handle_client_disconnect(int client_slot) {
    if (!is_client_valid(client_slot)) {
        return;
    }
    
    ESP_LOGI(TAG, "Client %d disconnected", client_slot);
    
    // Mark client as inactive immediately
    ws_clients[client_slot].active = false;
    int sockfd = ws_clients[client_slot].sockfd;
    ws_clients[client_slot].sockfd = -1;
    
    // Call C disconnect callback IMMEDIATELY (don't queue)
    // This ensures cleanup happens even if the queue is blocked by long-running scripts
    if (wsserver_c_callbacks[WSSERVER_EVENT_DISCONNECT].active) {
        ESP_LOGI(TAG, "Calling C disconnect callback directly from HTTP thread for client %d", client_slot);
        wsserver_c_callbacks[WSSERVER_EVENT_DISCONNECT].func.disconnect(client_slot);
    }
    
    // Queue disconnect event for Python callback (runs in MicroPython task context)
    // This allows Python callbacks to safely access MicroPython objects
    // Pass NULL data to indicate this is a disconnect event
    if (wsserver_callbacks[WSSERVER_EVENT_DISCONNECT].active) {
        ESP_LOGI(TAG, "Queueing disconnect event for client %d (Python callback)", client_slot);
        if (!httpserver_queue_message(HTTP_MSG_WEBSOCKET, client_slot, NULL, 0, NULL)) {
            ESP_LOGE(TAG, "Failed to queue disconnect event");
        }
    }
    
    ESP_LOGI(TAG, "Client %d cleanup complete (socket %d closed)", client_slot, sockfd);
}

// ------------------------------------------------------------------------
// WebSocket Handler
// ------------------------------------------------------------------------

// WebSocket Event Handler
// CONTEXT: ESP-IDF HTTP Server Task
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        // Handshake handling
        ESP_LOGI(TAG, "WebSocket handshake received");
        int sockfd = httpd_req_to_sockfd(req);
        httpd_handle_t server_handle = req->handle;  // Store which server this client connected to
        int client_slot = add_client(sockfd, server_handle);
        
        if (client_slot >= 0) {
            ESP_LOGI(TAG, "Added client %d with socket %d", client_slot, sockfd);
            ESP_LOGI(TAG, "Client %d connected, socket fd: %d", client_slot, sockfd);
            
            // Call C connect callback IMMEDIATELY (don't queue)
            // This ensures the client is initialized promptly, even if queue is blocked
            if (wsserver_c_callbacks[WSSERVER_EVENT_CONNECT].active) {
                ESP_LOGI(TAG, "Calling C connect callback directly from HTTP thread for client %d", client_slot);
                wsserver_c_callbacks[WSSERVER_EVENT_CONNECT].func.connect(client_slot);
            }
            
            // Queue connect event for Python callback (runs in MicroPython task context)
            // This allows Python callbacks to safely access MicroPython objects
            // Pass NULL data to indicate this is a connect event
            if (wsserver_callbacks[WSSERVER_EVENT_CONNECT].active) {
                ESP_LOGI(TAG, "Queueing connect event for client %d (Python callback)", client_slot);
                if (!httpserver_queue_message(HTTP_MSG_WEBSOCKET, client_slot, NULL, 0, NULL)) {
                    ESP_LOGE(TAG, "Failed to queue connect event");
                }
            }
        }
        
        return ESP_OK;
    }
    
    // Handle WebSocket frames
    ESP_LOGD(TAG, "WebSocket frame received");
    int sockfd = httpd_req_to_sockfd(req);
    int client_slot = find_client_by_fd(sockfd);
    
    if (client_slot < 0) {
        ESP_LOGE(TAG, "Received frame from unknown client (socket: %d)", sockfd);
        return ESP_FAIL;
    }
    
    // Update activity time for ANY frame
    ws_clients[client_slot].last_activity = esp_timer_get_time() / 1000;  // Store in milliseconds
    
    // Get frame information
    httpd_ws_frame_t ws_pkt = {0};
    uint8_t *buf = NULL;
    
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;  // Default type
    
    // Get the frame length
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
        return ret;
    }
    
    ESP_LOGD(TAG, "Frame len is %d, type is %d", ws_pkt.len, ws_pkt.type);
    
    // Handle control frames immediately
    if (ws_pkt.type == HTTPD_WS_TYPE_PONG) {
        ESP_LOGD(TAG, "Received PONG from client %d", client_slot);
        return ESP_OK;
    }
    
    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "Received CLOSE from client %d", client_slot);
        handle_client_disconnect(client_slot);
        return ESP_OK;
    }
    
    if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
        ESP_LOGI(TAG, "Received PING from client %d", client_slot);
        httpd_ws_frame_t pong = {0};
        pong.type = HTTPD_WS_TYPE_PONG;
        pong.len = 0;
        ret = httpd_ws_send_frame(req, &pong);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send PONG: %d", ret);
        }
        return ESP_OK;
    }
    
    // Allocate memory for the message payload
    if (ws_pkt.len) {
        buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for WS message");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        
        // Receive the actual message
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            return ret;
        }
        
        // Ensure null-termination for text frames
        if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
            buf[ws_pkt.len] = 0;
        }
    } else {
        // Empty message, nothing to process
        ESP_LOGD(TAG, "Received empty WebSocket frame, ignoring");
        return ESP_OK;
    }
    
    // Process message based on type
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        ESP_LOGI(TAG, "Rx TEXT msg: '%s'", buf);
        handle_ws_message(client_slot, (char*)buf, ws_pkt.len, false);
    } else if (ws_pkt.type == HTTPD_WS_TYPE_BINARY) {
        ESP_LOGI(TAG, "Rx BINARY msg, len=%d", ws_pkt.len);
        handle_ws_message(client_slot, (char*)buf, ws_pkt.len, true);
    }
    
    free(buf);
    
    return ESP_OK;
}

// ------------------------------------------------------------------------
// Ping/Pong Timer
// ------------------------------------------------------------------------

// Timer callback for pinging clients
static void ws_ping_timer_callback(void* arg) {
    if (!wsserver_running || !ws_server || ping_interval_s == 0) {
        return;
    }
    
    int64_t now = esp_timer_get_time() / 1000;  // Convert to milliseconds
    
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_clients[i].active) {
            // Check for inactivity timeout if enabled
            if (ping_timeout_s > 0) {
                int64_t inactivity_s = (now - ws_clients[i].last_activity) / 1000;  // Convert ms to seconds
                
                if (inactivity_s > ping_timeout_s) {
                    ESP_LOGI(TAG, "Client %d timed out (inactive for %d seconds)", 
                             i, (int)inactivity_s);
                    handle_client_disconnect(i);
                    continue;
                }
            }
            
            // Send ping using the correct server handle for this client
            httpd_ws_frame_t ping = {0};
            ping.type = HTTPD_WS_TYPE_PING;
            
            httpd_handle_t server_handle = ws_clients[i].server_handle;
            if (!server_handle) {
                ESP_LOGE(TAG, "Client %d has no server handle, disconnecting", i);
                handle_client_disconnect(i);
                continue;
            }
            
            ESP_LOGD(TAG, "Sending PING to client %d", i);
            // Use synchronous send for ping (no payload, blocking OK in timer callback)
            esp_err_t ret = httpd_ws_send_frame_async(server_handle, ws_clients[i].sockfd, &ping);
            
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to send PING to client %d: %d", i, ret);
                handle_client_disconnect(i);
            }
        }
    }
}

// ------------------------------------------------------------------------
// C API Functions (for use by other C modules like webrepl)
// ------------------------------------------------------------------------

// Callback to free payload after async send completes
static void wsserver_send_complete_cb(esp_err_t err, int sockfd, void *arg) {
    uint8_t *payload = (uint8_t *)arg;
    if (payload) {
        free(payload);
    }
}

// Register C callback for WebSocket events
// This allows other C modules (like webrepl) to hook into wsserver events directly
// without going through Python callbacks (faster!)
bool wsserver_register_c_callback(int event_type, void *callback_func) {
    if (event_type < 0 || event_type >= 4 || !callback_func) {
        ESP_LOGE(TAG, "Invalid event type %d or callback function %p", event_type, callback_func);
        return false;
    }
    
    wsserver_c_callbacks[event_type].active = true;
    
    // Store the function pointer based on event type
    switch (event_type) {
        case WSSERVER_EVENT_CONNECT:
            wsserver_c_callbacks[event_type].func.connect = (wsserver_connect_cb_t)callback_func;
            break;
        case WSSERVER_EVENT_DISCONNECT:
            wsserver_c_callbacks[event_type].func.disconnect = (wsserver_disconnect_cb_t)callback_func;
            break;
        case WSSERVER_EVENT_MESSAGE:
            wsserver_c_callbacks[event_type].func.message = (wsserver_message_cb_t)callback_func;
            break;
        case WSSERVER_EVENT_PREQUEUE_FILTER:
            wsserver_c_callbacks[event_type].func.prequeue_filter = (wsserver_prequeue_filter_cb_t)callback_func;
            break;
        default:
            ESP_LOGE(TAG, "Unknown event type %d in switch", event_type);
            return false;
    }
    
    ESP_LOGI(TAG, "Registered C callback for event type %d", event_type);
    return true;
}

// Get wsserver handle (for sending messages directly)
httpd_handle_t wsserver_get_handle(void) {
    return ws_server;
}

// Get client sockfd by client_id (for sending messages directly)
int wsserver_get_client_sockfd(int client_id) {
    if (client_id < 0 || client_id >= MAX_WS_CLIENTS || !ws_clients[client_id].active) {
        return -1;
    }
    return ws_clients[client_id].sockfd;
}

// Send data to a client (C API version)
bool wsserver_send_to_client(int client_id, const uint8_t *data, size_t len, bool is_binary) {
    if (!wsserver_running) {
        return false;
    }
    
    if (client_id < 0 || client_id >= MAX_WS_CLIENTS || !ws_clients[client_id].active) {
        return false;
    }
    
    // Use the server handle that this client is connected to (HTTP or HTTPS)
    httpd_handle_t server_handle = ws_clients[client_id].server_handle;
    if (!server_handle) {
        ESP_LOGE(TAG, "Client %d has no server handle", client_id);
        return false;
    }
    
    // CRITICAL: Copy data before queuing (httpd_ws_send_data_async doesn't copy payload!)
    // This function is called from drain task, so we must queue to HTTP server task
    uint8_t *data_copy = (uint8_t *)malloc(len);
    if (data_copy == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for data copy");
        return false;
    }
    memcpy(data_copy, data, len);
    
    httpd_ws_frame_t ws_pkt = {0};
    ws_pkt.payload = data_copy;  // Use copy, not original
    ws_pkt.len = len;
    ws_pkt.type = is_binary ? HTTPD_WS_TYPE_BINARY : HTTPD_WS_TYPE_TEXT;
    ws_pkt.final = true;
    ws_pkt.fragmented = false;
    
    // Use async send with callback to free payload after send completes
    // This queues the send to HTTP server task, ensuring thread safety
    esp_err_t ret = httpd_ws_send_data_async(server_handle, ws_clients[client_id].sockfd, &ws_pkt,
                                             wsserver_send_complete_cb, data_copy);
    
    if (ret != ESP_OK) {
        free(data_copy);  // Free on error (callback won't be called)
        ESP_LOGE(TAG, "Failed to send to client %d: error %d (0x%x)", client_id, ret, ret);
        
        // Check for fatal errors that indicate dead connection
        // ESP_FAIL (-1/0xffffffff): Generic failure (connection dead)
        // ESP_ERR_HTTPD_INVALID_REQ (0x8011): Invalid socket/request
        // ESP_ERR_HTTPD_RESP_SEND (0x8007): Send failed (connection broken)
        if (ret == ESP_FAIL || ret == ESP_ERR_HTTPD_INVALID_REQ || ret == ESP_ERR_HTTPD_RESP_SEND) {
            ESP_LOGW(TAG, "Fatal send error to client %d, cleaning up connection", client_id);
            handle_client_disconnect(client_id);
        }
        
        return false;
    }
    
    return true;
}

// C API: Check if wsserver is running
bool wsserver_is_running(void) {
    return wsserver_running;
}

// C API: Start the WebSocket server
// Returns: true on success, false on failure
bool wsserver_start_c(const char *path, int ping_interval, int ping_timeout) {
    if (wsserver_running) {
        ESP_LOGI(TAG, "WebSocket server already running");
        return true;
    }
    
    if (!path) {
        ESP_LOGE(TAG, "Path cannot be NULL");
        return false;
    }
    
    ESP_LOGI(TAG, "Starting WebSocket server on path '%s'", path);
    
    // Try to get existing HTTP server handle from httpserver module
    bool use_existing_handle = false;
    ws_server = httpserver_get_handle();
    if (ws_server) {
        use_existing_handle = true;
        ESP_LOGI(TAG, "Using existing HTTP server handle: %p", ws_server);
    } else {
        ESP_LOGI(TAG, "No existing HTTP server found, will create a new one");
    }
    
    // Set ping parameters
    ping_interval_s = ping_interval;
    ping_timeout_s = ping_timeout;
    
    ESP_LOGI(TAG, "WebSocket ping configured: interval=%ds, timeout=%ds", 
             (int)ping_interval_s, (int)ping_timeout_s);
    
    // Initialize client array
    init_clients();
    
    // Handle HTTP server setup if needed
    if (!use_existing_handle) {
        ESP_LOGI(TAG, "No HTTP server provided, creating a new one");
        
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = 8080;
        config.max_open_sockets = MAX_WS_CLIENTS + 2;
        config.max_uri_handlers = 2;
        config.lru_purge_enable = true;
        config.recv_wait_timeout = 10;
        config.send_wait_timeout = 10;
        
        ESP_LOGI(TAG, "Starting new HTTP server on port %d", config.server_port);
        esp_err_t ret = httpd_start(&ws_server, &config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start server: %d (0x%x)", ret, ret);
            return false;
        }
    }
    
    // Register URI handler for WebSocket endpoint
    httpd_uri_t ws_uri = {
        .uri = path,
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
        ,
        .is_websocket = true,
        .handle_ws_control_frames = true,
        .supported_subprotocol = "webrepl.binary.v1"
#endif
    };
    
    ESP_LOGI(TAG, "Registering WebSocket handler for '%s'", path);
    esp_err_t ret = httpd_register_uri_handler(ws_server, &ws_uri);
    if (ret != ESP_OK && ret != ESP_ERR_HTTPD_HANDLER_EXISTS) {
        ESP_LOGE(TAG, "Failed to register URI handler on HTTP: %d (0x%x)", ret, ret);
        if (!use_existing_handle) {
            httpd_stop(ws_server);
            ws_server = NULL;
        }
        return false;
    }
    
    if (ret == ESP_ERR_HTTPD_HANDLER_EXISTS) {
        ESP_LOGW(TAG, "Handler already exists for '%s', continuing", path);
    }
    
    // Also register on HTTPS server if available
    httpd_handle_t https_server = httpserver_get_https_handle();
    if (https_server != NULL) {
        ESP_LOGI(TAG, "Registering WebSocket handler on HTTPS server %p for '%s'", https_server, path);
        ret = httpd_register_uri_handler(https_server, &ws_uri);
        if (ret != ESP_OK && ret != ESP_ERR_HTTPD_HANDLER_EXISTS) {
            ESP_LOGE(TAG, "Failed to register URI handler on HTTPS: %d (0x%x)", ret, ret);
            // Don't fail completely - HTTP handler is registered
        } else {
            ESP_LOGI(TAG, "WebSocket handler successfully registered on HTTPS server");
        }
    } else {
        ESP_LOGW(TAG, "No HTTPS server available - WSS connections will not work. Only WS (unencrypted) is available.");
    }
    
    // Start ping timer if interval > 0
    if (ping_interval_s > 0 && !ping_timer) {
        esp_timer_create_args_t timer_args = {
            .callback = ws_ping_timer_callback,
            .name = "ws_ping"
        };
        ret = esp_timer_create(&timer_args, &ping_timer);
        if (ret == ESP_OK) {
            ret = esp_timer_start_periodic(ping_timer, (uint64_t)ping_interval_s * 1000000ULL);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start ping timer: %d", ret);
                esp_timer_delete(ping_timer);
                ping_timer = NULL;
            } else {
                ESP_LOGI(TAG, "Ping timer started (interval=%ds)", (int)ping_interval_s);
            }
        } else {
            ESP_LOGE(TAG, "Failed to create ping timer: %d", ret);
        }
    }
    
    wsserver_running = true;
    ESP_LOGI(TAG, "WebSocket server started successfully on '%s'", path);
    return true;
}

// ------------------------------------------------------------------------
// MicroPython API Functions
// ------------------------------------------------------------------------

// wsserver.start(path, http_handle=None, ping_interval=5, ping_timeout=10)
static mp_obj_t wsserver_start(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_path, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_http_handle, MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_ping_interval, MP_ARG_INT, {.u_int = 5} },
        { MP_QSTR_ping_timeout, MP_ARG_INT, {.u_int = 10} },
    };
    
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    
    if (wsserver_running) {
        ESP_LOGI(TAG, "WebSocket server already running");
        return mp_obj_new_bool(true);
    }
    
    if (!mp_obj_is_str(args[0].u_obj)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Path must be a string"));
    }
    
    const char *path = mp_obj_str_get_str(args[0].u_obj);
    ESP_LOGI(TAG, "Starting WebSocket server on path '%s'", path);
    
    // Always try to get existing HTTP server handle from httpserver module first
    bool use_existing_handle = false;
    ws_server = httpserver_get_handle();
    if (ws_server) {
        use_existing_handle = true;
        ESP_LOGI(TAG, "Using existing HTTP server handle: %p", ws_server);
    } else {
        ESP_LOGI(TAG, "No existing HTTP server found, will create a new one");
    }
    
    // Get ping parameters from parsed arguments
    ping_interval_s = args[2].u_int;
    ping_timeout_s = args[3].u_int;
    
    ESP_LOGI(TAG, "WebSocket ping configured: interval=%ds, timeout=%ds", 
             (int)ping_interval_s, (int)ping_timeout_s);
    
    // Initialize client array
    init_clients();
    
    // Handle HTTP server setup
    if (!use_existing_handle) {
        ESP_LOGI(TAG, "No HTTP server provided, creating a new one");
        
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = 8080;
        config.max_open_sockets = MAX_WS_CLIENTS + 2;
        config.max_uri_handlers = 2;
        config.lru_purge_enable = true;
        config.recv_wait_timeout = 10;
        config.send_wait_timeout = 10;
        
        ESP_LOGI(TAG, "Starting new HTTP server on port %d", config.server_port);
        esp_err_t ret = httpd_start(&ws_server, &config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start server: %d (0x%x)", ret, ret);
            return mp_obj_new_bool(false);
        }
    }
    
    // Register URI handler for WebSocket endpoint
    httpd_uri_t ws_uri = {
        .uri = path,
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
        ,
        .is_websocket = true,
        .handle_ws_control_frames = true,  // Let ESP-IDF handle ping/pong/close automatically
        .supported_subprotocol = "webrepl.binary.v1"
#endif
    };
    
    ESP_LOGI(TAG, "Registering WebSocket handler for '%s'", path);
    esp_err_t ret = httpd_register_uri_handler(ws_server, &ws_uri);
    if (ret != ESP_OK && ret != ESP_ERR_HTTPD_HANDLER_EXISTS) {
        ESP_LOGE(TAG, "Failed to register URI handler on HTTP: %d (0x%x)", ret, ret);
        if (!use_existing_handle) {
            httpd_stop(ws_server);
        }
        return mp_obj_new_bool(false);
    }
    
    // Also register on HTTPS server if available
    httpd_handle_t https_server = httpserver_get_https_handle();
    if (https_server != NULL) {
        ESP_LOGI(TAG, "Registering WebSocket handler on HTTPS server %p for '%s'", https_server, path);
        ret = httpd_register_uri_handler(https_server, &ws_uri);
        if (ret != ESP_OK && ret != ESP_ERR_HTTPD_HANDLER_EXISTS) {
            ESP_LOGE(TAG, "Failed to register URI handler on HTTPS: %d (0x%x)", ret, ret);
            // Don't fail completely - HTTP handler is registered
        } else {
            ESP_LOGI(TAG, "WebSocket handler successfully registered on HTTPS server");
        }
    } else {
        ESP_LOGW(TAG, "No HTTPS server available - WSS connections will not work. Only WS (unencrypted) is available.");
    }
    
    // Set up ping timer if enabled
    if (ping_interval_s > 0) {
        ESP_LOGI(TAG, "Starting ping timer with interval %ds", (int)ping_interval_s);
        esp_timer_create_args_t timer_args = {
            .callback = ws_ping_timer_callback,
            .name = "ws_ping"
        };
        esp_timer_create(&timer_args, &ping_timer);
        esp_timer_start_periodic(ping_timer, ping_interval_s * 1000000ULL);
    }
    
    wsserver_running = true;
    ESP_LOGI(TAG, "WebSocket server started successfully");
    return mp_obj_new_bool(true);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(wsserver_start_obj, 1, wsserver_start);

// wsserver.on(event_type, callback)
static mp_obj_t wsserver_on(mp_obj_t event_type_obj, mp_obj_t callback_obj) {
    if (!mp_obj_is_int(event_type_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("Event type must be an integer"));
    }
    
    if (!mp_obj_is_callable(callback_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("Callback must be callable"));
    }
    
    int event_type = mp_obj_get_int(event_type_obj);
    
    if (event_type < 0 || event_type >= 3) {
        mp_raise_ValueError(MP_ERROR_TEXT("Invalid event type"));
    }
    
    // Store callback
    wsserver_callbacks[event_type].active = true;
    wsserver_callbacks[event_type].func = callback_obj;
    
    const char* event_name = 
        event_type == WSSERVER_EVENT_CONNECT ? "connect" :
        event_type == WSSERVER_EVENT_DISCONNECT ? "disconnect" : "message";
    
    ESP_LOGI(TAG, "Registered callback for event '%s' (%d)", event_name, event_type);
    
    return mp_obj_new_bool(true);
}
static MP_DEFINE_CONST_FUN_OBJ_2(wsserver_on_obj, wsserver_on);

// wsserver.send(client_id, data, is_binary=False)
static mp_obj_t wsserver_send(size_t n_args, const mp_obj_t *args) {
    if (n_args < 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("Missing arguments"));
    }
    
    if (!mp_obj_is_int(args[0])) {
        mp_raise_TypeError(MP_ERROR_TEXT("Client ID must be an integer"));
    }
    
    int client_slot = mp_obj_get_int(args[0]);
    
    if (!is_client_valid(client_slot)) {
        ESP_LOGE(TAG, "Invalid client ID: %d", client_slot);
        return mp_obj_new_bool(false);
    }
    
    // Get data
    const char *data = NULL;
    size_t len = 0;
    
    if (mp_obj_is_str_or_bytes(args[1])) {
        data = mp_obj_str_get_data(args[1], &len);
    } else {
        mp_raise_TypeError(MP_ERROR_TEXT("Data must be string or bytes"));
    }
    
    if (len == 0 || data == NULL) {
        ESP_LOGE(TAG, "Invalid data (empty or NULL)");
        return mp_obj_new_bool(false);
    }
    
    // Check for optional binary flag
    bool is_binary = false;
    if (n_args >= 3 && mp_obj_is_bool(args[2])) {
        is_binary = mp_obj_is_true(args[2]);
    }
    
    ESP_LOGI(TAG, "Sending %d bytes to client %d (type: %s)", 
            (int)len, client_slot, is_binary ? "binary" : "text");
    
    // Create data copy for async operation
    uint8_t *data_copy = (uint8_t *)malloc(len);
    if (data_copy == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for data copy");
        return mp_obj_new_bool(false);
    }
    
    memcpy(data_copy, data, len);
    
    httpd_ws_frame_t ws_pkt = {0};
    ws_pkt.payload = data_copy;
    ws_pkt.len = len;
    ws_pkt.type = is_binary ? HTTPD_WS_TYPE_BINARY : HTTPD_WS_TYPE_TEXT;
    ws_pkt.final = true;
    ws_pkt.fragmented = false;
    
    int sockfd = ws_clients[client_slot].sockfd;
    
    // Update activity timestamp
    ws_clients[client_slot].last_activity = esp_timer_get_time();
    
    // Send frame asynchronously with callback to free payload after send completes
    esp_err_t ret = httpd_ws_send_data_async(ws_server, sockfd, &ws_pkt, 
                                             wsserver_send_complete_cb, data_copy);
    
    if (ret != ESP_OK) {
        // Free on error (callback won't be called)
        free(data_copy);
        ESP_LOGE(TAG, "Failed to send message to client %d: %d (0x%x)", 
                client_slot, ret, ret);
        
        // Check for fatal errors
        if (ret == ESP_ERR_HTTPD_INVALID_REQ || ret == ESP_ERR_HTTPD_RESP_SEND) {
            ESP_LOGE(TAG, "Fatal error sending to client %d, removing client", client_slot);
            handle_client_disconnect(client_slot);
        }
        
        return mp_obj_new_bool(false);
    }
    
    // Don't free data_copy here - callback will free it after send completes
    
    ESP_LOGI(TAG, "Successfully sent message to client %d", client_slot);
    return mp_obj_new_bool(true);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(wsserver_send_obj, 2, 3, wsserver_send);

// wsserver.close(client_id)
static mp_obj_t wsserver_close(mp_obj_t client_id_obj) {
    if (!mp_obj_is_int(client_id_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("Client ID must be an integer"));
    }
    
    int client_slot = mp_obj_get_int(client_id_obj);
    
    ESP_LOGI(TAG, "Closing client %d", client_slot);
    
    if (!is_client_valid(client_slot)) {
        ESP_LOGE(TAG, "Invalid client ID: %d", client_slot);
        return mp_obj_new_bool(false);
    }
    
    // Send close frame
    httpd_ws_frame_t ws_pkt = {0};
    ws_pkt.type = HTTPD_WS_TYPE_CLOSE;
    ws_pkt.len = 0;
    
    int sockfd = ws_clients[client_slot].sockfd;
    // Use synchronous send for close (no payload, blocking OK during cleanup)
    esp_err_t ret = httpd_ws_send_frame_async(ws_server, sockfd, &ws_pkt);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send close frame: %d", ret);
    }
    
    handle_client_disconnect(client_slot);
    
    return mp_obj_new_bool(ret == ESP_OK);
}
static MP_DEFINE_CONST_FUN_OBJ_1(wsserver_close_obj, wsserver_close);

// wsserver.stop()
static mp_obj_t wsserver_stop(void) {
    if (!wsserver_running) {
        ESP_LOGI(TAG, "WebSocket server not running");
        return mp_obj_new_bool(true);
    }
    
    ESP_LOGI(TAG, "Stopping WebSocket server");
    
    // Clear callbacks
    for (int i = 0; i < 3; i++) {
        wsserver_callbacks[i].active = false;
        wsserver_callbacks[i].func = MP_OBJ_NULL;
    }
    
    // Stop ping timer
    if (ping_timer) {
        esp_timer_stop(ping_timer);
        esp_timer_delete(ping_timer);
        ping_timer = NULL;
    }
    
    // Close all client connections
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_clients[i].active) {
            httpd_ws_frame_t ws_pkt = {0};
            ws_pkt.type = HTTPD_WS_TYPE_CLOSE;
            ws_pkt.len = 0;
            
            ESP_LOGI(TAG, "Sending close frame to client %d (socket: %d)", 
                    i, ws_clients[i].sockfd);
            
            // Use synchronous send for close (no payload, blocking OK during cleanup)
            httpd_ws_send_frame_async(ws_server, ws_clients[i].sockfd, &ws_pkt);
            handle_client_disconnect(i);
        }
    }
    
    // Stop HTTP server (only if we created it)
    // Note: If using external HTTP server, don't stop it
    // For now, we'll keep the server running
    
    ws_server = NULL;
    wsserver_running = false;
    
    ESP_LOGI(TAG, "WebSocket server stopped");
    return mp_obj_new_bool(true);
}
static MP_DEFINE_CONST_FUN_OBJ_0(wsserver_stop_obj, wsserver_stop);

// wsserver.is_connected(client_id)
static mp_obj_t wsserver_is_connected(mp_obj_t client_id_obj) {
    if (!mp_obj_is_int(client_id_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("Client ID must be an integer"));
    }
    
    int client_slot = mp_obj_get_int(client_id_obj);
    bool valid = is_client_valid(client_slot);
    
    ESP_LOGI(TAG, "Checking if client %d is connected: %s", client_slot, valid ? "yes" : "no");
    
    return mp_obj_new_bool(valid);
}
static MP_DEFINE_CONST_FUN_OBJ_1(wsserver_is_connected_obj, wsserver_is_connected);

// wsserver.client_info(client_id)
static mp_obj_t wsserver_client_info(mp_obj_t client_id_obj) {
    if (!mp_obj_is_int(client_id_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("Client ID must be an integer"));
    }
    
    int client_slot = mp_obj_get_int(client_id_obj);
    
    if (!is_client_valid(client_slot)) {
        return mp_const_none;
    }
    
    // Calculate time since last activity
    int64_t now = esp_timer_get_time();
    int64_t inactive_time_ms = (now - ws_clients[client_slot].last_activity) / 1000;
    
    // Create dictionary with client info
    mp_obj_t dict = mp_obj_new_dict(3);
    
    mp_obj_dict_store(dict, 
                     mp_obj_new_str("socket", 6),
                     mp_obj_new_int(ws_clients[client_slot].sockfd));
    
    mp_obj_dict_store(dict,
                     mp_obj_new_str("last_activity", 13),
                     mp_obj_new_int((int)(ws_clients[client_slot].last_activity / 1000000)));
    
    mp_obj_dict_store(dict,
                     mp_obj_new_str("inactive_ms", 11),
                     mp_obj_new_int((int)inactive_time_ms));
    
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_1(wsserver_client_info_obj, wsserver_client_info);

// wsserver.count_clients()
static mp_obj_t wsserver_count_clients(void) {
    int count = 0;
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_clients[i].active) {
            count++;
        }
    }
    
    return mp_obj_new_int(count);
}
static MP_DEFINE_CONST_FUN_OBJ_0(wsserver_count_clients_obj, wsserver_count_clients);

// ------------------------------------------------------------------------
// Module definition
// ------------------------------------------------------------------------

// Module globals table
static const mp_rom_map_elem_t wsserver_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_wsserver) },
    
    // Functions
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&wsserver_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_on), MP_ROM_PTR(&wsserver_on_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&wsserver_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&wsserver_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&wsserver_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_connected), MP_ROM_PTR(&wsserver_is_connected_obj) },
    { MP_ROM_QSTR(MP_QSTR_client_info), MP_ROM_PTR(&wsserver_client_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_count_clients), MP_ROM_PTR(&wsserver_count_clients_obj) },
    
    // Event type constants
    { MP_ROM_QSTR(MP_QSTR_CONNECT), MP_ROM_INT(WSSERVER_EVENT_CONNECT) },
    { MP_ROM_QSTR(MP_QSTR_DISCONNECT), MP_ROM_INT(WSSERVER_EVENT_DISCONNECT) },
    { MP_ROM_QSTR(MP_QSTR_MESSAGE), MP_ROM_INT(WSSERVER_EVENT_MESSAGE) },
    
    // Frame type constants
    { MP_ROM_QSTR(MP_QSTR_TEXT), MP_ROM_INT(HTTPD_WS_TYPE_TEXT) },
    { MP_ROM_QSTR(MP_QSTR_BINARY), MP_ROM_INT(HTTPD_WS_TYPE_BINARY) },
    
    // Max clients constant
    { MP_ROM_QSTR(MP_QSTR_MAX_CLIENTS), MP_ROM_INT(MAX_WS_CLIENTS) },
};

// Module dict
static MP_DEFINE_CONST_DICT(wsserver_module_globals, wsserver_module_globals_table);

// Register the module
const mp_obj_module_t wsserver_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&wsserver_module_globals,
};

// Register module with MicroPython
MP_REGISTER_MODULE(MP_QSTR_wsserver, wsserver_module);

// Module is registered as part of esp32 module (see modesp32.c)
