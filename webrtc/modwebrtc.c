/*
 * pyDirect WebRTC Module
 * 
 * Provides WebRTC PeerConnection with DataChannel support for MicroPython.
 * Designed for browser-to-ESP32 communication using WebRTC DataChannels.
 * 
 * Key Features:
 * - Data-channel-only mode (no audio/video overhead)
 * - HTTPS signaling via existing httpserver module
 * - Bidirectional messaging with browser
 * - Low-latency P2P communication
 * - NAT traversal with STUN/TURN support
 * 
 * Architecture:
 *   Browser (HTTPS) <--signaling--> ESP32 (HTTPS)
 *   Browser <--WebRTC DataChannel (P2P)--> ESP32
 * 
 * Usage:
 *   import webrtc
 *   peer = webrtc.Peer()
 *   peer.on_offer(lambda sdp: print("Received offer"))
 *   peer.on_data(lambda data: print("Received:", data))
 *   peer.send("Hello from ESP32!")
 * 
 * License: MIT
 */

#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "esp_log.h"

#include "py/runtime.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/objarray.h"
#include "py/mphal.h"  // For mp_hal_wake_main_task()
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>

// MicroPython 1.27 uses 'static' instead of 'STATIC' macro
#ifndef STATIC
#define STATIC static
#endif

static const char *TAG = "WEBRTC";

//=============================================================================
// Module State and Configuration
//=============================================================================

// Maximum data channel message size
#define WEBRTC_MAX_MESSAGE_SIZE 65535

// DataChannel cache sizes (tune for performance vs memory)
#define WEBRTC_SEND_CACHE_SIZE (100 * 1024)  // 100KB send buffer
#define WEBRTC_RECV_CACHE_SIZE (100 * 1024)  // 100KB receive buffer

// Event queue for async events (SDP, ICE candidates, data)
#define WEBRTC_EVENT_QUEUE_SIZE 10

typedef enum {
    WEBRTC_EVENT_SDP_OFFER,
    WEBRTC_EVENT_SDP_ANSWER,
    WEBRTC_EVENT_ICE_CANDIDATE,
    WEBRTC_EVENT_DATA_RECEIVED,
    WEBRTC_EVENT_STATE_CHANGE,
    WEBRTC_EVENT_ERROR
} webrtc_event_type_t;

typedef struct {
    webrtc_event_type_t type;
    union {
        struct {
            char *sdp;
            size_t len;
        } sdp_data;
        struct {
            char *candidate;
            size_t len;
        } ice_data;
        struct {
            uint8_t *data;
            size_t len;
            uint16_t stream_id;
        } data_msg;
        struct {
            esp_peer_state_t state;
        } state_data;
        struct {
            char *message;
        } error_data;
    };
} webrtc_event_t;

//=============================================================================
// WebRTC Peer Object
//=============================================================================

typedef struct {
    mp_obj_base_t base;
    
    // ESP Peer handle
    esp_peer_handle_t peer_handle;
    
    // State
    esp_peer_state_t state;
    bool connected;
    bool data_channel_open;
    
    // Callbacks (MicroPython callables)
    mp_obj_t on_sdp_offer_cb;
    mp_obj_t on_sdp_answer_cb;
    mp_obj_t on_ice_candidate_cb;
    mp_obj_t on_data_cb;
    mp_obj_t on_state_cb;
    mp_obj_t on_error_cb;
    
    // Event queue for async processing (processed via process_queue())
    QueueHandle_t event_queue;
    
    // Self reference for async process_queue() callback (stored as MP object)
    mp_obj_t self_obj;
    
    // Mutex for thread safety
    SemaphoreHandle_t mutex;
    
    // Background task for esp_peer_main_loop()
    TaskHandle_t process_task;
    bool process_task_running;
    
    // Configuration
    bool role_controlling;  // True = controlling (client), False = controlled (server)
    
} webrtc_peer_obj_t;

//=============================================================================
// Forward Declarations
//=============================================================================

static int webrtc_on_msg_callback(esp_peer_msg_t *msg, void *ctx);
static int webrtc_on_state_callback(esp_peer_state_t state, void *ctx);
static int webrtc_on_data_callback(esp_peer_data_frame_t *frame, void *ctx);
STATIC mp_obj_t webrtc_peer_process_queue(mp_obj_t self_in);  // Forward declaration

//=============================================================================
// Utility Functions
//=============================================================================

