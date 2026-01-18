/**
 * modplc.c - HomePlug / SLAC / EXI module for CCS EVSE emulation
 * 
 * This module provides:
 * 1. Raw Ethernet access via L2TAP for HomePlug MME (EtherType 0x88E1)
 * 2. SLAC protocol responder (EVSE mode) state machine
 * 3. MME commands to configure TPLink modem (SET_KEY, GET_KEY, GET_SW)
 * 4. DIN 70121 EXI codec for V2G messages (future)
 * 
 * Architecture follows GVRET pattern:
 * - FreeRTOS task for autonomous SLAC handling
 * - Ringbuffer for async event delivery
 * - mp_sched_schedule() for callbacks to Python
 * 
 * Hardware: ESP32-P4 with Ethernet + TP-Link TL-PA4010P (EVSE PIB)
 * 
 * References:
 * - pyPLC: https://github.com/uhi22/pyPLC
 * - HomePlug GP: ISO 15118-3
 * - SLAC: Signal Level Attenuation Characterization
 */

#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "esp_log.h"

#include "py/runtime.h"
#include "py/mphal.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "esp_vfs_l2tap.h"
#include "esp_netif.h"
#include "esp_eth.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "exi_din.h"

static const char *TAG = "PLC";

// ============================================================================
// HomePlug Constants
// ============================================================================

#define HOMEPLUG_ETHERTYPE      0x88E1  // HomePlug MME EtherType

// HomePlug MME Types (Management Message Entry)
#define CM_SET_KEY              0x6008
#define CM_GET_KEY              0x600C
#define CM_SC_JOIN              0x6010
#define CM_CHAN_EST             0x6014
#define CM_TM_UPDATE            0x6018
#define CM_AMP_MAP              0x601C
#define CM_BRG_INFO             0x6020
#define CM_CONN_NEW             0x6024
#define CM_CONN_REL             0x6028
#define CM_CONN_MOD             0x602C
#define CM_CONN_INFO            0x6030
#define CM_STA_CAP              0x6034
#define CM_NW_INFO              0x6038
#define CM_GET_BEACON           0x603C
#define CM_HFID                 0x6040
#define CM_MME_ERROR            0x6044
#define CM_NW_STATS             0x6048
#define CM_SLAC_PARAM           0x6064
#define CM_START_ATTEN_CHAR     0x6068
#define CM_ATTEN_CHAR           0x606C
#define CM_PKCS_CERT            0x6070
#define CM_MNBC_SOUND           0x6074
#define CM_VALIDATE             0x6078
#define CM_SLAC_MATCH           0x607C
#define CM_SLAC_USER_DATA       0x6080
#define CM_ATTEN_PROFILE        0x6084
#define CM_GET_SW               0xA000

// MME subtypes
#define MMTYPE_REQ              0x0000
#define MMTYPE_CNF              0x0001
#define MMTYPE_IND              0x0002
#define MMTYPE_RSP              0x0003

// Broadcast MAC for HomePlug
static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t QUALCOMM_OUI[3] = {0x00, 0xB0, 0x52};  // Qualcomm Atheros OUI for MME

// ============================================================================
// SLAC State Machine (EVSE Mode)
// ============================================================================

typedef enum {
    SLAC_STATE_IDLE = 0,
    SLAC_STATE_WAIT_PARAM_REQ,      // Waiting for SLAC_PARAM.REQ from car
    SLAC_STATE_WAIT_ATTEN_CHAR,     // Sent PARAM.CNF, waiting for sounds
    SLAC_STATE_WAIT_MATCH_REQ,      // Sent ATTEN_CHAR.IND, waiting for MATCH.REQ
    SLAC_STATE_MATCHED,             // SLAC complete, network formed
    SLAC_STATE_ERROR
} slac_state_t;

static const char* slac_state_names[] = {
    "IDLE",
    "WAIT_PARAM_REQ",
    "WAIT_ATTEN_CHAR", 
    "WAIT_MATCH_REQ",
    "MATCHED",
    "ERROR"
};

// ============================================================================
// Module Configuration
// ============================================================================

#define PLC_STACK_SIZE      8192
#define PLC_PRIORITY        5
#define PLC_RINGBUF_SIZE    4096
#define MAX_FRAME_SIZE      1518
#define SLAC_TIMEOUT_MS     10000   // 10s timeout for SLAC phases

typedef struct {
    // State
    bool enabled;
    slac_state_t slac_state;
    
    // L2TAP file descriptor
    int l2tap_fd;
    
    // Task handle
    TaskHandle_t task_handle;
    
    // Ringbuffer for events to Python
    RingbufHandle_t event_ringbuf;
    
    // Python callback for SLAC completion
    mp_obj_t slac_callback;
    
    // Our MAC address (from Ethernet interface)
    uint8_t our_mac[6];
    
    // Car's MAC address (learned during SLAC)
    uint8_t car_mac[6];
    bool car_mac_known;
    
    // SLAC session data
    uint8_t run_id[8];          // RunID from SLAC_PARAM.REQ
    uint8_t application_type;   // 0x00 = EV charging
    uint8_t security_type;      // 0x00 = no security
    
    // NID/NMK (Network ID / Network Membership Key)
    uint8_t nid[7];             // 54 bits, but stored as 7 bytes
    uint8_t nmk[16];            // 128-bit key
    bool nid_nmk_set;
    
    // Attenuation data
    uint8_t num_sounds_received;
    uint8_t atten_profile[58];  // Average attenuation per group
    
    // Timing
    uint32_t state_enter_time_ms;
    
    // Statistics
    uint32_t rx_count;
    uint32_t tx_count;
    uint32_t slac_attempts;
    uint32_t slac_success;
    
} plc_config_t;

