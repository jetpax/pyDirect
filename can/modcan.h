/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Musumeci Salvatore
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
#ifndef MICROPY_INCLUDED_ESP32_MACHINE_CAN_H
#define MICROPY_INCLUDED_ESP32_MACHINE_CAN_H
/*
#include "modmachine.h"
#include "freertos/task.h"

#include "mpconfigport.h"
#include "py/obj.h"
*/
#include "driver/twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define CAN_MODE_SILENT_LOOPBACK (0x10)

typedef enum {
    MODE_NORMAL = TWAI_MODE_NORMAL,
    MODE_SLEEP = -1,
    MODE_LOOPBACK = -2, // TWAI_MODE_NORMAL | CAN_MODE_SILENT_LOOPBACK,
    MODE_SILENT = TWAI_MODE_NO_ACK,
    MODE_SILENT_LOOPBACK = -3,
    MODE_LISTEN_ONLY = TWAI_MODE_LISTEN_ONLY, // esp32 specific
} can_mode_t;

typedef enum {
    FILTER_RAW_SINGLE = 1,
    FILTER_RAW_DUAL,
    FILTER_ADDRESS
} filter_mode_t;

typedef enum {
    RX_STATE_FIFO_EMPTY = 1,
    RX_STATE_MESSAGE_PENDING,
    RX_STATE_FIFO_FULL,
    RX_STATE_FIFO_OVERFLOW,
} rx_state_t;

typedef enum {
    NOT_INITIATED = TWAI_STATE_STOPPED - 1,
    STOPPED = TWAI_STATE_STOPPED,
    RUNNING = TWAI_STATE_RUNNING,
    BUS_OFF = TWAI_STATE_BUS_OFF,
    RECOVERING = TWAI_STATE_RECOVERING,
} state_t;

typedef enum {
    ERROR = -1,
    /*
    ERROR_ACTIVE = TWAI_ERROR_ACTIVE,
    ERROR_WARNING = TWAI_ERROR_WARNING,
    ERROR_PASSIVE = TWAI_ERROR_PASSIVE,
    ERROR_BUS_OFF = TWAI_ERROR_BUS_OFF,
    */
} error_state_t;


typedef enum {
    RTR = 1,
    EXTENDED_ID,
    FD_F,
    BRS,
} message_flags_t;

typedef enum {
    CRC = 1,
    FORM,
    OVERRUN,
    ESI,
} recv_errors_t;

typedef enum {
    ARB = 1,
    NACK,
    ERR,
} send_errors_t;

typedef struct {
    twai_timing_config_t timing;
    twai_filter_config_t filter;
    twai_general_config_t general;
    uint32_t bitrate; // bit/s
    bool initialized;
} esp32_can_config_t;

// ============================================================================
// CAN Manager API (for multi-client CAN bus management)
// ============================================================================

typedef enum {
    CAN_CLIENT_MODE_RX_ONLY,      // RX only, contributes to LISTEN_ONLY requirement
    CAN_CLIENT_MODE_TX_ENABLED,   // TX capable, contributes to NORMAL requirement
} can_client_mode_t;

// RX callback function type (must be defined before can_client struct)
typedef void (*can_rx_callback_t)(const twai_message_t *frame, void *arg);

// Forward declaration
typedef struct can_client can_client_t;

// Client structure
struct can_client {
    uint32_t client_id;
    bool is_registered;
    bool is_activated;
    can_client_mode_t mode;
    can_rx_callback_t rx_callback;
    void *rx_callback_arg;
    can_client_t *next;
    volatile int refcount;         // Reference count for safe callback execution
    volatile bool pending_delete;  // Mark for deletion when refcount reaches 0
};

// Opaque handle type for CAN clients (actually a pointer to can_client_t)
typedef can_client_t* can_handle_t;

typedef struct {
    mp_obj_base_t base;
    esp32_can_config_t *config;
    mp_obj_t rx_callback;
    mp_obj_t tx_callback;
    TaskHandle_t irq_handler;
    byte rx_state;
    bool extframe : 1;
    bool loopback : 1;
    byte last_tx_success : 1;
    byte bus_recovery_success : 1;
    uint16_t num_error_warning; // FIXME: populate this value somewhere
    uint16_t num_error_passive;
    uint16_t num_bus_off;
    twai_handle_t handle;
    twai_status_info_t status;
    // CAN Manager fields
    can_client_t *clients;  // Linked list of registered clients
    can_client_t *pending_free_clients;  // Clients pending deferred free
    uint32_t registered_clients;
    uint32_t activated_clients;
    uint32_t activated_transmitting_clients;
    QueueHandle_t tx_queue;  // Shared TX queue
    QueueHandle_t rx_queue;  // RX queue for frames from manager callback
    TaskHandle_t rx_dispatcher_task;  // RX dispatcher task
    TaskHandle_t tx_task_handle;  // TX queue task
    uint32_t next_client_id;  // Incrementing client ID counter
    volatile bool rx_dispatcher_should_stop;  // Signal to RX dispatcher to stop
} esp32_can_obj_t;

extern const mp_obj_type_t machine_can_type;

// Get TWAI handle from CAN module singleton (for use by other C modules like GVRET)
twai_handle_t esp32_can_get_handle(void);

// Get TWAI mode from CAN module singleton (for use by other C modules like GVRET)
twai_mode_t esp32_can_get_mode(void);

// CAN Manager API Functions
can_handle_t can_register(can_client_mode_t mode);
esp_err_t can_activate(can_handle_t h);
esp_err_t can_deactivate(can_handle_t h);
void can_unregister(can_handle_t h);
void can_set_rx_callback(can_handle_t h, can_rx_callback_t cb, void *arg);
esp_err_t can_add_filter(can_handle_t h, uint32_t id, uint32_t mask);
esp_err_t can_set_mode(can_handle_t h, can_client_mode_t mode);
esp_err_t can_transmit(can_handle_t h, const twai_message_t *msg);
bool can_is_registered(can_handle_t h);
void can_set_loopback(bool enabled);  // Set loopback mode (for testing)

// Python CAN Manager API
// Python callbacks are stored in the client's `arg` field and passed directly to
// can_set_rx_callback(). The MicroPython GC will keep the callback alive as long
// as the client is registered and the callback is referenced.

#endif // MICROPY_INCLUDED_ESP32_MACHINE_CAN_H