// Wrapper function to call process_queue() on a peer object (for async scheduling)
// This creates a bound method that can be scheduled via mp_sched_schedule()
STATIC mp_obj_t webrtc_process_queue_wrapper(mp_obj_t self_obj) {
    return webrtc_peer_process_queue(self_obj);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(webrtc_process_queue_wrapper_obj, webrtc_process_queue_wrapper);

// Background task to run esp_peer_main_loop() continuously
static void webrtc_process_task(void *pvParameters) {
    webrtc_peer_obj_t *self = (webrtc_peer_obj_t *)pvParameters;
    
    // Store values locally at start (safe even if self is freed later)
    esp_peer_handle_t handle = self->peer_handle;
    TaskHandle_t *task_handle_ptr = &self->process_task;
    bool *running_ptr = &self->process_task_running;
    
    while (*running_ptr && handle) {
        esp_peer_main_loop(handle);
        vTaskDelay(pdMS_TO_TICKS(10));  // ~100Hz
    }
    
    // Clear task handle
    *task_handle_ptr = NULL;
    vTaskDelete(NULL);
}

// Background task to close peer without blocking MicroPython
static void webrtc_close_task(void *pvParameters) {
    esp_peer_handle_t handle = (esp_peer_handle_t)pvParameters;
    if (handle) {
        esp_peer_close(handle);
    }
    vTaskDelete(NULL);
}

static void webrtc_lock(webrtc_peer_obj_t *self) {
    if (self->mutex) {
        xSemaphoreTake(self->mutex, portMAX_DELAY);
    }
}

static void webrtc_unlock(webrtc_peer_obj_t *self) {
    if (self->mutex) {
        xSemaphoreGive(self->mutex);
    }
}

static void webrtc_free_event(webrtc_event_t *event) {
    if (!event) return;
    
    switch (event->type) {
        case WEBRTC_EVENT_SDP_OFFER:
        case WEBRTC_EVENT_SDP_ANSWER:
            if (event->sdp_data.sdp) {
                free(event->sdp_data.sdp);
            }
            break;
        case WEBRTC_EVENT_ICE_CANDIDATE:
            if (event->ice_data.candidate) {
                free(event->ice_data.candidate);
            }
            break;
        case WEBRTC_EVENT_DATA_RECEIVED:
            if (event->data_msg.data) {
                free(event->data_msg.data);
            }
            break;
        case WEBRTC_EVENT_ERROR:
            if (event->error_data.message) {
                free(event->error_data.message);
            }
            break;
        default:
            break;
    }
}

//=============================================================================
// Event Processing (Called from MicroPython context via process_queue())
//=============================================================================

// Process a single event - MUST be called from MicroPython task context
static void webrtc_process_event(webrtc_peer_obj_t *self, webrtc_event_t *event) {
    // Invoke MicroPython callback based on event type
    switch (event->type) {
        case WEBRTC_EVENT_SDP_OFFER:
        case WEBRTC_EVENT_SDP_ANSWER: {
            mp_obj_t cb = (event->type == WEBRTC_EVENT_SDP_OFFER) ? 
                          self->on_sdp_offer_cb : self->on_sdp_answer_cb;
            
            if (cb != mp_const_none) {
                mp_obj_t sdp_str = mp_obj_new_str(event->sdp_data.sdp, event->sdp_data.len);
                mp_call_function_1(cb, sdp_str);
            }
            break;
        }
        
        case WEBRTC_EVENT_ICE_CANDIDATE: {
            if (self->on_ice_candidate_cb != mp_const_none) {
                mp_obj_t ice_str = mp_obj_new_str(event->ice_data.candidate, event->ice_data.len);
                mp_call_function_1(self->on_ice_candidate_cb, ice_str);
            }
            break;
        }
        
        case WEBRTC_EVENT_DATA_RECEIVED: {
            if (self->on_data_cb != mp_const_none) {
                mp_obj_t data_bytes = mp_obj_new_bytes(event->data_msg.data, event->data_msg.len);
                mp_call_function_1(self->on_data_cb, data_bytes);
            }
            break;
        }
        
        case WEBRTC_EVENT_STATE_CHANGE: {
            if (self->on_state_cb != mp_const_none) {
                mp_obj_t state_int = mp_obj_new_int(event->state_data.state);
                mp_call_function_1(self->on_state_cb, state_int);
            }
            break;
        }
        
        case WEBRTC_EVENT_ERROR: {
            if (self->on_error_cb != mp_const_none) {
                mp_obj_t err_str = mp_obj_new_str(event->error_data.message, strlen(event->error_data.message));
                mp_call_function_1(self->on_error_cb, err_str);
            }
            break;
        }
    }
    
    // Free event resources
    webrtc_free_event(event);
}

//=============================================================================
// ESP Peer Callbacks (C context)
//=============================================================================

static int webrtc_on_msg_callback(esp_peer_msg_t *msg, void *ctx) {
    webrtc_peer_obj_t *self = (webrtc_peer_obj_t *)ctx;
    
    if (!msg || !msg->data || msg->size == 0) {
        return 0;
    }
    
    webrtc_event_t event = {0};
    
    switch (msg->type) {
        case ESP_PEER_MSG_TYPE_SDP: {
            // Determine if offer or answer by parsing SDP
            bool is_offer = (strstr((const char *)msg->data, "a=sendrecv") != NULL || 
                           strstr((const char *)msg->data, "a=sendonly") != NULL);
            
            event.type = is_offer ? WEBRTC_EVENT_SDP_OFFER : WEBRTC_EVENT_SDP_ANSWER;
            event.sdp_data.sdp = malloc(msg->size + 1);
            if (event.sdp_data.sdp) {
                memcpy(event.sdp_data.sdp, msg->data, msg->size);
                event.sdp_data.sdp[msg->size] = '\0';
                event.sdp_data.len = msg->size;
                if (xQueueSend(self->event_queue, &event, 0) == pdTRUE) {
                    mp_sched_schedule(MP_OBJ_FROM_PTR(&webrtc_process_queue_wrapper_obj), self->self_obj);
                }
            }
            break;
        }
        
        case ESP_PEER_MSG_TYPE_CANDIDATE: {
            event.type = WEBRTC_EVENT_ICE_CANDIDATE;
            event.ice_data.candidate = malloc(msg->size + 1);
            if (event.ice_data.candidate) {
                memcpy(event.ice_data.candidate, msg->data, msg->size);
                event.ice_data.candidate[msg->size] = '\0';
                event.ice_data.len = msg->size;
                if (xQueueSend(self->event_queue, &event, 0) == pdTRUE) {
                    mp_sched_schedule(MP_OBJ_FROM_PTR(&webrtc_process_queue_wrapper_obj), self->self_obj);
                }
            }
            break;
        }
        
        default:
            ESP_LOGW(TAG, "Unknown message type: %d", msg->type);
            break;
    }
    
    return 0;
}

static int webrtc_on_state_callback(esp_peer_state_t state, void *ctx) {
    webrtc_peer_obj_t *self = (webrtc_peer_obj_t *)ctx;
    
    ESP_LOGD(TAG, "State changed: %d", state);
    
    webrtc_lock(self);
    self->state = state;
    
    // Update convenience flags
    self->connected = (state == ESP_PEER_STATE_CONNECTED);
    self->data_channel_open = (state == ESP_PEER_STATE_DATA_CHANNEL_OPENED);
    webrtc_unlock(self);
    
    // Queue state change event
    webrtc_event_t event = {
        .type = WEBRTC_EVENT_STATE_CHANGE,
        .state_data.state = state
    };
    if (xQueueSend(self->event_queue, &event, 0) == pdTRUE) {
        mp_sched_schedule(MP_OBJ_FROM_PTR(&webrtc_process_queue_wrapper_obj), self->self_obj);
    }
    
    return 0;
}

static int webrtc_on_data_callback(esp_peer_data_frame_t *frame, void *ctx) {
    webrtc_peer_obj_t *self = (webrtc_peer_obj_t *)ctx;
    
    if (!frame || !frame->data || frame->size == 0) {
        return 0;
    }
    
    ESP_LOGD(TAG, "Data received: %d bytes on stream %d (type=%s)", 
             frame->size, frame->stream_id, 
             frame->type == ESP_PEER_DATA_CHANNEL_STRING ? "STRING" : "DATA");
    
    // Queue data event
    webrtc_event_t event = {
        .type = WEBRTC_EVENT_DATA_RECEIVED,
        .data_msg.data = malloc(frame->size),
        .data_msg.len = frame->size,
        .data_msg.stream_id = frame->stream_id
    };
    
    if (event.data_msg.data) {
        memcpy(event.data_msg.data, frame->data, frame->size);
        if (xQueueSend(self->event_queue, &event, 0) == pdTRUE) {
            // Immediately schedule process_queue() to process the data asynchronously
            // If scheduler queue is full, data is still queued and will be processed
            // when process_queue() is called next
            mp_sched_schedule(MP_OBJ_FROM_PTR(&webrtc_process_queue_wrapper_obj), self->self_obj);
        }
    } else {
        ESP_LOGE(TAG, "Failed to allocate memory for received data");
    }
    
    return 0;
}

//=============================================================================
// MicroPython Class: webrtc.Peer
//=============================================================================

// Constructor: webrtc.Peer(role='controlling', stun_server=None, turn_server=None)
STATIC mp_obj_t webrtc_peer_make_new(const mp_obj_type_t *type, size_t n_args, 
                                      size_t n_kw, const mp_obj_t *args) {
    // Parse arguments
    enum { ARG_role, ARG_stun_server, ARG_turn_server };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_role, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_stun_server, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_turn_server, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    
    mp_arg_val_t parsed_args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed_args);
    
    // Allocate object
    webrtc_peer_obj_t *self = m_new_obj(webrtc_peer_obj_t);
    self->base.type = type;
    
    // Initialize state
    self->peer_handle = NULL;
    self->state = ESP_PEER_STATE_CLOSED;
    self->connected = false;
    self->data_channel_open = false;
    
    // Initialize callbacks to None
    self->on_sdp_offer_cb = mp_const_none;
    self->on_sdp_answer_cb = mp_const_none;
    self->on_ice_candidate_cb = mp_const_none;
    self->on_data_cb = mp_const_none;
    self->on_state_cb = mp_const_none;
    self->on_error_cb = mp_const_none;
    
    // Initialize background task
    self->process_task = NULL;
    self->process_task_running = false;
    
    // Parse role ('controlling' = client, 'controlled' = server)
    self->role_controlling = true;  // Default to controlling
    if (parsed_args[ARG_role].u_obj != mp_const_none) {
        const char *role_str = mp_obj_str_get_str(parsed_args[ARG_role].u_obj);
        if (strcmp(role_str, "controlled") == 0) {
            self->role_controlling = false;
        }
    }
    
    // Create mutex
    self->mutex = xSemaphoreCreateMutex();
    if (!self->mutex) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to create mutex"));
    }
    
    // Create event queue (events processed via process_queue() in MicroPython context)
    self->event_queue = xQueueCreate(WEBRTC_EVENT_QUEUE_SIZE, sizeof(webrtc_event_t));
    if (!self->event_queue) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to create event queue"));
    }
    
    // Store self reference for async process_queue() scheduling
    self->self_obj = MP_OBJ_FROM_PTR(self);
    
    // Configure ESP Peer (data-channel-only, no media)
    esp_peer_default_cfg_t default_cfg = {
        // For local network without STUN: reduce timeout to prevent long hangs on closure.
        // 5 seconds is a safe middle ground for local LAN.
        .agent_recv_timeout = 5000, 
        .data_ch_cfg = {
            .send_cache_size = WEBRTC_SEND_CACHE_SIZE,
            .recv_cache_size = WEBRTC_RECV_CACHE_SIZE,
        }
    };
    
    ESP_LOGI(TAG, "WebRTC configured for local network (Host candidates only)");
    
    esp_peer_cfg_t peer_cfg = {
        .server_num = 0, // No STUN/TURN servers needed for LAN-only use
        .role = self->role_controlling ? ESP_PEER_ROLE_CONTROLLING : ESP_PEER_ROLE_CONTROLLED,
        .ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL,
        .audio_dir = ESP_PEER_MEDIA_DIR_NONE,  // No audio
        .video_dir = ESP_PEER_MEDIA_DIR_NONE,  // No video
        .enable_data_channel = true,           // Data channel only!
        .ctx = self,
        .on_msg = webrtc_on_msg_callback,
        .on_state = webrtc_on_state_callback,
        .on_data = webrtc_on_data_callback,
        .extra_cfg = &default_cfg,
        .extra_size = sizeof(default_cfg)
    };
    
    // Open ESP Peer
    int err = esp_peer_open(&peer_cfg, esp_peer_get_default_impl(), &self->peer_handle);
    if (err != 0) {
        ESP_LOGE(TAG, "Failed to open ESP Peer: %d", err);
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to initialize WebRTC peer"));
    }
    
    // Start background task to run esp_peer_main_loop() continuously
    self->process_task_running = true;
    BaseType_t task_ret = xTaskCreate(webrtc_process_task, "webrtc_process", 8192, (void*)self, 5, &self->process_task);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create process task");
        esp_peer_close(self->peer_handle);
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to create WebRTC process task"));
    }
    
    ESP_LOGI(TAG, "WebRTC Peer created (role: %s)", self->role_controlling ? "controlling" : "controlled");
    
    return MP_OBJ_FROM_PTR(self);
}