static plc_config_t plc_cfg = {
    .enabled = false,
    .slac_state = SLAC_STATE_IDLE,
    .l2tap_fd = -1,
    .task_handle = NULL,
    .event_ringbuf = NULL,
    .slac_callback = mp_const_none,
    .car_mac_known = false,
    .nid_nmk_set = false,
    .num_sounds_received = 0,
    .rx_count = 0,
    .tx_count = 0,
    .slac_attempts = 0,
    .slac_success = 0,
};

static SemaphoreHandle_t plc_mutex = NULL;

// ============================================================================
// Utility Functions
// ============================================================================

static void plc_init_mutex(void) {
    if (plc_mutex == NULL) {
        plc_mutex = xSemaphoreCreateMutex();
    }
}

static uint32_t get_time_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void mac_to_string(const uint8_t *mac, char *str) {
    sprintf(str, "%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void enter_slac_state(slac_state_t new_state) {
    ESP_LOGI(TAG, "SLAC state: %s -> %s", 
             slac_state_names[plc_cfg.slac_state],
             slac_state_names[new_state]);
    plc_cfg.slac_state = new_state;
    plc_cfg.state_enter_time_ms = get_time_ms();
}

// ============================================================================
// MME Frame Construction
// ============================================================================

// Ethernet + HomePlug MME header structure
typedef struct __attribute__((packed)) {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype;         // 0x88E1 in network byte order
    uint8_t  mme_version;       // 0x01
    uint16_t mme_type;          // Management message type
    uint8_t  oui[3];            // Vendor OUI (Qualcomm)
} mme_header_t;

static int compose_mme_header(uint8_t *buffer, const uint8_t *dst_mac, uint16_t mme_type) {
    mme_header_t *hdr = (mme_header_t *)buffer;
    
    memcpy(hdr->dst_mac, dst_mac, 6);
    memcpy(hdr->src_mac, plc_cfg.our_mac, 6);
    hdr->ethertype = htons(HOMEPLUG_ETHERTYPE);
    hdr->mme_version = 0x01;
    hdr->mme_type = mme_type;  // Little-endian on the wire
    memcpy(hdr->oui, QUALCOMM_OUI, 3);
    
    return sizeof(mme_header_t);
}

// ============================================================================
// SLAC Message Handlers
// ============================================================================

/**
 * Compose CM_SLAC_PARAM.CNF response
 * Sent in response to CM_SLAC_PARAM.REQ from car
 */
static int compose_slac_param_cnf(uint8_t *buffer) {
    int idx = compose_mme_header(buffer, plc_cfg.car_mac, CM_SLAC_PARAM | MMTYPE_CNF);
    
    // M-SOUND_TARGET (6 bytes) - broadcast
    memcpy(&buffer[idx], BROADCAST_MAC, 6);
    idx += 6;
    
    // NUM_SOUNDS (1 byte) - number of sounds to expect
    buffer[idx++] = 10;
    
    // TIME_OUT (1 byte) - timeout in 100ms units
    buffer[idx++] = 6;  // 600ms
    
    // RESP_TYPE (1 byte) - response type
    buffer[idx++] = 0x01;
    
    // FORWARDING_STA (6 bytes) - our MAC
    memcpy(&buffer[idx], plc_cfg.our_mac, 6);
    idx += 6;
    
    // APPLICATION_TYPE (1 byte)
    buffer[idx++] = plc_cfg.application_type;
    
    // SECURITY_TYPE (1 byte)
    buffer[idx++] = plc_cfg.security_type;
    
    // RunID (8 bytes) - echo back from request
    memcpy(&buffer[idx], plc_cfg.run_id, 8);
    idx += 8;
    
    ESP_LOGI(TAG, "Composed SLAC_PARAM.CNF (%d bytes)", idx);
    return idx;
}

/**
 * Compose CM_ATTEN_CHAR.IND
 * Sent after receiving sounds, contains attenuation profile
 */
static int compose_atten_char_ind(uint8_t *buffer) {
    int idx = compose_mme_header(buffer, plc_cfg.car_mac, CM_ATTEN_CHAR | MMTYPE_IND);
    
    // APPLICATION_TYPE (1 byte)
    buffer[idx++] = plc_cfg.application_type;
    
    // SECURITY_TYPE (1 byte)
    buffer[idx++] = plc_cfg.security_type;
    
    // SOURCE_ADDRESS (6 bytes) - car's MAC
    memcpy(&buffer[idx], plc_cfg.car_mac, 6);
    idx += 6;
    
    // RunID (8 bytes)
    memcpy(&buffer[idx], plc_cfg.run_id, 8);
    idx += 8;
    
    // SOURCE_ID (17 bytes) - zero-padded
    memset(&buffer[idx], 0, 17);
    idx += 17;
    
    // RESP_ID (17 bytes) - zero-padded
    memset(&buffer[idx], 0, 17);
    idx += 17;
    
    // NUM_SOUNDS (1 byte)
    buffer[idx++] = plc_cfg.num_sounds_received;
    
    // ATTEN_PROFILE (58 bytes) - attenuation per frequency group
    // Use plausible fake values if we don't have real measurements
    for (int i = 0; i < 58; i++) {
        // Fake attenuation: ~20dB average (plausible for short cable)
        buffer[idx++] = (plc_cfg.atten_profile[i] != 0) ? plc_cfg.atten_profile[i] : 20;
    }
    
    ESP_LOGI(TAG, "Composed ATTEN_CHAR.IND (%d bytes)", idx);
    return idx;
}

/**
 * Compose CM_SLAC_MATCH.CNF
 * Final SLAC response with NID/NMK
 */
static int compose_slac_match_cnf(uint8_t *buffer) {
    int idx = compose_mme_header(buffer, plc_cfg.car_mac, CM_SLAC_MATCH | MMTYPE_CNF);
    
    // APPLICATION_TYPE (1 byte)
    buffer[idx++] = plc_cfg.application_type;
    
    // SECURITY_TYPE (1 byte)
    buffer[idx++] = plc_cfg.security_type;
    
    // MVFLength (2 bytes) - length of variable fields
    buffer[idx++] = 0x3E;  // 62 bytes
    buffer[idx++] = 0x00;
    
    // PEV_ID (17 bytes) - from match request, zero for now
    memset(&buffer[idx], 0, 17);
    idx += 17;
    
    // PEV_MAC (6 bytes) - car's MAC
    memcpy(&buffer[idx], plc_cfg.car_mac, 6);
    idx += 6;
    
    // EVSE_ID (17 bytes) - our ID
    memset(&buffer[idx], 0, 17);
    buffer[idx] = 'E';
    buffer[idx+1] = 'V';
    buffer[idx+2] = 'S';
    buffer[idx+3] = 'E';
    idx += 17;
    
    // EVSE_MAC (6 bytes) - our MAC
    memcpy(&buffer[idx], plc_cfg.our_mac, 6);
    idx += 6;
    
    // RunID (8 bytes)
    memcpy(&buffer[idx], plc_cfg.run_id, 8);
    idx += 8;
    
    // Reserved (8 bytes)
    memset(&buffer[idx], 0, 8);
    idx += 8;
    
    // NID (7 bytes) - Network ID
    memcpy(&buffer[idx], plc_cfg.nid, 7);
    idx += 7;
    
    // Reserved (1 byte)
    buffer[idx++] = 0;
    
    // NMK (16 bytes) - Network Membership Key
    memcpy(&buffer[idx], plc_cfg.nmk, 16);
    idx += 16;
    
    ESP_LOGI(TAG, "Composed SLAC_MATCH.CNF (%d bytes)", idx);
    return idx;
}

/**
 * Compose CM_SET_KEY.REQ
 * Configure modem with NID/NMK
 */
static int compose_set_key_req(uint8_t *buffer) {
    int idx = compose_mme_header(buffer, BROADCAST_MAC, CM_SET_KEY | MMTYPE_REQ);
    
    // KeyType (1 byte) - 0x01 = NMK
    buffer[idx++] = 0x01;
    
    // MyNonce (4 bytes)
    buffer[idx++] = 0x00;
    buffer[idx++] = 0x00;
    buffer[idx++] = 0x00;
    buffer[idx++] = 0x00;
    
    // YourNonce (4 bytes)
    buffer[idx++] = 0x00;
    buffer[idx++] = 0x00;
    buffer[idx++] = 0x00;
    buffer[idx++] = 0x00;
    
    // PID (4 bytes) - Protocol Run Identifier
    buffer[idx++] = 0x04;
    buffer[idx++] = 0x00;
    buffer[idx++] = 0x00;
    buffer[idx++] = 0x00;
    
    // PRN (2 bytes) - Protocol Run Number
    buffer[idx++] = 0x00;
    buffer[idx++] = 0x00;
    
    // PMN (1 byte) - Protocol Message Number
    buffer[idx++] = 0x00;
    
    // CCo_Capability (1 byte) - Central Coordinator capable
    buffer[idx++] = 0x02;  // CCo capable
    
    // NID (7 bytes)
    memcpy(&buffer[idx], plc_cfg.nid, 7);
    idx += 7;
    
    // NewEKS (1 byte) - New Encryption Key Select
    buffer[idx++] = 0x01;
    
    // NewKey (16 bytes) - NMK
    memcpy(&buffer[idx], plc_cfg.nmk, 16);
    idx += 16;
    
    ESP_LOGI(TAG, "Composed SET_KEY.REQ (%d bytes)", idx);
    return idx;
}

/**
 * Compose CM_GET_SW.REQ
 * Request software version from modem
 */
static int compose_get_sw_req(uint8_t *buffer) {
    int idx = compose_mme_header(buffer, BROADCAST_MAC, CM_GET_SW | MMTYPE_REQ);
    // No payload for GET_SW.REQ
    ESP_LOGD(TAG, "Composed GET_SW.REQ (%d bytes)", idx);
    return idx;
}

// ============================================================================
// SLAC Message Parsing
// ============================================================================

static void handle_slac_param_req(const uint8_t *frame, int len) {
    if (plc_cfg.slac_state != SLAC_STATE_WAIT_PARAM_REQ &&
        plc_cfg.slac_state != SLAC_STATE_IDLE) {
        ESP_LOGW(TAG, "SLAC_PARAM.REQ received in unexpected state %s",
                 slac_state_names[plc_cfg.slac_state]);
        return;
    }
    
    const mme_header_t *hdr = (const mme_header_t *)frame;
    const uint8_t *payload = frame + sizeof(mme_header_t);
    
    // Save car's MAC
    memcpy(plc_cfg.car_mac, hdr->src_mac, 6);
    plc_cfg.car_mac_known = true;
    
    char mac_str[18];
    mac_to_string(plc_cfg.car_mac, mac_str);
    ESP_LOGI(TAG, "SLAC_PARAM.REQ from car: %s", mac_str);
    
    // Parse payload
    plc_cfg.application_type = payload[0];
    plc_cfg.security_type = payload[1];
    // RunID is at offset 2, length 8
    memcpy(plc_cfg.run_id, &payload[2], 8);
    
    plc_cfg.slac_attempts++;
    
    // Send SLAC_PARAM.CNF
    uint8_t resp[MAX_FRAME_SIZE];
    int resp_len = compose_slac_param_cnf(resp);
    
    if (write(plc_cfg.l2tap_fd, resp, resp_len) == resp_len) {
        plc_cfg.tx_count++;
        enter_slac_state(SLAC_STATE_WAIT_ATTEN_CHAR);
        plc_cfg.num_sounds_received = 0;
        memset(plc_cfg.atten_profile, 0, sizeof(plc_cfg.atten_profile));
    } else {
        ESP_LOGE(TAG, "Failed to send SLAC_PARAM.CNF: errno %d", errno);
        enter_slac_state(SLAC_STATE_ERROR);
    }
}

static void handle_mnbc_sound_ind(const uint8_t *frame, int len) {
    if (plc_cfg.slac_state != SLAC_STATE_WAIT_ATTEN_CHAR) {
        return;
    }
    
    plc_cfg.num_sounds_received++;
    ESP_LOGD(TAG, "MNBC_SOUND.IND received (%d/10)", plc_cfg.num_sounds_received);
    
    // After receiving sounds (or timeout), send ATTEN_CHAR.IND
    // In practice, we wait for all 10 sounds or use a timeout
    if (plc_cfg.num_sounds_received >= 10) {
        uint8_t resp[MAX_FRAME_SIZE];
        int resp_len = compose_atten_char_ind(resp);
        
        if (write(plc_cfg.l2tap_fd, resp, resp_len) == resp_len) {
            plc_cfg.tx_count++;
            enter_slac_state(SLAC_STATE_WAIT_MATCH_REQ);
        } else {
            ESP_LOGE(TAG, "Failed to send ATTEN_CHAR.IND");
            enter_slac_state(SLAC_STATE_ERROR);
        }
    }
}

static void handle_atten_char_rsp(const uint8_t *frame, int len) {
    // Car acknowledges our ATTEN_CHAR.IND
    ESP_LOGI(TAG, "ATTEN_CHAR.RSP received");
}

static void handle_slac_match_req(const uint8_t *frame, int len) {
    if (plc_cfg.slac_state != SLAC_STATE_WAIT_MATCH_REQ) {
        ESP_LOGW(TAG, "SLAC_MATCH.REQ in unexpected state");
        return;
    }
    
    ESP_LOGI(TAG, "SLAC_MATCH.REQ received - sending NID/NMK");
    
    // Send SLAC_MATCH.CNF with NID/NMK
    uint8_t resp[MAX_FRAME_SIZE];
    int resp_len = compose_slac_match_cnf(resp);
    
    if (write(plc_cfg.l2tap_fd, resp, resp_len) == resp_len) {
        plc_cfg.tx_count++;
        
        // Now configure our modem with the same NID/NMK
        uint8_t set_key[MAX_FRAME_SIZE];
        int set_key_len = compose_set_key_req(set_key);
        
        if (write(plc_cfg.l2tap_fd, set_key, set_key_len) == set_key_len) {
            ESP_LOGI(TAG, "SET_KEY sent to modem");
        }
        
        enter_slac_state(SLAC_STATE_MATCHED);
        plc_cfg.slac_success++;
        
        // Notify Python via callback
        if (plc_cfg.slac_callback != mp_const_none) {
            // Create MAC string
            char mac_str[18];
            mac_to_string(plc_cfg.car_mac, mac_str);
            bool scheduled = mp_sched_schedule(plc_cfg.slac_callback,
                                               mp_obj_new_str(mac_str, strlen(mac_str)));
            if (!scheduled) {
                ESP_LOGW(TAG, "Failed to schedule SLAC callback");
            }
        }
    } else {
        ESP_LOGE(TAG, "Failed to send SLAC_MATCH.CNF");
        enter_slac_state(SLAC_STATE_ERROR);
    }
}

static void handle_get_sw_cnf(const uint8_t *frame, int len) {
    // Parse software version response from modem
    // Payload starts after MME header
    const uint8_t *payload = frame + sizeof(mme_header_t);
    int payload_len = len - sizeof(mme_header_t);
    
    if (payload_len >= 6) {
        // Device ID starts at offset 0 (variable length string)
        ESP_LOGI(TAG, "Modem firmware: %.*s", payload_len > 64 ? 64 : payload_len, payload);
    }
}

// ============================================================================
// Frame Dispatch
// ============================================================================

static void process_frame(const uint8_t *frame, int len) {
    if (len < sizeof(mme_header_t)) {
        return;
    }
    
    const mme_header_t *hdr = (const mme_header_t *)frame;
    uint16_t mme_type = hdr->mme_type;
    uint16_t mme_base = mme_type & 0xFFFC;  // Mask off REQ/CNF/IND/RSP bits
    uint16_t mme_sub = mme_type & 0x0003;
    
    ESP_LOGD(TAG, "RX MME: type=0x%04X (base=0x%04X, sub=%d)", mme_type, mme_base, mme_sub);
    plc_cfg.rx_count++;
    
    switch (mme_base) {
        case CM_SLAC_PARAM:
            if (mme_sub == MMTYPE_REQ) {
                handle_slac_param_req(frame, len);
            }
            break;
            
        case CM_MNBC_SOUND:
            if (mme_sub == MMTYPE_IND) {
                handle_mnbc_sound_ind(frame, len);
            }
            break;
            
        case CM_ATTEN_CHAR:
            if (mme_sub == MMTYPE_RSP) {
                handle_atten_char_rsp(frame, len);
            }
            break;
            
        case CM_SLAC_MATCH:
            if (mme_sub == MMTYPE_REQ) {
                handle_slac_match_req(frame, len);
            }
            break;
            
        case CM_GET_SW:
            if (mme_sub == MMTYPE_CNF) {
                handle_get_sw_cnf(frame, len);
            }
            break;
            
        default:
            ESP_LOGD(TAG, "Unhandled MME type: 0x%04X", mme_type);
            break;
    }
}

// ============================================================================
// PLC Task
// ============================================================================

static void plc_task(void *arg) {
    uint8_t frame_buffer[MAX_FRAME_SIZE];
    
    ESP_LOGI(TAG, "PLC task started");
    
    // Wait for enable
    while (!plc_cfg.enabled) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    // Query modem firmware
    uint8_t get_sw[MAX_FRAME_SIZE];
    int get_sw_len = compose_get_sw_req(get_sw);
    if (write(plc_cfg.l2tap_fd, get_sw, get_sw_len) > 0) {
        plc_cfg.tx_count++;
        ESP_LOGI(TAG, "Sent GET_SW.REQ to modem");
    }
    
    enter_slac_state(SLAC_STATE_WAIT_PARAM_REQ);
    
    while (plc_cfg.enabled) {
        // Read frame with timeout
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(plc_cfg.l2tap_fd, &read_fds);
        
        struct timeval timeout = { .tv_sec = 0, .tv_usec = 100000 };  // 100ms
        
        int ret = select(plc_cfg.l2tap_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        if (ret > 0 && FD_ISSET(plc_cfg.l2tap_fd, &read_fds)) {
            int len = read(plc_cfg.l2tap_fd, frame_buffer, sizeof(frame_buffer));
            if (len > 0) {
                process_frame(frame_buffer, len);
            }
        }
        
        // Timeout handling for SLAC states
        uint32_t now = get_time_ms();
        uint32_t elapsed = now - plc_cfg.state_enter_time_ms;
        
        if (plc_cfg.slac_state == SLAC_STATE_WAIT_ATTEN_CHAR && elapsed > 2000) {
            // If we received some sounds but timeout before all 10, send ATTEN_CHAR anyway
            if (plc_cfg.num_sounds_received > 0) {
                ESP_LOGI(TAG, "Sound timeout - sending ATTEN_CHAR with %d sounds", 
                         plc_cfg.num_sounds_received);
                uint8_t resp[MAX_FRAME_SIZE];
                int resp_len = compose_atten_char_ind(resp);
                if (write(plc_cfg.l2tap_fd, resp, resp_len) == resp_len) {
                    plc_cfg.tx_count++;
                    enter_slac_state(SLAC_STATE_WAIT_MATCH_REQ);
                }
            }
        }
        
        if ((plc_cfg.slac_state == SLAC_STATE_WAIT_PARAM_REQ ||
             plc_cfg.slac_state == SLAC_STATE_WAIT_MATCH_REQ) && 
            elapsed > SLAC_TIMEOUT_MS) {
            ESP_LOGW(TAG, "SLAC timeout in state %s", slac_state_names[plc_cfg.slac_state]);
            enter_slac_state(SLAC_STATE_WAIT_PARAM_REQ);  // Reset for retry
        }
    }
    
    ESP_LOGI(TAG, "PLC task exiting");
    vTaskDelete(NULL);
}

// ============================================================================
// Public API - Start/Stop
// ============================================================================

static bool plc_start_evse_internal(void) {
    plc_init_mutex();
    
    if (plc_cfg.enabled) {
        ESP_LOGW(TAG, "PLC already running");
        return true;
    }
    
    // Open L2TAP
    plc_cfg.l2tap_fd = open("/dev/net/tap", O_RDWR | O_NONBLOCK);
    if (plc_cfg.l2tap_fd < 0) {
        ESP_LOGE(TAG, "Failed to open L2TAP: errno %d", errno);
        return false;
    }
    
    // Bind to Ethernet interface
    if (ioctl(plc_cfg.l2tap_fd, L2TAP_S_INTF, "ETH_DEF") < 0) {
        ESP_LOGE(TAG, "Failed to bind L2TAP to ETH_DEF: errno %d", errno);
        close(plc_cfg.l2tap_fd);
        plc_cfg.l2tap_fd = -1;
        return false;
    }
    
    // Set filter for HomePlug EtherType
    uint16_t ethertype = HOMEPLUG_ETHERTYPE;
    if (ioctl(plc_cfg.l2tap_fd, L2TAP_S_RCV_FILTER, &ethertype) < 0) {
        ESP_LOGE(TAG, "Failed to set EtherType filter: errno %d", errno);
        close(plc_cfg.l2tap_fd);
        plc_cfg.l2tap_fd = -1;
        return false;
    }
    
    // Get our MAC address from netif
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (netif) {
        esp_netif_get_mac(netif, plc_cfg.our_mac);
        char mac_str[18];
        mac_to_string(plc_cfg.our_mac, mac_str);
        ESP_LOGI(TAG, "Our MAC: %s", mac_str);
    } else {
        ESP_LOGW(TAG, "Could not get MAC from netif");
    }
    
    // Create event ringbuffer
    plc_cfg.event_ringbuf = xRingbufferCreate(PLC_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!plc_cfg.event_ringbuf) {
        ESP_LOGE(TAG, "Failed to create ringbuffer");
        close(plc_cfg.l2tap_fd);
        plc_cfg.l2tap_fd = -1;
        return false;
    }
    
    // Check NID/NMK set
    if (!plc_cfg.nid_nmk_set) {
        ESP_LOGW(TAG, "NID/NMK not set - use plc.set_key() first");
    }
    
    // Create task
    BaseType_t ret = xTaskCreate(plc_task, "plc_task", PLC_STACK_SIZE, 
                                  NULL, PLC_PRIORITY, &plc_cfg.task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create PLC task");
        vRingbufferDelete(plc_cfg.event_ringbuf);
        close(plc_cfg.l2tap_fd);
        plc_cfg.l2tap_fd = -1;
        return false;
    }
    
    plc_cfg.enabled = true;
    plc_cfg.slac_state = SLAC_STATE_IDLE;
    
    ESP_LOGI(TAG, "PLC EVSE mode started");
    return true;
}

static void plc_stop_internal(void) {
    if (!plc_cfg.enabled) {
        return;
    }
    
    plc_cfg.enabled = false;
    
    // Wait for task to exit
    if (plc_cfg.task_handle) {
        // Task will exit on next loop iteration
        vTaskDelay(pdMS_TO_TICKS(200));
        plc_cfg.task_handle = NULL;
    }
    
    // Clean up
    if (plc_cfg.l2tap_fd >= 0) {
        close(plc_cfg.l2tap_fd);
        plc_cfg.l2tap_fd = -1;
    }
    
    if (plc_cfg.event_ringbuf) {
        vRingbufferDelete(plc_cfg.event_ringbuf);
        plc_cfg.event_ringbuf = NULL;
    }
    
    plc_cfg.slac_state = SLAC_STATE_IDLE;
    ESP_LOGI(TAG, "PLC stopped");
}

// ============================================================================
// MicroPython Wrappers
// ============================================================================

// plc.start_evse()
static mp_obj_t plc_start_evse_wrapper(void) {
    bool result = plc_start_evse_internal();
    return mp_obj_new_bool(result);
}
static MP_DEFINE_CONST_FUN_OBJ_0(plc_start_evse_obj, plc_start_evse_wrapper);

// plc.stop()
static mp_obj_t plc_stop_wrapper(void) {
    plc_stop_internal();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(plc_stop_obj, plc_stop_wrapper);

// plc.set_callback(fn)
static mp_obj_t plc_set_callback_wrapper(mp_obj_t callback) {
    plc_cfg.slac_callback = callback;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(plc_set_callback_obj, plc_set_callback_wrapper);

// plc.set_key(nid, nmk)
static mp_obj_t plc_set_key_wrapper(mp_obj_t nid_obj, mp_obj_t nmk_obj) {
    mp_buffer_info_t nid_buf, nmk_buf;
    mp_get_buffer_raise(nid_obj, &nid_buf, MP_BUFFER_READ);
    mp_get_buffer_raise(nmk_obj, &nmk_buf, MP_BUFFER_READ);
    
    if (nid_buf.len != 7) {
        mp_raise_ValueError(MP_ERROR_TEXT("NID must be 7 bytes"));
    }
    if (nmk_buf.len != 16) {
        mp_raise_ValueError(MP_ERROR_TEXT("NMK must be 16 bytes"));
    }
    
    memcpy(plc_cfg.nid, nid_buf.buf, 7);
    memcpy(plc_cfg.nmk, nmk_buf.buf, 16);
    plc_cfg.nid_nmk_set = true;
    
    ESP_LOGI(TAG, "NID/NMK set");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(plc_set_key_obj, plc_set_key_wrapper);

// plc.get_status()
static mp_obj_t plc_get_status_wrapper(void) {
    mp_obj_t dict = mp_obj_new_dict(8);
    
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_enabled), 
                      mp_obj_new_bool(plc_cfg.enabled));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_state),
                      mp_obj_new_str(slac_state_names[plc_cfg.slac_state], 
                                     strlen(slac_state_names[plc_cfg.slac_state])));
    
    if (plc_cfg.car_mac_known) {
        char mac_str[18];
        mac_to_string(plc_cfg.car_mac, mac_str);
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_car_mac),
                          mp_obj_new_str(mac_str, strlen(mac_str)));
    }
    
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_rx_count),
                      mp_obj_new_int(plc_cfg.rx_count));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_tx_count),
                      mp_obj_new_int(plc_cfg.tx_count));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_slac_attempts),
                      mp_obj_new_int(plc_cfg.slac_attempts));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_slac_success),
                      mp_obj_new_int(plc_cfg.slac_success));
    
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(plc_get_status_obj, plc_get_status_wrapper);

