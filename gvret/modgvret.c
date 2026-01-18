#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "esp_log.h"

#include "py/runtime.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "driver/twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "modcan.h"  // For CAN manager API
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <sys/select.h>
#include <errno.h>
#include <inttypes.h>  // For PRIx32 format specifier

// Logger tag
static const char *TAG = "GVRET";

// Mutex for protecting gvret_cfg access (especially stats counters)
// This prevents race conditions when accessing stats from Python while callback is updating them
static SemaphoreHandle_t gvret_stats_mutex = NULL;

static void gvret_init_stats_mutex(void) {
    if (gvret_stats_mutex == NULL) {
        gvret_stats_mutex = xSemaphoreCreateMutex();
        if (gvret_stats_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create gvret_stats_mutex");
        }
    }
}

// Mutex for protecting gvret_cfg access (especially stats counters)
static SemaphoreHandle_t gvret_cfg_mutex = NULL;

// GVRET Protocol Constants
#define GVRET_START_BYTE 0xF1
#define GVRET_CMD_BUILD_CAN_FRAME 0x00
#define GVRET_CMD_TIME_SYNC 0x01
#define GVRET_CMD_DIG_INPUTS 0x02
#define GVRET_CMD_ANA_INPUTS 0x03
#define GVRET_CMD_SET_DIG_OUT 0x04
#define GVRET_CMD_SETUP_CANBUS 0x05
#define GVRET_CMD_GET_CANBUS_PARAMS 0x06
#define GVRET_CMD_GET_DEV_INFO 0x07
#define GVRET_CMD_SET_SW_MODE 0x08
#define GVRET_CMD_KEEPALIVE 0x09
#define GVRET_CMD_SET_SYSTYPE 0x0A
#define GVRET_CMD_ECHO_CAN_FRAME 0x0B
#define GVRET_CMD_GET_NUMBUSES 0x0C
#define GVRET_CMD_GET_EXT_BUSES 0x0D
#define GVRET_CMD_SET_EXT_BUSES 0x0E

// Configuration
#define GVRET_RINGBUF_SIZE (8 * 1024) // 8KB buffer for CAN frames
#define GVRET_TCP_PORT 23
#define GVRET_STACK_SIZE 4096
#define GVRET_PRIORITY 5

typedef struct {
    bool enabled;
    int tx_pin;
    int rx_pin;
    int baud_rate;
    can_handle_t can_handle;  // Handle from CAN manager
    TaskHandle_t tcp_task_handle;
    RingbufHandle_t ringbuf_handle;
    int tcp_client_sock;
    int tcp_listen_sock;  // Listen socket for TCP server
    mp_obj_t bitrate_change_callback;  // MicroPython callback for bitrate changes
    // Statistics counters (atomic access from multiple tasks)
    uint32_t rx_count;
    uint32_t tx_count;
    uint32_t dropped_count;
    // Callback safety (atomic counter for active callbacks)
    volatile int callback_active;  // Number of callbacks currently executing
} gvret_config_t;

static gvret_config_t gvret_cfg = {
    .can_handle = NULL,
    .bitrate_change_callback = mp_const_none,
    .rx_count = 0,
    .tx_count = 0,
    .dropped_count = 0,
    .tcp_listen_sock = -1,
    .tcp_client_sock = -1,
    .callback_active = 0
};

#define MAX_FILTERS 16
typedef struct {
    uint32_t id;
    uint32_t mask;
    bool extended;
    bool active;
} gvret_filter_t;

static gvret_filter_t filters[MAX_FILTERS];

// Checksum calculation (currently not used - GVRET sends 0 for checksum)
// static uint8_t checksum_calc(uint8_t *buffer, int length) {
//     uint8_t val = 0;
//     for (int c = 0; c < length; c++) {
//         val ^= buffer[c];
//     }
//     return val;
// }