// peer.on_offer(callback) - Set callback for SDP offers
STATIC mp_obj_t webrtc_peer_on_offer(mp_obj_t self_in, mp_obj_t callback) {
    webrtc_peer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    webrtc_lock(self);
    self->on_sdp_offer_cb = callback;
    webrtc_unlock(self);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(webrtc_peer_on_offer_obj, webrtc_peer_on_offer);

// peer.on_answer(callback) - Set callback for SDP answers
STATIC mp_obj_t webrtc_peer_on_answer(mp_obj_t self_in, mp_obj_t callback) {
    webrtc_peer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    webrtc_lock(self);
    self->on_sdp_answer_cb = callback;
    webrtc_unlock(self);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(webrtc_peer_on_answer_obj, webrtc_peer_on_answer);

// peer.on_ice(callback) - Set callback for ICE candidates
STATIC mp_obj_t webrtc_peer_on_ice(mp_obj_t self_in, mp_obj_t callback) {
    webrtc_peer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    webrtc_lock(self);
    self->on_ice_candidate_cb = callback;
    webrtc_unlock(self);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(webrtc_peer_on_ice_obj, webrtc_peer_on_ice);

// peer.on_data(callback) - Set callback for incoming data
STATIC mp_obj_t webrtc_peer_on_data(mp_obj_t self_in, mp_obj_t callback) {
    webrtc_peer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    webrtc_lock(self);
    self->on_data_cb = callback;
    webrtc_unlock(self);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(webrtc_peer_on_data_obj, webrtc_peer_on_data);

// peer.on_state(callback) - Set callback for state changes
STATIC mp_obj_t webrtc_peer_on_state(mp_obj_t self_in, mp_obj_t callback) {
    webrtc_peer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    webrtc_lock(self);
    self->on_state_cb = callback;
    webrtc_unlock(self);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(webrtc_peer_on_state_obj, webrtc_peer_on_state);

// peer.start_connection() - Start the connection process
//
// For CONTROLLING role: generates SDP offer (delivered via on_offer callback)
// For CONTROLLED role: prepares to accept remote SDP offer
//
// MUST be called before set_remote_sdp() for controlled role!
STATIC mp_obj_t webrtc_peer_start_connection(mp_obj_t self_in) {
    webrtc_peer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    if (!self->peer_handle) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Peer not initialized"));
    }
    
    int err = esp_peer_new_connection(self->peer_handle);
    if (err != 0) {
        ESP_LOGE(TAG, "Failed to start connection: %d", err);
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to start connection"));
    }
    
    ESP_LOGI(TAG, "Connection started (role: %s)", 
             self->role_controlling ? "controlling" : "controlled");
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(webrtc_peer_start_connection_obj, webrtc_peer_start_connection);

// peer.create_offer() - Alias for start_connection() (for controlling role)
STATIC mp_obj_t webrtc_peer_create_offer(mp_obj_t self_in) {
    return webrtc_peer_start_connection(self_in);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(webrtc_peer_create_offer_obj, webrtc_peer_create_offer);

// peer.set_remote_sdp(sdp_string) - Set remote SDP (offer or answer)
STATIC mp_obj_t webrtc_peer_set_remote_sdp(mp_obj_t self_in, mp_obj_t sdp_in) {
    webrtc_peer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    if (!self->peer_handle) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Peer not initialized"));
    }
    
    size_t sdp_len;
    const char *sdp = mp_obj_str_get_data(sdp_in, &sdp_len);
    
    esp_peer_msg_t msg = {
        .type = ESP_PEER_MSG_TYPE_SDP,
        .data = (uint8_t *)sdp,
        .size = sdp_len
    };
    
    int err = esp_peer_send_msg(self->peer_handle, &msg);
    if (err != 0) {
        ESP_LOGE(TAG, "Failed to set remote SDP: %d", err);
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to set remote SDP"));
    }
    
    ESP_LOGI(TAG, "Remote SDP set");
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(webrtc_peer_set_remote_sdp_obj, webrtc_peer_set_remote_sdp);

// peer.add_ice_candidate(candidate_string) - Add ICE candidate
STATIC mp_obj_t webrtc_peer_add_ice_candidate(mp_obj_t self_in, mp_obj_t candidate_in) {
    webrtc_peer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    if (!self->peer_handle) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Peer not initialized"));
    }
    
    size_t candidate_len;
    const char *candidate = mp_obj_str_get_data(candidate_in, &candidate_len);
    
    esp_peer_msg_t msg = {
        .type = ESP_PEER_MSG_TYPE_CANDIDATE,
        .data = (uint8_t *)candidate,
        .size = candidate_len
    };
    
    int err = esp_peer_send_msg(self->peer_handle, &msg);
    if (err != 0) {
        ESP_LOGE(TAG, "Failed to add ICE candidate: %d", err);
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to add ICE candidate"));
    }
    
    ESP_LOGI(TAG, "ICE candidate added");
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(webrtc_peer_add_ice_candidate_obj, webrtc_peer_add_ice_candidate);

// peer.send(data) - Send data via DataChannel
STATIC mp_obj_t webrtc_peer_send(mp_obj_t self_in, mp_obj_t data_in) {
    webrtc_peer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    if (!self->peer_handle) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Peer not initialized"));
    }
    
    if (!self->data_channel_open) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Data channel not open yet"));
    }
    
    // Get data (support both str and bytes)
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);
    
    if (bufinfo.len > WEBRTC_MAX_MESSAGE_SIZE) {
        mp_raise_ValueError(MP_ERROR_TEXT("Message too large"));
    }
    
    esp_peer_data_frame_t frame = {
        .type = ESP_PEER_DATA_CHANNEL_DATA,
        .stream_id = 0,
        .data = bufinfo.buf,
        .size = bufinfo.len
    };
    
    int err = esp_peer_send_data(self->peer_handle, &frame);
    if (err != 0) {
        ESP_LOGE(TAG, "Failed to send data: %d", err);
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to send data"));
    }
    
    ESP_LOGI(TAG, "Sent %d bytes via DataChannel (type=DATA)", bufinfo.len);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(webrtc_peer_send_obj, webrtc_peer_send);

// peer.is_connected() - Check if peer is connected
STATIC mp_obj_t webrtc_peer_is_connected(mp_obj_t self_in) {
    webrtc_peer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    webrtc_lock(self);
    bool connected = self->data_channel_open;
    webrtc_unlock(self);
    return mp_obj_new_bool(connected);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(webrtc_peer_is_connected_obj, webrtc_peer_is_connected);

// peer.disconnect() - Bluntly drop the current session without closing the handle
STATIC mp_obj_t webrtc_peer_disconnect(mp_obj_t self_in) {
    webrtc_peer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->peer_handle) {
        ESP_LOGI(TAG, "Bluntly disconnecting current session...");
        int err = esp_peer_disconnect(self->peer_handle);
        return mp_obj_new_int(err);
    }
    return mp_obj_new_int(-1);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(webrtc_peer_disconnect_obj, webrtc_peer_disconnect);

// peer.close() - Close the peer connection
STATIC mp_obj_t webrtc_peer_close(mp_obj_t self_in) {
    webrtc_peer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    // Stop background process task
    if (self->process_task_running) {
        self->process_task_running = false;
        if (self->process_task) {
            // Wait for task to exit (should be quick - just checks flag)
            for (int i = 0; i < 10 && self->process_task; i++) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }
    
    if (self->peer_handle) {
        esp_peer_handle_t handle = self->peer_handle;
        self->peer_handle = NULL;
        
        // Start background task to perform the actual blocking close
        BaseType_t ret = xTaskCreate(webrtc_close_task, "webrtc_close", 4096, (void*)handle, 1, NULL);
        
        if (ret != pdPASS) {
            esp_peer_close(handle);
        }
    }
    
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(webrtc_peer_close_obj, webrtc_peer_close);

// peer.close_sync() - Synchronously close peer (BLOCKS until done)
// Use this when you need resources freed before creating a new peer
STATIC mp_obj_t webrtc_peer_close_sync(mp_obj_t self_in) {
    webrtc_peer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    // Stop background process task
    if (self->process_task_running) {
        self->process_task_running = false;
        if (self->process_task) {
            for (int i = 0; i < 10 && self->process_task; i++) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }
    
    if (self->peer_handle) {
        esp_peer_handle_t handle = self->peer_handle;
        self->peer_handle = NULL;
        
        // Disconnect first to speed up close (drops network immediately)
        esp_peer_disconnect(handle);
        esp_peer_close(handle);
    }
    
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(webrtc_peer_close_sync_obj, webrtc_peer_close_sync);

// peer.process_queue() - Process queued events
//
// This method processes events queued by ESP-IDF callbacks (SDP, ICE, data, state)
// and invokes the registered MicroPython callbacks. Called automatically via
// mp_sched_schedule() when events occur.
//
// Returns: Number of events processed
STATIC mp_obj_t webrtc_peer_process_queue(mp_obj_t self_in) {
    webrtc_peer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    webrtc_event_t event;
    int processed = 0;
    
    // Process all queued events (non-blocking)
    while (xQueueReceive(self->event_queue, &event, 0) == pdTRUE) {
        webrtc_process_event(self, &event);
        processed++;
    }
    
    return mp_obj_new_int(processed);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(webrtc_peer_process_queue_obj, webrtc_peer_process_queue);


//=============================================================================
// MicroPython Class Definition
//=============================================================================

STATIC const mp_rom_map_elem_t webrtc_peer_locals_dict_table[] = {
    // Callback setters
    { MP_ROM_QSTR(MP_QSTR_on_offer), MP_ROM_PTR(&webrtc_peer_on_offer_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_answer), MP_ROM_PTR(&webrtc_peer_on_answer_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_ice), MP_ROM_PTR(&webrtc_peer_on_ice_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_data), MP_ROM_PTR(&webrtc_peer_on_data_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_state), MP_ROM_PTR(&webrtc_peer_on_state_obj) },
    
    // Connection methods
    { MP_ROM_QSTR(MP_QSTR_start_connection), MP_ROM_PTR(&webrtc_peer_start_connection_obj) },
    { MP_ROM_QSTR(MP_QSTR_create_offer), MP_ROM_PTR(&webrtc_peer_create_offer_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_remote_sdp), MP_ROM_PTR(&webrtc_peer_set_remote_sdp_obj) },
    { MP_ROM_QSTR(MP_QSTR_add_ice_candidate), MP_ROM_PTR(&webrtc_peer_add_ice_candidate_obj) },
    
    // Data transfer
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&webrtc_peer_send_obj) },
    
    // Status and event processing
    { MP_ROM_QSTR(MP_QSTR_is_connected), MP_ROM_PTR(&webrtc_peer_is_connected_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&webrtc_peer_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&webrtc_peer_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_close_sync), MP_ROM_PTR(&webrtc_peer_close_sync_obj) },
    { MP_ROM_QSTR(MP_QSTR_process_queue), MP_ROM_PTR(&webrtc_peer_process_queue_obj) },
};
STATIC MP_DEFINE_CONST_DICT(webrtc_peer_locals_dict, webrtc_peer_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    webrtc_peer_type,
    MP_QSTR_Peer,
    MP_TYPE_FLAG_NONE,
    make_new, webrtc_peer_make_new,
    locals_dict, &webrtc_peer_locals_dict
);

//=============================================================================
// C API Functions (for use by other C modules)
//=============================================================================

esp_peer_handle_t webrtc_peer_get_handle(mp_obj_t peer_obj) {
    if (peer_obj == MP_OBJ_NULL) {
        return NULL;
    }
    
    // Verify it's a webrtc.Peer object
    if (!mp_obj_is_type(peer_obj, &webrtc_peer_type)) {
        return NULL;
    }
    
    webrtc_peer_obj_t *self = MP_OBJ_TO_PTR(peer_obj);
    return self->peer_handle;
}

bool webrtc_peer_is_data_channel_open(mp_obj_t peer_obj) {
    if (peer_obj == MP_OBJ_NULL) {
        return false;
    }
    
    // Verify it's a webrtc.Peer object
    if (!mp_obj_is_type(peer_obj, &webrtc_peer_type)) {
        return false;
    }
    
    webrtc_peer_obj_t *self = MP_OBJ_TO_PTR(peer_obj);
    webrtc_lock(self);
    bool is_open = self->data_channel_open;
    webrtc_unlock(self);
    return is_open;
}

int webrtc_peer_send_raw(esp_peer_handle_t handle, const uint8_t *data, size_t len) {
    if (!handle || !data || len == 0) {
        return -1;
    }
    
    esp_peer_data_frame_t frame = {
        .stream_id = 0,
        .data = (uint8_t *)data,
        .size = len
    };
    
    int err = esp_peer_send_data(handle, &frame);
    if (err != 0) {
        ESP_LOGE(TAG, "webrtc_peer_send_raw: Failed to send: %d", err);
        return err;
    }
    
    return 0;
}

//=============================================================================
// Module Definition
//=============================================================================

// webrtc.pre_generate_cert() - Pre-generate DTLS certificates to speed up connection
STATIC mp_obj_t webrtc_pre_generate_cert(void) {
    ESP_LOGI(TAG, "Pre-generating DTLS certificate...");
    int err = esp_peer_pre_generate_cert();
    if (err == 0) {
        ESP_LOGI(TAG, "DTLS certificate pre-generated successfully");
    } else {
        ESP_LOGE(TAG, "Failed to pre-generate DTLS certificate: %d", err);
    }
    return mp_obj_new_int(err);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(webrtc_pre_generate_cert_obj, webrtc_pre_generate_cert);

STATIC const mp_rom_map_elem_t webrtc_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_webrtc) },
    { MP_ROM_QSTR(MP_QSTR_Peer), MP_ROM_PTR(&webrtc_peer_type) },
    
    // Global optimizations
    { MP_ROM_QSTR(MP_QSTR_pre_generate_cert), MP_ROM_PTR(&webrtc_pre_generate_cert_obj) },
    
    // State constants (for on_state callback)
    { MP_ROM_QSTR(MP_QSTR_STATE_CLOSED), MP_ROM_INT(ESP_PEER_STATE_CLOSED) },
    { MP_ROM_QSTR(MP_QSTR_STATE_DISCONNECTED), MP_ROM_INT(ESP_PEER_STATE_DISCONNECTED) },
    { MP_ROM_QSTR(MP_QSTR_STATE_NEW_CONNECTION), MP_ROM_INT(ESP_PEER_STATE_NEW_CONNECTION) },
    { MP_ROM_QSTR(MP_QSTR_STATE_PAIRING), MP_ROM_INT(ESP_PEER_STATE_PAIRING) },
    { MP_ROM_QSTR(MP_QSTR_STATE_PAIRED), MP_ROM_INT(ESP_PEER_STATE_PAIRED) },
    { MP_ROM_QSTR(MP_QSTR_STATE_CONNECTING), MP_ROM_INT(ESP_PEER_STATE_CONNECTING) },
    { MP_ROM_QSTR(MP_QSTR_STATE_CONNECTED), MP_ROM_INT(ESP_PEER_STATE_CONNECTED) },
    { MP_ROM_QSTR(MP_QSTR_STATE_CONNECT_FAILED), MP_ROM_INT(ESP_PEER_STATE_CONNECT_FAILED) },
    { MP_ROM_QSTR(MP_QSTR_STATE_DATA_CHANNEL_CONNECTED), MP_ROM_INT(ESP_PEER_STATE_DATA_CHANNEL_CONNECTED) },
    { MP_ROM_QSTR(MP_QSTR_STATE_DATA_CHANNEL_OPENED), MP_ROM_INT(ESP_PEER_STATE_DATA_CHANNEL_OPENED) },
    { MP_ROM_QSTR(MP_QSTR_STATE_DATA_CHANNEL_CLOSED), MP_ROM_INT(ESP_PEER_STATE_DATA_CHANNEL_CLOSED) },
    { MP_ROM_QSTR(MP_QSTR_STATE_DATA_CHANNEL_DISCONNECTED), MP_ROM_INT(ESP_PEER_STATE_DATA_CHANNEL_DISCONNECTED) },
};
STATIC MP_DEFINE_CONST_DICT(webrtc_module_globals, webrtc_module_globals_table);

const mp_obj_module_t mp_module_webrtc = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&webrtc_module_globals,
};

// Register the module to make it available in MicroPython
MP_REGISTER_MODULE(MP_QSTR_webrtc, mp_module_webrtc);