// plc.get_modem_info()
static mp_obj_t plc_get_modem_info_wrapper(void) {
    if (plc_cfg.l2tap_fd < 0) {
        // Try temporary open to query modem
        int fd = open("/dev/net/tap", O_RDWR | O_NONBLOCK);
        if (fd < 0) {
            return mp_const_none;
        }
        
        if (ioctl(fd, L2TAP_S_INTF, "ETH_DEF") < 0 ||
            ioctl(fd, L2TAP_S_RCV_FILTER, (uint16_t[]){HOMEPLUG_ETHERTYPE}) < 0) {
            close(fd);
            return mp_const_none;
        }
        
        // Get MAC
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
        uint8_t mac[6];
        if (netif) {
            esp_netif_get_mac(netif, mac);
            memcpy(plc_cfg.our_mac, mac, 6);
        }
        
        // Send GET_SW.REQ
        uint8_t req[MAX_FRAME_SIZE];
        int req_len = compose_get_sw_req(req);
        write(fd, req, req_len);
        
        // Wait for response
        fd_set read_fds;
        struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);
        
        mp_obj_t result = mp_const_none;
        
        if (select(fd + 1, &read_fds, NULL, NULL, &timeout) > 0) {
            uint8_t resp[MAX_FRAME_SIZE];
            int len = read(fd, resp, sizeof(resp));
            if (len > sizeof(mme_header_t)) {
                const mme_header_t *hdr = (const mme_header_t *)resp;
                if ((hdr->mme_type & 0xFFFC) == CM_GET_SW) {
                    const char *payload = (const char *)(resp + sizeof(mme_header_t));
                    int payload_len = len - sizeof(mme_header_t);
                    
                    mp_obj_t dict = mp_obj_new_dict(2);
                    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_firmware),
                                      mp_obj_new_str(payload, payload_len > 64 ? 64 : payload_len));
                    
                    char mac_str[18];
                    mac_to_string(hdr->src_mac, mac_str);
                    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_mac),
                                      mp_obj_new_str(mac_str, strlen(mac_str)));
                    
                    result = dict;
                }
            }
        }
        
        close(fd);
        return result;
    }
    
    // If already running, return cached info
    mp_obj_t dict = mp_obj_new_dict(1);
    char mac_str[18];
    mac_to_string(plc_cfg.our_mac, mac_str);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_mac),
                      mp_obj_new_str(mac_str, strlen(mac_str)));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(plc_get_modem_info_obj, plc_get_modem_info_wrapper);