// CAN RX callback - called by manager's RX dispatcher task
// NOTE: This is called from a FreeRTOS task, not from MicroPython context
// Must be careful about accessing gvret_cfg - it's a static structure so should be safe
static void gvret_can_rx_callback(const twai_message_t *message, void *arg) {
    // Increment callback counter atomically (for barrier in gvret_stop)
    __sync_fetch_and_add(&gvret_cfg.callback_active, 1);
    
    // Early return checks - must be fast and safe
    // Check message pointer first (most likely to be NULL if there's a problem)
    if (message == NULL) {
        __sync_fetch_and_sub(&gvret_cfg.callback_active, 1);
        return;
    }
    
    // Check if GVRET is enabled and ringbuffer exists before processing
    // This prevents crashes if callback is called after GVRET is stopped
    // Note: gvret_cfg is static, so accessing it is safe even if GVRET is stopped
    // The enabled flag acts as a guard to prevent processing
    if (!gvret_cfg.enabled) {
        __sync_fetch_and_sub(&gvret_cfg.callback_active, 1);
        return;  // GVRET is stopped, ignore frame
    }
    
    if (gvret_cfg.ringbuf_handle == NULL) {
        // Ringbuffer not initialized, drop frame
        gvret_init_stats_mutex();
        if (gvret_stats_mutex != NULL && xSemaphoreTake(gvret_stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            gvret_cfg.dropped_count++;
            xSemaphoreGive(gvret_stats_mutex);
        }
        __sync_fetch_and_sub(&gvret_cfg.callback_active, 1);
        return;
    }
    
    // Skip RTR frames - they shouldn't be forwarded
    if (message->rtr) {
        ESP_LOGD(TAG, "Skipping RTR frame: ID=0x%08" PRIx32, message->identifier);
        return;
    }
    
    uint8_t buffer[32]; // GVRET frame buffer (max 32 bytes: start + cmd + timestamp(4) + id(4) + bus+len(1) + data(8) + checksum(1))
    
    // Software Filtering
    bool accepted = false;
    bool has_active_filters = false;
    for (int i = 0; i < MAX_FILTERS; i++) {
        if (filters[i].active) {
            has_active_filters = true;
            // Check if ID matches: (ID & Mask) == (Filter & Mask)
            if ((message->identifier & filters[i].mask) == (filters[i].id & filters[i].mask)) {
                if (message->extd == filters[i].extended) {
                    accepted = true;
                    break;
                }
            }
        }
    }
    
    if (has_active_filters && !accepted) {
        // Increment dropped count for filtered frames
        gvret_init_stats_mutex();
        if (gvret_stats_mutex != NULL && xSemaphoreTake(gvret_stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            gvret_cfg.dropped_count++;
            xSemaphoreGive(gvret_stats_mutex);
        }
        __sync_fetch_and_sub(&gvret_cfg.callback_active, 1);
        return;
    }

    // Format GVRET packet
    int idx = 0;
    buffer[idx++] = GVRET_START_BYTE;
    buffer[idx++] = 0; // Command: Frame Received

    uint32_t now = (uint32_t)esp_timer_get_time();
    buffer[idx++] = (uint8_t)(now & 0xFF);
    buffer[idx++] = (uint8_t)(now >> 8);
    buffer[idx++] = (uint8_t)(now >> 16);
    buffer[idx++] = (uint8_t)(now >> 24);

    uint32_t id = message->identifier;
    if (message->extd) {
        id |= (1 << 31);
    }
    buffer[idx++] = (uint8_t)(id & 0xFF);
    buffer[idx++] = (uint8_t)(id >> 8);
    buffer[idx++] = (uint8_t)(id >> 16);
    buffer[idx++] = (uint8_t)(id >> 24);

    // Bus 0 (default) << 4 | Length
    buffer[idx++] = (0 << 4) | (message->data_length_code & 0x0F);

    for (int i = 0; i < message->data_length_code; i++) {
        buffer[idx++] = message->data[i];
    }

    // Note: SavvyCAN doesn't read checksum byte - it processes frame at rx_step == buildData.length() + 8
    // So we don't send a checksum byte for CAN frames

    // Use non-blocking send to avoid blocking the RX dispatcher task
    // If ringbuffer is full, drop the frame immediately
    BaseType_t ringbuf_result = xRingbufferSend(gvret_cfg.ringbuf_handle, buffer, idx, 0);
    gvret_init_stats_mutex();
    if (gvret_stats_mutex != NULL && xSemaphoreTake(gvret_stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (ringbuf_result != pdTRUE) {
            // Ringbuffer full - drop the frame immediately (non-blocking)
            gvret_cfg.dropped_count++;
            static int ringbuf_full_count = 0;
            ringbuf_full_count++;
            if (ringbuf_full_count == 1 || ringbuf_full_count % 100 == 0) {
                ESP_LOGW(TAG, "Ringbuffer full, dropping frame (count: %d). TCP task may be slow or disconnected.", ringbuf_full_count);
            }
        } else {
            gvret_cfg.rx_count++;
        }
        xSemaphoreGive(gvret_stats_mutex);
    }
    
    // Decrement callback counter (callback execution complete)
    __sync_fetch_and_sub(&gvret_cfg.callback_active, 1);
}

// GVRET Protocol State Machine
typedef enum {
    IDLE,
    GET_COMMAND,
    BUILD_CAN_FRAME,
    TIME_SYNC,
    GET_DIG_INPUTS,
    GET_ANALOG_INPUTS,
    SET_DIG_OUTPUTS,
    SETUP_CANBUS,
    GET_CANBUS_PARAMS,
    GET_DEVICE_INFO,
    SET_SINGLEWIRE_MODE,
    KEEPALIVE,
    SET_SYSTYPE,
    ECHO_CAN_FRAME,
    GET_NUMBUSES,
    GET_EXT_BUSES,
    SET_EXT_BUSES
} gvret_state_t;

static gvret_state_t state = IDLE;
static int step = 0;
static int frame_len = 0;
static uint8_t setup_canbus_buffer[9] = {0};  // Buffer for SETUP_CANBUS payload
static uint8_t build_can_frame_buffer[16] = {0};  // Buffer for BUILD_CAN_FRAME payload (max 16 bytes: 4 ID + 1 bus + 1 len + 8 data + 1 checksum)

// Helper function to send response with error handling
static void send_response(int sock, uint8_t *data, int len) {
    int sent = 0;
    ESP_LOGD(TAG, "Sending response: %d bytes, cmd=0x%02X", len, len > 1 ? data[1] : 0);
    while (sent < len) {
        int ret = send(sock, data + sent, len - sent, 0);
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Socket buffer full, wait a bit and retry
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            } else {
                ESP_LOGE(TAG, "Send error: errno %d", errno);
                break;
            }
        }
        sent += ret;
    }
    if (sent != len) {
        ESP_LOGW(TAG, "Partial send: %d/%d bytes", sent, len);
    } else {
        ESP_LOGD(TAG, "Response sent successfully: %d bytes", sent);
    }
}

static void process_incoming_byte(uint8_t in_byte, int sock) {
    uint32_t now = (uint32_t)esp_timer_get_time();
    uint8_t resp[32];

    switch (state) {
    case IDLE:
        if (in_byte == 0xF1) {
            ESP_LOGD(TAG, "Received 0xF1, entering GET_COMMAND state");
            state = GET_COMMAND;
        } else if (in_byte == 0xE7) {
            ESP_LOGD(TAG, "Received 0xE7 (binary mode), ignoring");
            // Stay in IDLE, we're always in binary mode
        } else {
            ESP_LOGD(TAG, "IDLE: received 0x%02X, ignoring", in_byte);
        }
        break;
    case GET_COMMAND:
        ESP_LOGD(TAG, "CMD: %02X", in_byte);
        switch (in_byte) {
        case 0xE7:
            // Handle 0xE7 even in GET_COMMAND state (shouldn't happen, but be safe)
            ESP_LOGD(TAG, "Received 0xE7 in GET_COMMAND state, resetting to IDLE");
            state = IDLE;
            break;
        case GVRET_CMD_GET_DEV_INFO: // 0x07
            resp[0] = 0xF1;
            resp[1] = 0x07;
            resp[2] = 0x01; // Build number LSB
            resp[3] = 0x00; // Build number MSB
            resp[4] = 0x20; // EEPROM version (ignored by SavvyCAN)
            resp[5] = 0x00; // File type (ignored by SavvyCAN)
            resp[6] = 0x00; // Auto log (ignored by SavvyCAN)
            resp[7] = 0x00; // Single wire mode
            send_response(sock, resp, 8);
            ESP_LOGI(TAG, "Received GET_DEV_INFO");
            state = IDLE;
            break;
        case GVRET_CMD_GET_NUMBUSES: // 0x0C
            resp[0] = 0xF1;
            resp[1] = 0x0C;
            resp[2] = 1; // Num buses (we only have one CAN bus implemented)
            send_response(sock, resp, 3);
            ESP_LOGI(TAG, "Received GET_NUMBUSES");
            state = IDLE;
            break;
        case GVRET_CMD_GET_CANBUS_PARAMS: // 0x06
            resp[0] = 0xF1;
            resp[1] = 0x06;
            // Bus 0: byte 0 = enabled (bits 0-3) | listenOnly (bits 4-7)
            resp[2] = 1; // Enabled (bit 0), no listenOnly
            resp[3] = (uint8_t)(gvret_cfg.baud_rate & 0xFF);
            resp[4] = (uint8_t)(gvret_cfg.baud_rate >> 8);
            resp[5] = (uint8_t)(gvret_cfg.baud_rate >> 16);
            resp[6] = (uint8_t)(gvret_cfg.baud_rate >> 24);
            // Bus 1: byte 0 = enabled (bits 0-3) | listenOnly (bits 4-5) | singleWire (bits 6-7)
            resp[7] = 0; // Disabled, no listenOnly, no singleWire
            resp[8] = 0; // Bus 1 baud LSB
            resp[9] = 0; // Bus 1 baud
            resp[10] = 0; // Bus 1 baud
            resp[11] = 0; // Bus 1 baud MSB
            send_response(sock, resp, 12);
            ESP_LOGI(TAG, "Received GET_CANBUS_PARAMS");
            state = IDLE;
            break;
        case GVRET_CMD_GET_EXT_BUSES: // 0x0D
            resp[0] = 0xF1;
            resp[1] = 0x0D;
            for (int i=0; i<15; i++) resp[2+i] = 0;
            send_response(sock, resp, 17);
            state = IDLE;
            break;
        case GVRET_CMD_SET_EXT_BUSES: // 0x0E
            state = SET_EXT_BUSES;
            step = 0;
            break;
        case GVRET_CMD_KEEPALIVE: // 0x09
            resp[0] = 0xF1;
            resp[1] = 0x09;
            resp[2] = 0xDE;
            resp[3] = 0xAD;
            send_response(sock, resp, 4);
            state = IDLE;
            break;
        case GVRET_CMD_TIME_SYNC: // 0x01
            // SavvyCAN sends F1 01 and expects response, no data payload.
            resp[0] = 0xF1;
            resp[1] = 0x01;
            resp[2] = (uint8_t)(now & 0xFF);
            resp[3] = (uint8_t)(now >> 8);
            resp[4] = (uint8_t)(now >> 16);
            resp[5] = (uint8_t)(now >> 24);
            send_response(sock, resp, 6);
            ESP_LOGI(TAG, "Received TIME_SYNC");
            state = IDLE; 
            break;
        case GVRET_CMD_SETUP_CANBUS: // 0x05
            state = SETUP_CANBUS;
            step = 0;
            break;
        case GVRET_CMD_BUILD_CAN_FRAME: // 0x00
            state = BUILD_CAN_FRAME;
            step = 0;
            break;
        default:
            ESP_LOGW(TAG, "Unknown CMD: %02X", in_byte);
            state = IDLE;
            break;
        }
        break;
    
    // TIME_SYNC state removed as it requires no data bytes

    case SETUP_CANBUS:
        // SavvyCAN sends 9 bytes: CAN0 config (4 bytes), CAN1 config (4 bytes), zero byte (1 byte)
        // CAN0 config format: bitrate (32-bit little-endian) with flags:
        //   Bit 31: Valid flag (if set, use this config)
        //   Bit 30: Enabled flag
        //   Bit 29: Listen-only flag
        if (step < 8) {
            setup_canbus_buffer[step] = in_byte;
        }
        step++;
        if (step >= 9) {
            // Parse CAN0 bitrate (bytes 0-3)
            uint32_t can0_config = (uint32_t)setup_canbus_buffer[0] |
                                   ((uint32_t)setup_canbus_buffer[1] << 8) |
                                   ((uint32_t)setup_canbus_buffer[2] << 16) |
                                   ((uint32_t)setup_canbus_buffer[3] << 24);
            
            // Check if valid flag is set (bit 31)
            if (can0_config & 0x80000000) {
                // Extract bitrate (mask out flag bits)
                uint32_t new_bitrate = can0_config & 0x0FFFFFFF;
                
                if (new_bitrate > 0 && new_bitrate != gvret_cfg.baud_rate) {
                    ESP_LOGI(TAG, "SETUP_CANBUS: New bitrate requested: %lu (current: %d)", 
                             (unsigned long)new_bitrate, gvret_cfg.baud_rate);
                    
                    // Store new bitrate
                    gvret_cfg.baud_rate = new_bitrate;
                    
                    // Call MicroPython callback if registered with saturation detection
                    if (gvret_cfg.bitrate_change_callback != mp_const_none) {
                        static int bitrate_sched_fail_count = 0;
                        bool scheduled = mp_sched_schedule(gvret_cfg.bitrate_change_callback, 
                                                          MP_OBJ_NEW_SMALL_INT(new_bitrate));
                        if (!scheduled) {
                            // Scheduler queue full or MicroPython busy
                            bitrate_sched_fail_count++;
                            ESP_LOGW(TAG, "Failed to schedule bitrate change callback (fail count: %d)", bitrate_sched_fail_count);
                        } else {
                            if (bitrate_sched_fail_count > 0) {
                                ESP_LOGI(TAG, "Bitrate change callback scheduled (was %d failures)", bitrate_sched_fail_count);
                                bitrate_sched_fail_count = 0;
                            }
                        }
                    }
                }
                
                // Handle listen-only flag (bit 29)
                bool listen_only = (can0_config & 0x20000000) != 0;
                if (gvret_cfg.can_handle != NULL) {
                    esp_err_t mode_ret;
                    if (listen_only) {
                        ESP_LOGI(TAG, "SETUP_CANBUS: SavvyCAN requested listen-only mode");
                        mode_ret = can_set_mode(gvret_cfg.can_handle, CAN_CLIENT_MODE_RX_ONLY);
                        if (mode_ret == ESP_ERR_INVALID_STATE) {
                            ESP_LOGW(TAG, "SETUP_CANBUS: Cannot switch to listen-only - other TX clients active");
                        } else if (mode_ret == ESP_OK) {
                            ESP_LOGI(TAG, "SETUP_CANBUS: Switched to listen-only mode");
                        }
                    } else {
                        ESP_LOGI(TAG, "SETUP_CANBUS: SavvyCAN cleared listen-only flag");
                        mode_ret = can_set_mode(gvret_cfg.can_handle, CAN_CLIENT_MODE_TX_ENABLED);
                        if (mode_ret == ESP_OK) {
                            ESP_LOGI(TAG, "SETUP_CANBUS: Switched to TX-enabled mode");
                        }
                    }
                }
            }
            
            // Reset buffer and state
            memset(setup_canbus_buffer, 0, sizeof(setup_canbus_buffer));
            state = IDLE;
        }
        break;

    case SET_EXT_BUSES:
        // SavvyCAN sends 13 bytes (12 config + 1 zero byte)
        step++;
        if (step >= 13) state = IDLE;
        break;

        
    case BUILD_CAN_FRAME:
        // Structure: ID(4), Bus(1), Len(1), Data(Len), Checksum(1)
        // Total bytes: 4 + 1 + 1 + Len + 1 = 7 + Len
        
        // Store byte in buffer (max 16 bytes needed)
        if (step < sizeof(build_can_frame_buffer)) {
            build_can_frame_buffer[step] = in_byte;
        }
        
        if (step == 5) {
            frame_len = in_byte & 0xF;
            if (frame_len > 8) frame_len = 8;
        }
        
        step++;
        
        // When we've received all bytes, transmit the CAN frame
        if (step >= (7 + frame_len)) {
            // Parse frame: ID (bytes 0-3), Bus (byte 4), Len (byte 5), Data (bytes 6+), Checksum (last byte, ignored)
            uint32_t can_id = build_can_frame_buffer[0] |
                             ((uint32_t)build_can_frame_buffer[1] << 8) |
                             ((uint32_t)build_can_frame_buffer[2] << 16) |
                             ((uint32_t)build_can_frame_buffer[3] << 24);
            
            bool extended = (can_id & 0x80000000) != 0;
            can_id &= 0x7FFFFFFF;  // Mask out extended flag
            
            // Bus number (byte 4) - we only support bus 0, ignore others
            uint8_t bus = build_can_frame_buffer[4];
            
            // Length already parsed (byte 5, lower 4 bits)
            // Data starts at byte 6
            
            // Only transmit if CAN handle is valid and enabled
            if (gvret_cfg.can_handle != NULL && gvret_cfg.enabled && bus == 0) {
                twai_message_t tx_msg;
                tx_msg.identifier = can_id;
                tx_msg.flags = extended ? TWAI_MSG_FLAG_EXTD : 0;
                tx_msg.data_length_code = frame_len;
                tx_msg.rtr = 0;
                
                // Copy data (bytes 6 to 6+frame_len-1)
                for (int i = 0; i < frame_len && i < 8; i++) {
                    tx_msg.data[i] = build_can_frame_buffer[6 + i];
                }
                
                // Transmit frame via CAN manager
                esp_err_t tx_ret = can_transmit(gvret_cfg.can_handle, &tx_msg);
                if (tx_ret == ESP_OK) {
                    gvret_init_stats_mutex();
                    if (gvret_stats_mutex != NULL && xSemaphoreTake(gvret_stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        gvret_cfg.tx_count++;
                        xSemaphoreGive(gvret_stats_mutex);
                    }
                    ESP_LOGD(TAG, "TX frame: ID=0x%08" PRIx32 " (%s), len=%d", can_id, extended ? "EXT" : "STD", frame_len);
                } else {
                    gvret_init_stats_mutex();
                    if (gvret_stats_mutex != NULL && xSemaphoreTake(gvret_stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        gvret_cfg.dropped_count++;
                        xSemaphoreGive(gvret_stats_mutex);
                    }
                    ESP_LOGW(TAG, "TX failed: %s (0x%x)", esp_err_to_name(tx_ret), tx_ret);
                }
            } else {
                if (bus != 0) {
                    ESP_LOGW(TAG, "Ignoring frame for unsupported bus %d", bus);
                } else {
                    ESP_LOGW(TAG, "Cannot transmit: CAN handle NULL or GVRET disabled");
                }
                gvret_init_stats_mutex();
                if (gvret_stats_mutex != NULL && xSemaphoreTake(gvret_stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    gvret_cfg.dropped_count++;
                    xSemaphoreGive(gvret_stats_mutex);
                }
            }
            
            // Reset buffer and state
            memset(build_can_frame_buffer, 0, sizeof(build_can_frame_buffer));
            state = IDLE;
            step = 0;
            frame_len = 0;
        }
        break;

    default:
        state = IDLE;
        break;
    }
}

static void gvret_reset_state(void) {
    state = IDLE;
    step = 0;
    frame_len = 0;
    memset(setup_canbus_buffer, 0, sizeof(setup_canbus_buffer));
    memset(build_can_frame_buffer, 0, sizeof(build_can_frame_buffer));
    ESP_LOGI(TAG, "GVRET State Reset");
}

static void tcp_server_task(void *arg) {
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        gvret_cfg.enabled = false;
        vTaskDelete(NULL);
        return;
    }
    gvret_cfg.tcp_listen_sock = listen_sock;  // Store for cleanup

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(GVRET_TCP_PORT);

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(listen_sock);
        gvret_cfg.enabled = false;
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        close(listen_sock);
        gvret_cfg.enabled = false;
        vTaskDelete(NULL);
        return;
    }

    // Wait for enabled flag to be set (tasks are created before enabled is set)
    while (!gvret_cfg.enabled) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    while (gvret_cfg.enabled) {
        ESP_LOGI(TAG, "Socket listening");
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            // Check if we should exit before reporting error
            if (!gvret_cfg.enabled) {
                ESP_LOGI(TAG, "TCP server task exiting (GVRET disabled)");
                break;
            }
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Socket accepted from client");
        gvret_cfg.tcp_client_sock = sock;
        gvret_reset_state(); // Reset state for new connection
        
        // Stage 2: Activate CAN client (bus activates to NORMAL mode)
        if (gvret_cfg.can_handle != NULL) {
            esp_err_t ret = can_activate(gvret_cfg.can_handle);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to activate CAN client: %s", esp_err_to_name(ret));
                close(sock);
                gvret_cfg.tcp_client_sock = -1;
                continue;
            }
            ESP_LOGI(TAG, "CAN client activated - bus now active");
        }
        
        ESP_LOGI(TAG, "GVRET ready for commands and CAN frame transmission");

        // Set non-blocking to allow select/polling
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        
        // Enable TCP_NODELAY to reduce latency (important for SavvyCAN)
        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        
        ESP_LOGI(TAG, "Client connected, socket configured (TCP_NODELAY enabled)");

        // Adaptive timeout: shorter when processing commands, longer when idle
        bool in_command_processing = false;
        uint32_t last_rx_time = esp_timer_get_time() / 1000; // ms

        while (gvret_cfg.enabled) {
            // Check if we should exit before processing
            if (!gvret_cfg.enabled) {
                ESP_LOGI(TAG, "TCP client loop exiting (GVRET disabled)");
                break;
            }
            
            fd_set read_fds;
            fd_set write_fds;
            FD_ZERO(&read_fds);
            FD_ZERO(&write_fds);
            FD_SET(sock, &read_fds);
            FD_SET(sock, &write_fds);

            // Adaptive timeout: Use short timeout after recent RX activity, longer when idle
            uint32_t now = esp_timer_get_time() / 1000; // ms
            uint32_t time_since_rx = now - last_rx_time;
            
            struct timeval timeout;
            timeout.tv_sec = 0;
            if (time_since_rx < 100) {
                // Recently received data - use short timeout for responsive command processing
                timeout.tv_usec = 1000; // 1ms
            } else {
                // Idle - use longer timeout to reduce CPU usage and allow frame batching
                timeout.tv_usec = 50000; // 50ms
            }

            int activity = select(sock + 1, &read_fds, &write_fds, NULL, &timeout);

            if (activity < 0) {
                ESP_LOGE(TAG, "Select error: errno %d", errno);
                break;
            }

            // Track if we processed incoming data this iteration
            // This prevents sending CAN frames while command responses are being sent
            bool processed_incoming = false;

            // Check for incoming data (Commands from SavvyCAN)
            // Read all available data in a loop (SavvyCAN may send multiple commands in one packet)
            if (FD_ISSET(sock, &read_fds)) {
                while (1) {
                    uint8_t rx_buffer[64];
                    int len = recv(sock, rx_buffer, sizeof(rx_buffer), 0);
                    if (len > 0) {
                        processed_incoming = true;
                        last_rx_time = esp_timer_get_time() / 1000; // Update for adaptive timeout
                        ESP_LOGD(TAG, "Received %d bytes", len);
                        for (int i = 0; i < len; i++) {
                            ESP_LOGD(TAG, "Processing byte %d: 0x%02X", i, rx_buffer[i]);
                            process_incoming_byte(rx_buffer[i], sock);
                        }
                        // Continue reading if more data is available
                    } else if (len == 0) {
                        ESP_LOGI(TAG, "Connection closed");
                        goto connection_closed;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // No more data available, break out of read loop
                            break;
                        } else {
                            ESP_LOGE(TAG, "Recv error: errno %d", errno);
                            goto connection_closed;
                        }
                    }
                }
            }

            // Check for outgoing data (CAN Frames to SavvyCAN)
            // CRITICAL: Only send CAN frames when NOT processing commands to avoid TCP stream collision
            // Command responses (from send_response) and CAN frames must not interleave
            // Only send CAN frames when:
            // 1. Socket is writable
            // 2. We're in IDLE state (not processing a command)
            // 3. We didn't process incoming data this iteration (to avoid interleaving with command responses)
            if (FD_ISSET(sock, &write_fds) && state == IDLE && !processed_incoming) {
                size_t item_size;
                uint8_t *item = (uint8_t *)xRingbufferReceive(gvret_cfg.ringbuf_handle, &item_size, 0);
                if (item) {
                    // Extract CAN ID from frame for logging (bytes 6-9 after F1 00 timestamp)
                    uint32_t can_id = 0;
                    if (item_size >= 10) {
                        can_id = item[6] | (item[7] << 8) | (item[8] << 16) | (item[9] << 24);
                        can_id &= 0x7FFFFFFF; // Mask out extended bit
                    }
                    int written = send(sock, item, item_size, 0);
                    vRingbufferReturnItem(gvret_cfg.ringbuf_handle, (void *)item);
                    if (written < 0) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
                            break;
                        } else {
                            ESP_LOGW(TAG, "Send would block, dropping frame (TCP buffer full)");
                        }
                    } else if (written != item_size) {
                        ESP_LOGW(TAG, "Partial send: %d/%d bytes", written, item_size);
                    }
                }
            }
        }
        
        connection_closed:
        // Deactivate CAN client when connection closes
        if (gvret_cfg.can_handle != NULL) {
            can_deactivate(gvret_cfg.can_handle);
            ESP_LOGI(TAG, "CAN client deactivated - bus may go to STOPPED or LISTEN_ONLY");
        }
        
        if (sock != -1) {
            shutdown(sock, 0);
            close(sock);
            gvret_cfg.tcp_client_sock = -1;
        }
    }
    if (listen_sock >= 0) {
        close(listen_sock);
        gvret_cfg.tcp_listen_sock = -1;
    }
    ESP_LOGI(TAG, "TCP server task exiting");
    vTaskDelete(NULL);
}