// ============================================================================
// EXI Codec Wrappers
// ============================================================================

// plc.exi_decode(data) -> dict
// Decode EXI data (with or without V2GTP header) to a dict
static mp_obj_t plc_exi_decode_wrapper(mp_obj_t data_obj) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(data_obj, &buf, MP_BUFFER_READ);
    
    const uint8_t *data = buf.buf;
    int len = buf.len;
    
    // Check for V2GTP header and strip if present
    const uint8_t *exi_data = data;
    int exi_len = len;
    
    if (len >= 8 && data[0] == 0x01 && data[1] == 0xFE) {
        // Has V2GTP header
        if (exi_remove_v2gtp_header(data, len, &exi_data, &exi_len) < 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("Invalid V2GTP header"));
        }
    }
    
    // Decode the message
    din_decoded_msg_t msg;
    if (exi_decode(exi_data, exi_len, &msg) < 0) {
        // Return dict with unknown type
        mp_obj_t dict = mp_obj_new_dict(1);
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_type),
                          mp_obj_new_str("Unknown", 7));
        return dict;
    }
    
    // Build result dict
    mp_obj_t dict = mp_obj_new_dict(8);
    
    // Message type
    const char *type_name = exi_msg_type_name(msg.type);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_type),
                      mp_obj_new_str(type_name, strlen(type_name)));
    
    // Add type-specific fields
    switch (msg.type) {
        case DIN_MSG_PRE_CHARGE_REQ:
            mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_EVTargetVoltage),
                              mp_obj_new_int(msg.ev_target_voltage));
            mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_EVTargetCurrent),
                              mp_obj_new_int(msg.ev_target_current));
            break;
            
        case DIN_MSG_CURRENT_DEMAND_REQ:
            mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_EVTargetVoltage),
                              mp_obj_new_int(msg.ev_target_voltage));
            mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_EVTargetCurrent),
                              mp_obj_new_int(msg.ev_target_current_demand));
            mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ChargingComplete),
                              mp_obj_new_bool(msg.charging_complete));
            break;
            
        case DIN_MSG_POWER_DELIVERY_REQ:
            mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ChargeProgress),
                              msg.charge_progress_start ? 
                                  mp_obj_new_str("Start", 5) : 
                                  mp_obj_new_str("Stop", 4));
            break;
            
        default:
            break;
    }
    
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_1(plc_exi_decode_obj, plc_exi_decode_wrapper);

// plc.exi_encode(msg_type, params) -> bytes
// Encode a DIN message response
static mp_obj_t plc_exi_encode_wrapper(mp_obj_t type_obj, mp_obj_t params_obj) {
    // Get message type string
    const char *type_str = mp_obj_str_get_str(type_obj);
    
    // Map string to enum
    din_msg_type_t msg_type = DIN_MSG_UNKNOWN;
    
    if (strcmp(type_str, "SupportedAppProtocolRes") == 0) {
        msg_type = DIN_MSG_SUPPORTED_APP_PROTOCOL_RES;
    } else if (strcmp(type_str, "SessionSetupRes") == 0) {
        msg_type = DIN_MSG_SESSION_SETUP_RES;
    } else if (strcmp(type_str, "ServiceDiscoveryRes") == 0) {
        msg_type = DIN_MSG_SERVICE_DISCOVERY_RES;
    } else if (strcmp(type_str, "ServicePaymentSelectionRes") == 0) {
        msg_type = DIN_MSG_SERVICE_PAYMENT_SELECTION_RES;
    } else if (strcmp(type_str, "ContractAuthenticationRes") == 0) {
        msg_type = DIN_MSG_CONTRACT_AUTHENTICATION_RES;
    } else if (strcmp(type_str, "ChargeParameterDiscoveryRes") == 0) {
        msg_type = DIN_MSG_CHARGE_PARAMETER_DISCOVERY_RES;
    } else if (strcmp(type_str, "CableCheckRes") == 0) {
        msg_type = DIN_MSG_CABLE_CHECK_RES;
    } else if (strcmp(type_str, "PreChargeRes") == 0) {
        msg_type = DIN_MSG_PRE_CHARGE_RES;
    } else if (strcmp(type_str, "PowerDeliveryRes") == 0) {
        msg_type = DIN_MSG_POWER_DELIVERY_RES;
    } else if (strcmp(type_str, "CurrentDemandRes") == 0) {
        msg_type = DIN_MSG_CURRENT_DEMAND_RES;
    } else if (strcmp(type_str, "WeldingDetectionRes") == 0) {
        msg_type = DIN_MSG_WELDING_DETECTION_RES;
    } else if (strcmp(type_str, "SessionStopRes") == 0) {
        msg_type = DIN_MSG_SESSION_STOP_RES;
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("Unknown message type"));
    }
    
    // Build encode params
    din_encode_params_t params;
    memset(&params, 0, sizeof(params));
    params.type = msg_type;
    
    // Parse optional params dict
    if (params_obj != mp_const_none && mp_obj_is_type(params_obj, &mp_type_dict)) {
        mp_obj_t voltage_key = MP_OBJ_NEW_QSTR(MP_QSTR_EVSEPresentVoltage);
        mp_obj_t voltage_val = mp_obj_dict_get(params_obj, voltage_key);
        if (voltage_val != MP_OBJ_NULL) {
            params.evse_present_voltage = mp_obj_get_int(voltage_val);
        }
        
        mp_obj_t current_key = MP_OBJ_NEW_QSTR(MP_QSTR_EVSEPresentCurrent);
        mp_obj_t current_val = mp_obj_dict_get(params_obj, current_key);
        if (current_val != MP_OBJ_NULL) {
            params.evse_present_current = mp_obj_get_int(current_val);
        }
        
        mp_obj_t finished_key = MP_OBJ_NEW_QSTR(MP_QSTR_Finished);
        mp_obj_t finished_val = mp_obj_dict_get(params_obj, finished_key);
        if (finished_val != MP_OBJ_NULL) {
            params.processing_finished = mp_obj_is_true(finished_val);
        }
    }
    
    // Encode the message with V2GTP header
    uint8_t buffer[256];
    int len = exi_encode_with_header(&params, buffer, sizeof(buffer));
    
    if (len < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("Encoding failed"));
    }
    
    return mp_obj_new_bytes(buffer, len);
}
static MP_DEFINE_CONST_FUN_OBJ_2(plc_exi_encode_obj, plc_exi_encode_wrapper);