void gvret_init(void) {
    // Initialize config defaults if needed
}

// Forward declaration
void gvret_stop(void);

// Set CAN handle from shared CAN module (deprecated - use can_register() instead)
// This function is kept for backward compatibility but should not be used
void gvret_set_can_handle(twai_handle_t handle) {
    // This function is deprecated - GVRET now uses CAN manager API
    // Keeping for backward compatibility but does nothing
    (void)handle;
}

bool gvret_start(int tx_pin, int rx_pin, int baud_rate) {
    // If already enabled, stop first to ensure clean state
    if (gvret_cfg.enabled) {
        ESP_LOGI(TAG, "GVRET already running, stopping first");
        gvret_stop();
        // NOTE: Reduced delay to avoid blocking MicroPython/WebREPL for too long
        // gvret_stop() already waits for tasks to exit
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Ensure all handles are NULL before starting (except can_handle which will be set below)
    gvret_cfg.tcp_task_handle = NULL;
    gvret_cfg.ringbuf_handle = NULL;
    gvret_cfg.tcp_client_sock = -1;
    gvret_cfg.tcp_listen_sock = -1;
    gvret_cfg.enabled = false;

    gvret_cfg.tx_pin = tx_pin;
    gvret_cfg.rx_pin = rx_pin;
    gvret_cfg.baud_rate = baud_rate;
    
    // Reset statistics when starting
    gvret_init_stats_mutex();
    if (gvret_stats_mutex != NULL && xSemaphoreTake(gvret_stats_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        gvret_cfg.rx_count = 0;
        gvret_cfg.tx_count = 0;
        gvret_cfg.dropped_count = 0;
        xSemaphoreGive(gvret_stats_mutex);
    }

    ESP_LOGI(TAG, "GVRET start: TX=%d, RX=%d, bitrate=%d", tx_pin, rx_pin, baud_rate);

    // Stage 1: Register with CAN manager (bus stays STOPPED)
    gvret_cfg.can_handle = can_register(CAN_CLIENT_MODE_TX_ENABLED);
    if (gvret_cfg.can_handle == NULL) {
        ESP_LOGE(TAG, "Failed to register with CAN manager");
        gvret_cfg.enabled = false;
        return false;
    }
    
    // Set RX callback for receiving frames
    can_set_rx_callback(gvret_cfg.can_handle, gvret_can_rx_callback, NULL);
    
    // Create Ring Buffer
    gvret_cfg.ringbuf_handle = xRingbufferCreate(GVRET_RINGBUF_SIZE, RINGBUF_TYPE_NOSPLIT);
    if (gvret_cfg.ringbuf_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        can_unregister(gvret_cfg.can_handle);
        gvret_cfg.can_handle = NULL;
        gvret_cfg.enabled = false;
        return false;
    }
    
    // Note: Bus stays STOPPED until TCP client connects (can_activate() called)
    // No CAN RX task needed - manager's RX dispatcher will call our callback

    if (xTaskCreate(tcp_server_task, "tcp_server", GVRET_STACK_SIZE, NULL, GVRET_PRIORITY, &gvret_cfg.tcp_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TCP server task");
        vRingbufferDelete(gvret_cfg.ringbuf_handle);
        gvret_cfg.ringbuf_handle = NULL;
        can_unregister(gvret_cfg.can_handle);
        gvret_cfg.can_handle = NULL;
        gvret_cfg.enabled = false;
        return false;
    }

    // Set enabled AFTER tasks are created and initialized
    // Tasks check enabled flag in their loops, so they'll wait until it's set
    // NOTE: Removed delay to avoid blocking MicroPython/WebREPL
    // TCP task will check enabled flag in its loop
    gvret_cfg.enabled = true;
    
    ESP_LOGI(TAG, "GVRET started successfully (registered, waiting for TCP client)");
    return true;
}

void gvret_stop(void) {
    if (!gvret_cfg.enabled) return;

    ESP_LOGI(TAG, "Stopping GVRET...");

    // Set enabled to false FIRST so tasks exit gracefully
    gvret_cfg.enabled = false;
    
    // Deactivate CAN client if still activated
    if (gvret_cfg.can_handle != NULL) {
        can_deactivate(gvret_cfg.can_handle);
    }

    // Close listen socket to make accept() return in TCP task
    if (gvret_cfg.tcp_listen_sock >= 0) {
        close(gvret_cfg.tcp_listen_sock);
        gvret_cfg.tcp_listen_sock = -1;
    }

    // Close any active TCP client connection first to avoid crash
    if (gvret_cfg.tcp_client_sock >= 0) {
        shutdown(gvret_cfg.tcp_client_sock, 0);
        close(gvret_cfg.tcp_client_sock);
        gvret_cfg.tcp_client_sock = -1;
    }

    // Give tasks time to exit gracefully (they check enabled flag and delete themselves)
    // Wait longer to ensure tasks have fully exited and are no longer accessing resources
    vTaskDelay(pdMS_TO_TICKS(200));

    // Tasks delete themselves when enabled=false, so we just need to clear handles
    // Don't try to delete tasks here - they've already deleted themselves
    // Just clear the handles to mark them as gone
    gvret_cfg.tcp_task_handle = NULL;

    // Unregister from CAN manager (manager handles bus state)
    // This will mark client as pending_delete and prevent new callbacks
    if (gvret_cfg.can_handle != NULL) {
        can_unregister(gvret_cfg.can_handle);
        gvret_cfg.can_handle = NULL;
    }

    // CRITICAL: Wait for all active callbacks to complete before deleting ringbuffer
    // This prevents use-after-free if callbacks are still accessing ringbuffer
    ESP_LOGI(TAG, "Waiting for active callbacks to complete...");
    int callback_wait_iterations = 0;
    const int max_wait_iterations = 100; // 1 second timeout (10ms * 100)
    while (__sync_fetch_and_add(&gvret_cfg.callback_active, 0) > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        callback_wait_iterations++;
        if (callback_wait_iterations >= max_wait_iterations) {
            ESP_LOGW(TAG, "Timeout waiting for callbacks to complete (callback_active=%d), proceeding anyway",
                     gvret_cfg.callback_active);
            break;
        }
    }
    if (callback_wait_iterations > 0) {
        ESP_LOGI(TAG, "All callbacks completed after %d ms", callback_wait_iterations * 10);
    }

    // Free Ring Buffer - now safe to delete after callback barrier
    if (gvret_cfg.ringbuf_handle != NULL) {
        vRingbufferDelete(gvret_cfg.ringbuf_handle);
        gvret_cfg.ringbuf_handle = NULL;
    }
    
    // Reset statistics
    gvret_init_stats_mutex();
    if (gvret_stats_mutex != NULL && xSemaphoreTake(gvret_stats_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        gvret_cfg.rx_count = 0;
        gvret_cfg.tx_count = 0;
        gvret_cfg.dropped_count = 0;
        xSemaphoreGive(gvret_stats_mutex);
    }
    
    ESP_LOGI(TAG, "GVRET stopped");
}

void gvret_add_filter(uint32_t id, uint32_t mask, bool extended) {
    for (int i = 0; i < MAX_FILTERS; i++) {
        if (!filters[i].active) {
            filters[i].id = id;
            filters[i].mask = mask;
            filters[i].extended = extended;
            filters[i].active = true;
            return;
        }
    }
}

void gvret_clear_filters(void) {
    for (int i = 0; i < MAX_FILTERS; i++) {
        filters[i].active = false;
    }
}

void gvret_get_stats(uint32_t *rx_count, uint32_t *tx_count, uint32_t *drop_count) {
    gvret_init_stats_mutex();
    
    if (gvret_stats_mutex != NULL && xSemaphoreTake(gvret_stats_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (rx_count) *rx_count = gvret_cfg.rx_count;
        if (tx_count) *tx_count = gvret_cfg.tx_count;
        if (drop_count) *drop_count = gvret_cfg.dropped_count;
        xSemaphoreGive(gvret_stats_mutex);
    } else {
        // Mutex timeout - return zeros to be safe (shouldn't happen in normal operation)
        if (rx_count) *rx_count = 0;
        if (tx_count) *tx_count = 0;
        if (drop_count) *drop_count = 0;
    }
}

// MicroPython wrapper functions
static mp_obj_t gvret_start_wrapper(size_t n_args, const mp_obj_t *args) {
    int tx_pin = mp_obj_get_int(args[0]);
    int rx_pin = mp_obj_get_int(args[1]);
    int baud_rate = mp_obj_get_int(args[2]);
    
    if (gvret_start(tx_pin, rx_pin, baud_rate)) {
        return mp_const_true;
    } else {
        return mp_const_false;
    }
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gvret_start_obj, 3, 3, gvret_start_wrapper);

static mp_obj_t gvret_stop_wrapper(void) {
    gvret_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gvret_stop_obj, gvret_stop_wrapper);

static mp_obj_t gvret_add_filter_wrapper(size_t n_args, const mp_obj_t *args) {
    uint32_t id = mp_obj_get_int(args[0]);
    uint32_t mask = mp_obj_get_int(args[1]);
    bool extended = mp_obj_is_true(args[2]);
    gvret_add_filter(id, mask, extended);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gvret_add_filter_obj, 3, 3, gvret_add_filter_wrapper);

static mp_obj_t gvret_clear_filters_wrapper(void) {
    gvret_clear_filters();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gvret_clear_filters_obj, gvret_clear_filters_wrapper);

static mp_obj_t gvret_set_bitrate_change_callback_wrapper(mp_obj_t callback) {
    if (callback == mp_const_none || mp_obj_is_callable(callback)) {
        gvret_cfg.bitrate_change_callback = callback;
        ESP_LOGI(TAG, "Bitrate change callback registered");
        return mp_const_none;
    } else {
        mp_raise_TypeError(MP_ERROR_TEXT("callback must be callable or None"));
    }
}
static MP_DEFINE_CONST_FUN_OBJ_1(gvret_set_bitrate_change_callback_obj, gvret_set_bitrate_change_callback_wrapper);

static mp_obj_t gvret_get_bitrate_wrapper(void) {
    return mp_obj_new_int(gvret_cfg.baud_rate);
}
static MP_DEFINE_CONST_FUN_OBJ_0(gvret_get_bitrate_obj, gvret_get_bitrate_wrapper);

static mp_obj_t gvret_get_stats_wrapper(void) {
    uint32_t rx, tx, dropped;
    gvret_get_stats(&rx, &tx, &dropped);
    
    mp_obj_t tuple[3];
    tuple[0] = mp_obj_new_int_from_uint(rx);
    tuple[1] = mp_obj_new_int_from_uint(tx);
    tuple[2] = mp_obj_new_int_from_uint(dropped);
    
    return mp_obj_new_tuple(3, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_0(gvret_get_stats_obj, gvret_get_stats_wrapper);

// Module globals table
static const mp_rom_map_elem_t gvret_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_gvret) },
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&gvret_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&gvret_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_add_filter), MP_ROM_PTR(&gvret_add_filter_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear_filters), MP_ROM_PTR(&gvret_clear_filters_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_bitrate_change_callback), MP_ROM_PTR(&gvret_set_bitrate_change_callback_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_bitrate), MP_ROM_PTR(&gvret_get_bitrate_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_stats), MP_ROM_PTR(&gvret_get_stats_obj) },
};
static MP_DEFINE_CONST_DICT(gvret_module_globals, gvret_module_globals_table);

// Module definition
const mp_obj_module_t gvret_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&gvret_module_globals,
};

// Register the module
MP_REGISTER_MODULE(MP_QSTR_gvret, gvret_user_cmodule);