// ============================================================================
// Module Definition
// ============================================================================

static const mp_rom_map_elem_t plc_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_plc) },
    
    // SLAC Functions
    { MP_ROM_QSTR(MP_QSTR_start_evse), MP_ROM_PTR(&plc_start_evse_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&plc_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_callback), MP_ROM_PTR(&plc_set_callback_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_key), MP_ROM_PTR(&plc_set_key_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_status), MP_ROM_PTR(&plc_get_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_modem_info), MP_ROM_PTR(&plc_get_modem_info_obj) },
    
    // EXI Functions
    { MP_ROM_QSTR(MP_QSTR_exi_decode), MP_ROM_PTR(&plc_exi_decode_obj) },
    { MP_ROM_QSTR(MP_QSTR_exi_encode), MP_ROM_PTR(&plc_exi_encode_obj) },
    
    // SLAC State Constants
    { MP_ROM_QSTR(MP_QSTR_STATE_IDLE), MP_ROM_INT(SLAC_STATE_IDLE) },
    { MP_ROM_QSTR(MP_QSTR_STATE_WAIT_PARAM), MP_ROM_INT(SLAC_STATE_WAIT_PARAM_REQ) },
    { MP_ROM_QSTR(MP_QSTR_STATE_WAIT_ATTEN), MP_ROM_INT(SLAC_STATE_WAIT_ATTEN_CHAR) },
    { MP_ROM_QSTR(MP_QSTR_STATE_WAIT_MATCH), MP_ROM_INT(SLAC_STATE_WAIT_MATCH_REQ) },
    { MP_ROM_QSTR(MP_QSTR_STATE_MATCHED), MP_ROM_INT(SLAC_STATE_MATCHED) },
    { MP_ROM_QSTR(MP_QSTR_STATE_ERROR), MP_ROM_INT(SLAC_STATE_ERROR) },
};
static MP_DEFINE_CONST_DICT(plc_module_globals, plc_module_globals_table);

const mp_obj_module_t mp_module_plc = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&plc_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_plc, mp_module_plc);
