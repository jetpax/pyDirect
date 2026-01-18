/* The MIT License (MIT)
 *
 * Copyright (c) 2019 Musumeci Salvatore
 * Copyright (c) 2021 Ihor Nehrutsa
 * Copyright (c) 2022 Yuriy Makarov
 * Copyright (c) 2026 Jonathan Peace
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
// #include <string.h>

#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "mpconfigport.h"
#include "py/obj.h"
#include "py/objarray.h"
#include "py/binary.h"
#include "py/runtime.h"
#include "py/builtin.h"
#include "py/mphal.h"
#include "py/mperrno.h"
#include "mpconfigport.h"
#include "freertos/task.h"
#include "esp_idf_version.h"
#include "py/stackctrl.h"
#if MICROPY_PY_THREAD
#include "py/mpthread.h"
#endif

#include "esp_err.h"
#include "esp_log.h"
#include "soc/soc_caps.h"
#include "soc/clk_tree_defs.h"
#include "soc/twai_periph.h"
#include "hal/twai_types.h"
#include "hal/twai_hal.h"
#include "driver/twai.h"
#include "esp_task.h"
#include "modcan.h"

// Logger tag
static const char *TAG = "TWAI";

// Include for clock source definitions
#if CONFIG_IDF_TARGET_ESP32
    // Original ESP32 doesn't need additional clock source includes
#else
    // For ESP32-C3/S2/S3 etc, include clock definitions if available
    #if __has_include("hal/clk_tree_hal.h")
        #include "hal/clk_tree_hal.h"
    #endif
    #if __has_include("soc/clk_tree_defs.h")
        #include "soc/clk_tree_defs.h"
    #endif
#endif

// Fallback definitions for missing constants
#ifndef TWAI_CLK_SRC_DEFAULT
    // Define a fallback if TWAI_CLK_SRC_DEFAULT is not available
    #if defined(TWAI_CLK_SRC_APB)
        #define TWAI_CLK_SRC_DEFAULT TWAI_CLK_SRC_APB
    #elif defined(SOC_MOD_CLK_APB)
        #define TWAI_CLK_SRC_DEFAULT SOC_MOD_CLK_APB
    #else
        #define TWAI_CLK_SRC_DEFAULT 0
    #endif
#endif


// Default bitrate: 500kb
#define CAN_TASK_PRIORITY           (ESP_TASK_PRIO_MIN + 1)
#define CAN_TASK_STACK_SIZE         (4096)  // Increased from 1024 to prevent stack overflow
#define CAN_DEFAULT_PRESCALER (8)
#define CAN_DEFAULT_SJW (3)
#define CAN_DEFAULT_BS1 (15)
#define CAN_DEFAULT_BS2 (4)
#define CAN_MAX_DATA_FRAME          (8)


// Добавьте перед строкой 472 (примерно)
extern BaseType_t xTaskCreatePinnedToCore(TaskFunction_t pxTaskCode,
    const char * const pcName,
    const uint32_t usStackDepth,
    void * const pvParameters,
    UBaseType_t uxPriority,
    TaskHandle_t * const pxCreatedTask,
    const BaseType_t xCoreID);

static void update_bus_state(void);

static const twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

// Universal timing configuration for all ESP32 variants
// Handles structure differences between ESP32 generations

// Universal function to get timing configuration for any ESP32 variant
static inline twai_timing_config_t get_timing_config(uint32_t bitrate) {
#if CONFIG_IDF_TARGET_ESP32
    // For original ESP32: use manual timing calculations based on 40MHz APB clock
    // Bitrate = XTAL_FREQ / (BRP * (1 + tseg_1 + tseg_2))
    switch (bitrate) {
        case 1000:    return (twai_timing_config_t){.brp = 2000, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false}; // 1kbps
        case 5000:    return (twai_timing_config_t){.brp = 400, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false};  // 5kbps
        case 10000:   return (twai_timing_config_t){.brp = 200, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false}; // 10kbps
        case 12500:   return (twai_timing_config_t){.brp = 160, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false}; // 12.5kbps
        case 16000:   return (twai_timing_config_t){.brp = 125, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false}; // 16kbps
        case 20000:   return (twai_timing_config_t){.brp = 100, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false}; // 20kbps
        case 25000:   return (twai_timing_config_t){.brp = 80, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false};  // 25kbps
        case 50000:   return (twai_timing_config_t){.brp = 40, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false}; // 50kbps
        case 100000:  return (twai_timing_config_t){.brp = 20, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false}; // 100kbps
        case 125000:  return (twai_timing_config_t){.brp = 16, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false}; // 125kbps
        case 250000:  return (twai_timing_config_t){.brp = 8, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false};  // 250kbps
        case 500000:  return (twai_timing_config_t){.brp = 4, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false};  // 500kbps
        case 800000:  return (twai_timing_config_t){.brp = 2, .tseg_1 = 23, .tseg_2 = 6, .sjw = 3, .triple_sampling = false}; // 800kbps
        case 1000000: return (twai_timing_config_t){.brp = 2, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false}; // 1Mbps
        default:      return (twai_timing_config_t){.brp = 4, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false}; // Default to 500k
    }
#else
    // For ESP32-C3/S2/S3: use ESP-IDF macros with proper compound literal syntax
    switch (bitrate) {
        case 1000:    { twai_timing_config_t timing = TWAI_TIMING_CONFIG_1KBITS(); return timing; }
        case 5000:    { twai_timing_config_t timing = TWAI_TIMING_CONFIG_5KBITS(); return timing; }
        case 10000:   { twai_timing_config_t timing = TWAI_TIMING_CONFIG_10KBITS(); return timing; }
        case 12500:   { twai_timing_config_t timing = TWAI_TIMING_CONFIG_12_5KBITS(); return timing; }
        case 16000:   { twai_timing_config_t timing = TWAI_TIMING_CONFIG_16KBITS(); return timing; }
        case 20000:   { twai_timing_config_t timing = TWAI_TIMING_CONFIG_20KBITS(); return timing; }
        case 25000:   { twai_timing_config_t timing = TWAI_TIMING_CONFIG_25KBITS(); return timing; }
        case 50000:   { twai_timing_config_t timing = TWAI_TIMING_CONFIG_50KBITS(); return timing; }
        case 100000:  { twai_timing_config_t timing = TWAI_TIMING_CONFIG_100KBITS(); return timing; }
        case 125000:  { twai_timing_config_t timing = TWAI_TIMING_CONFIG_125KBITS(); return timing; }
        case 250000:  { twai_timing_config_t timing = TWAI_TIMING_CONFIG_250KBITS(); return timing; }
        case 500000:  { twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS(); return timing; }
        case 800000:  { twai_timing_config_t timing = TWAI_TIMING_CONFIG_800KBITS(); return timing; }
        case 1000000: { twai_timing_config_t timing = TWAI_TIMING_CONFIG_1MBITS(); return timing; }
        default:      { twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS(); return timing; } // Default to 500k
    }
#endif
}

// singleton CAN device object
esp32_can_config_t can_config = {
    .general = TWAI_GENERAL_CONFIG_DEFAULT(GPIO_NUM_2, GPIO_NUM_4, TWAI_MODE_NORMAL),
    .filter = TWAI_FILTER_CONFIG_ACCEPT_ALL(),
    .timing = {0}, // Will be initialized properly during first use via get_timing_config()
    .initialized = false
};

static esp32_can_obj_t esp32_can_obj = {
    {&machine_can_type},
    .config = &can_config,
    .handle = NULL,
    .clients = NULL,
    .pending_free_clients = NULL,
    .registered_clients = 0,
    .activated_clients = 0,
    .activated_transmitting_clients = 0,
    .tx_queue = NULL,
    .rx_queue = NULL,
    .rx_dispatcher_task = NULL,
    .tx_task_handle = NULL,
    .next_client_id = 1,
    .rx_dispatcher_should_stop = false,
};

// CAN module's own client handle (for participating in manager)
static can_handle_t can_module_client_handle = NULL;

// RX callback for CAN module - receives frames from manager and enqueues them
static void can_module_rx_callback(const twai_message_t *message, void *arg) {
    esp32_can_obj_t *self = (esp32_can_obj_t *)arg;
    
    // Safety checks - must be fast and safe (called from RX dispatcher task)
    if (message == NULL || self == NULL) {
        return;
    }
    
    // Check if device is initialized (config should be valid)
    // This check prevents use-after-free if can_deinit() is called while callback is executing
    if (self->config == NULL || !self->config->initialized) {
        // Device is being deinitialized or not initialized - drop frame silently
        return;
    }
    
    // Store local copy of queue pointer (queue might be deleted by can_deinit during callback)
    QueueHandle_t rx_queue = self->rx_queue;
    if (rx_queue == NULL) {
        // Queue not created yet - this can happen if callback is called before ensure_can_activated() completes
        // Drop frame silently (shouldn't happen in normal operation)
        return;
    }
    
    // Enqueue frame for recv() to consume
    BaseType_t queue_result = xQueueSend(rx_queue, message, 0);
    if (queue_result != pdTRUE) {
        // Queue full - drop frame (user should increase queue size or read faster)
        ESP_LOGW(TAG, "can_module_rx_callback: RX queue full, dropping frame");
        return;  // Don't trigger callback if queue is full
    }
    
    // Trigger MicroPython IRQ callback if set (for irq_recv())
    // Re-check self->rx_callback and self->config->initialized (might have changed during queue send)
    if (self->config != NULL && self->config->initialized && 
        self->rx_callback != mp_const_none && self->rx_callback != NULL) {
        // Check queue size to determine callback parameter
        UBaseType_t queue_messages = uxQueueMessagesWaiting(rx_queue);
        int callback_param = 0;  // First message
        // Re-check config is still valid before accessing rx_queue_len
        if (self->config != NULL && queue_messages >= self->config->general.rx_queue_len) {
            callback_param = 1;  // Queue full
        }
        // Schedule callback with saturation detection
        static int sched_fail_count = 0;
        bool scheduled = mp_sched_schedule(self->rx_callback, MP_OBJ_NEW_SMALL_INT(callback_param));
        if (!scheduled) {
            // Scheduler queue full - track consecutive failures
            sched_fail_count++;
            if (sched_fail_count == 1 || sched_fail_count == 10 || sched_fail_count % 100 == 0) {
                ESP_LOGW(TAG, "can_module_rx_callback: Scheduler saturation detected (fail count: %d)", sched_fail_count);
            }
            // TODO: Consider adaptive backoff if saturation persists
        } else {
            // Reset failure counter on success
            if (sched_fail_count > 0) {
                ESP_LOGD(TAG, "can_module_rx_callback: Scheduler recovered (was %d failures)", sched_fail_count);
                sched_fail_count = 0;
            }
        }
    }
}

// Get TWAI handle from CAN module singleton (for use by other C modules like GVRET)
twai_handle_t esp32_can_get_handle(void) {
    if (esp32_can_obj.config->initialized && esp32_can_obj.handle != NULL) {
        ESP_LOGD(TAG, "esp32_can_get_handle: returning handle %p", (void*)esp32_can_obj.handle);
        return esp32_can_obj.handle;
    }
    ESP_LOGW(TAG, "esp32_can_get_handle: CAN not initialized, returning NULL");
    return NULL;
}

twai_mode_t esp32_can_get_mode(void) {
    if (esp32_can_obj.config->initialized) {
        return esp32_can_obj.config->general.mode;
    }
    return TWAI_MODE_NORMAL; // Default if not initialized
}

// INTERNAL Deinitialize can
void can_deinit(esp32_can_obj_t *self) {
    ESP_LOGI(TAG, "can_deinit: starting deinitialization");
    
    // Clear MicroPython IRQ callbacks
    self->rx_callback = mp_const_none;
    self->tx_callback = mp_const_none;
    
    // Clear manager RX callback before deactivating
    if (can_module_client_handle != NULL) {
        can_set_rx_callback(can_module_client_handle, NULL, NULL);
    }
    
    // Deactivate and unregister CAN module client
    if (can_module_client_handle != NULL) {
        can_deactivate(can_module_client_handle);
        can_unregister(can_module_client_handle);
        can_module_client_handle = NULL;
        ESP_LOGI(TAG, "can_deinit: CAN module unregistered from manager");
    }
    
    // Clean up RX queue
    if (self->rx_queue != NULL) {
        vQueueDelete(self->rx_queue);
        self->rx_queue = NULL;
        ESP_LOGD(TAG, "can_deinit: RX queue deleted");
    }
    
    // Manager's update_bus_state() will handle driver stop/uninstall
    // But we still need to clean up IRQ task
    if (self->irq_handler != NULL) {
        ESP_LOGD(TAG, "can_deinit: deleting IRQ task");
        vTaskDelete(self->irq_handler);
        self->irq_handler = NULL;
    }
    self->handle = NULL;
    self->config->initialized = false;
    ESP_LOGD(TAG, "can_deinit: deinitialization complete");
}

static void esp32_can_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    esp32_can_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (self->config->initialized) {
        qstr mode;
        switch (self->config->general.mode) {
            case TWAI_MODE_LISTEN_ONLY:
                mode = MP_QSTR_LISTEN;
                break;
            case TWAI_MODE_NO_ACK:
                mode = MP_QSTR_NO_ACK;
                break;
            case TWAI_MODE_NORMAL:
                mode = MP_QSTR_NORMAL;
                break;
            default:
                mode = MP_QSTR_UNKNOWN;
                break;
        }
        mp_printf(print, "CAN(tx=%u, rx=%u, bitrate=%u, mode=%q, loopback=%u, extframe=%u)",
            self->config->general.tx_io,
            self->config->general.rx_io,
            self->config->bitrate,
            mode,
            self->loopback,
            self->extframe);
    } else {
        mp_printf(print, "Device is not initialized");
    }
}

// INTERNAL FUNCTION FreeRTOS IRQ task
static void esp32_can_irq_task(void *self_in) {
    esp32_can_obj_t *self = MP_OBJ_TO_PTR(self_in);
    uint32_t alerts;

    ESP_LOGD(TAG, "irq_task: starting IRQ task");
    check_esp_err(twai_reconfigure_alerts_v2(self->handle, TWAI_ALERT_ALL,
        // TWAI_ALERT_RX_DATA | TWAI_ALERT_RX_QUEUE_FULL | TWAI_ALERT_BUS_OFF | TWAI_ALERT_ERR_PASS |
        // TWAI_ALERT_ABOVE_ERR_WARN | TWAI_ALERT_TX_FAILED | TWAI_ALERT_TX_SUCCESS | TWAI_ALERT_BUS_RECOVERED,
        // TWAI_ALERT_TX_IDLE | TWAI_ALERT_BELOW_ERR_WARN | TWAI_ALERT_ERR_ACTIVE | TWAI_ALERT_RECOVERY_IN_PROGRESS |
        // TWAI_ALERT_ARB_LOST | TWAI_ALERT_BUS_ERROR | TWAI_ALERT_RX_FIFO_OVERRUN | TWAI_ALERT_TX_RETRIED | TWAI_ALERT_PERIPH_RESET,
        NULL
        ));

    while (1) {
        check_esp_err(twai_read_alerts_v2(self->handle, &alerts, portMAX_DELAY));

        if (alerts & TWAI_ALERT_BUS_OFF) {
            ESP_LOGE(TAG, "irq_task: BUS_OFF alert detected");
            ++self->num_bus_off;
            ESP_LOGD(TAG, "irq_task: waiting 3s before recovery");
            for (int i = 3; i > 0; i--) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            ESP_LOGD(TAG, "irq_task: initiating recovery");
            twai_initiate_recovery_v2(self->handle); // Needs 128 occurrences of bus free signal
        }
        if (alerts & TWAI_ALERT_ERR_PASS) {
            ESP_LOGW(TAG, "irq_task: ERROR_PASSIVE alert");
            ++self->num_error_passive;
        }
        if (alerts & TWAI_ALERT_ABOVE_ERR_WARN) {
            ESP_LOGW(TAG, "irq_task: ERROR_WARNING alert");
            ++self->num_error_warning;
        }
        if (alerts & (TWAI_ALERT_BUS_RECOVERED)) {
            ESP_LOGI(TAG, "irq_task: BUS_RECOVERED alert");
            self->bus_recovery_success = 1;
        }

        if (alerts & (TWAI_ALERT_TX_FAILED | TWAI_ALERT_TX_SUCCESS)) {
            bool success = (alerts & TWAI_ALERT_TX_SUCCESS) > 0;
            ESP_LOGD(TAG, "irq_task: TX %s", success ? "SUCCESS" : "FAILED");
            self->last_tx_success = success;
        }

        if (self->tx_callback != mp_const_none) {
            check_esp_err(twai_get_status_info_v2(self->handle, &self->status));
            if (alerts & TWAI_ALERT_TX_IDLE) {
                mp_sched_schedule(self->tx_callback, MP_OBJ_NEW_SMALL_INT(0));
            }
            if (alerts & TWAI_ALERT_TX_SUCCESS) {
                mp_sched_schedule(self->tx_callback, MP_OBJ_NEW_SMALL_INT(1));
            }
            if (alerts & TWAI_ALERT_TX_FAILED) {
                mp_sched_schedule(self->tx_callback, MP_OBJ_NEW_SMALL_INT(2));
            }
            if (alerts & TWAI_ALERT_TX_RETRIED) {
                mp_sched_schedule(self->tx_callback, MP_OBJ_NEW_SMALL_INT(3));
            }
        }

        if (self->rx_callback != mp_const_none) {
            if (alerts & TWAI_ALERT_RX_DATA) {
                check_esp_err(twai_get_status_info_v2(self->handle, &self->status));
                uint32_t msgs_to_rx = self->status.msgs_to_rx;

                if (msgs_to_rx == 1) {
                    // first message in queue
                    mp_sched_schedule(self->rx_callback, MP_OBJ_NEW_SMALL_INT(0));
                } else if (msgs_to_rx >= self->config->general.rx_queue_len) {
                    // queue is full
                    mp_sched_schedule(self->rx_callback, MP_OBJ_NEW_SMALL_INT(1));
                }
            }
            if (alerts & TWAI_ALERT_RX_QUEUE_FULL) {
                // queue overflow
                mp_sched_schedule(self->rx_callback, MP_OBJ_NEW_SMALL_INT(2));
            }
            if (alerts & TWAI_ALERT_RX_FIFO_OVERRUN) {
                mp_sched_schedule(self->rx_callback, MP_OBJ_NEW_SMALL_INT(3));
            }
        }
    }
}

static mp_obj_t esp32_can_init_helper(esp32_can_obj_t *self, size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    // Set CAN component log level to DEBUG at runtime
    // This ensures DEBUG logs are visible even if component-level log level is higher
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
    
    enum { ARG_mode, ARG_prescaler, ARG_sjw, ARG_bs1, ARG_bs2, ARG_auto_restart, ARG_bitrate, ARG_extframe,
           ARG_tx_io, ARG_rx_io, ARG_clkout_io, ARG_bus_off_io, ARG_tx_queue, ARG_rx_queue};
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_mode, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = TWAI_MODE_NORMAL} },
        { MP_QSTR_prescaler, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = CAN_DEFAULT_PRESCALER} },
        { MP_QSTR_sjw, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = CAN_DEFAULT_SJW} },
        { MP_QSTR_bs1, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = CAN_DEFAULT_BS1} },
        { MP_QSTR_bs2, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = CAN_DEFAULT_BS2} },
        { MP_QSTR_auto_restart, MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_bitrate, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 500000} },
        { MP_QSTR_extframe, MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_tx, MP_ARG_INT, {.u_int = 4} },
        { MP_QSTR_rx, MP_ARG_INT, {.u_int = 5} },
        { MP_QSTR_clkout, MP_ARG_INT, {.u_int = TWAI_IO_UNUSED} },
        { MP_QSTR_bus_off, MP_ARG_INT, {.u_int = TWAI_IO_UNUSED} },
        { MP_QSTR_tx_queue, MP_ARG_INT, {.u_int = 1} },
        { MP_QSTR_rx_queue, MP_ARG_INT, {.u_int = 1} },
    };

    // parse args
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // Configure device
    // self->config->general.mode = args[ARG_mode].u_int & 0x0F;
    self->config->general.tx_io = args[ARG_tx_io].u_int;
    self->config->general.rx_io = args[ARG_rx_io].u_int;
    self->config->general.clkout_io = args[ARG_clkout_io].u_int;
    self->config->general.bus_off_io = args[ARG_bus_off_io].u_int;
    self->config->general.tx_queue_len = args[ARG_tx_queue].u_int;
    self->config->general.rx_queue_len = args[ARG_rx_queue].u_int;
    self->config->general.alerts_enabled = TWAI_ALERT_ALL;
    // TWAI_ALERT_AND_LOG || TWAI_ALERT_BELOW_ERR_WARN || TWAI_ALERT_ERR_ACTIVE || TWAI_ALERT_BUS_RECOVERED ||
    // TWAI_ALERT_ABOVE_ERR_WARN || TWAI_ALERT_BUS_ERROR || TWAI_ALERT_ERR_PASS || TWAI_ALERT_BUS_OFF;
    
    self->config->general.clkout_divider = 0;


    // Configure device - detect loopback mode
    // MODE_LOOPBACK = -2, MODE_SILENT_LOOPBACK = -3, CAN_MODE_SILENT_LOOPBACK = 0x10 (flag)
    int mode_int = (int)args[ARG_mode].u_int;
    self->loopback = (mode_int == MODE_LOOPBACK || mode_int == MODE_SILENT_LOOPBACK ||
                      ((args[ARG_mode].u_int & CAN_MODE_SILENT_LOOPBACK) > 0));

    // If loopback mode is set, use TWAI_MODE_NO_ACK as in the official example
    // TWAI_MODE_NO_ACK allows transmission without requiring ACK from other nodes
    if (self->loopback) {
        self->config->general.mode = TWAI_MODE_NO_ACK;
        ESP_LOGI(TAG, "init_helper: Loopback mode enabled (TWAI_MODE_NO_ACK)");
    } else {
        self->config->general.mode = args[ARG_mode].u_int & 0x0F;
    }


    self->extframe = args[ARG_extframe].u_bool;
    if (args[ARG_auto_restart].u_bool) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("Auto-restart not supported"));
    }
    self->config->filter = f_config; // TWAI_FILTER_CONFIG_ACCEPT_ALL(); //

    // clear errors
    self->num_error_warning = 0;
    self->num_error_passive = 0;
    self->num_bus_off = 0;

    // Calculate CAN nominal bit timing from bitrate if provided
    self->config->bitrate = args[ARG_bitrate].u_int;
    
    if (self->config->bitrate == 0) {
        // Manual configuration with custom parameters
        self->config->timing = (twai_timing_config_t) {
            .brp = args[ARG_prescaler].u_int,
            .sjw = args[ARG_sjw].u_int,
            .tseg_1 = args[ARG_bs1].u_int,
            .tseg_2 = args[ARG_bs2].u_int,
            .triple_sampling = false
        };
    } else {
        // Use universal timing configuration for all ESP32 variants
        self->config->timing = get_timing_config(self->config->bitrate);
    }

    // Always initialize singleton timing if not done yet (for first use)
    if (can_config.timing.brp == 0) {
        can_config.timing = get_timing_config(500000); // Default 500k timing for singleton
    }

    // Log timing configuration to serial console (not REPL)
#if !CONFIG_IDF_TARGET_ESP32
    // ESP-IDF 5.x uses quanta_resolution_hz instead of brp for ESP32-C3/S2/S3
    ESP_LOGD(TAG, "timing: quanta_resolution_hz=%lu, brp=%lu, tseg_1=%u, tseg_2=%u, sjw=%u",
             (unsigned long)self->config->timing.quanta_resolution_hz,
             (unsigned long)self->config->timing.brp,
             self->config->timing.tseg_1, self->config->timing.tseg_2, self->config->timing.sjw);
#else
    ESP_LOGD(TAG, "timing: brp=%lu, tseg_1=%u, tseg_2=%u, sjw=%u",
             (unsigned long)self->config->timing.brp,
             self->config->timing.tseg_1, self->config->timing.tseg_2, self->config->timing.sjw);
#endif
    ESP_LOGI(TAG, "init: bitrate=%lu, mode=%u, loopback=%d",
             (unsigned long)self->config->bitrate, self->config->general.mode, self->loopback);

    ESP_LOGI(TAG, "init_helper: starting initialization");
    ESP_LOGD(TAG, "init_helper: tx=%d, rx=%d, bitrate=%lu, mode=%d, loopback=%d",
             (int)self->config->general.tx_io, (int)self->config->general.rx_io,
             (unsigned long)self->config->bitrate, (int)self->config->general.mode, (int)self->loopback);

    // Ensure handle is NULL (should be NULL after deinit - manager manages the driver)
    if (self->handle != NULL) {
        // Driver handle still set - this shouldn't happen after deinit
        // Clear it (manager will set it when client activates)
        ESP_LOGW(TAG, "init_helper: Handle not NULL before init (%p), clearing it", (void*)self->handle);
        self->handle = NULL;
    }

    // Register CAN module itself with manager (if not already registered)
    // Manager will handle driver installation/start when client activates
    if (can_module_client_handle == NULL) {
        // Determine mode based on requested mode
        can_client_mode_t client_mode = CAN_CLIENT_MODE_TX_ENABLED;
        if (self->config->general.mode == TWAI_MODE_LISTEN_ONLY) {
            client_mode = CAN_CLIENT_MODE_RX_ONLY;
        }
        
        // Apply loopback setting to global object (for manager's update_bus_state())
        // This ensures manager API respects loopback mode even if instance API isn't used
        esp32_can_obj.loopback = self->loopback;
        ESP_LOGI(TAG, "init_helper: Loopback mode = %d (from instance config)", (int)self->loopback);
        
        can_module_client_handle = can_register(client_mode);
        if (can_module_client_handle == NULL) {
            ESP_LOGE(TAG, "init_helper: Failed to register CAN module with manager");
            mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to register CAN module"));
            return mp_const_none;
        }
        
        // RX callback will be set by ensure_can_activated() when module is activated
        // This allows recv() to receive frames via manager callback system
        
        ESP_LOGI(TAG, "init_helper: CAN module registered with manager");
    }
    
    // Note: CAN module is registered but NOT activated yet
    // It will activate lazily when send(), recv(), or irq() is called
    // This allows GVRET to control when the bus activates (when SavvyCAN connects)
    
    ESP_LOGI(TAG, "init_helper: CAN module registered (not activated - will activate on first use)");
    
    self->config->initialized = true;
    ESP_LOGD(TAG, "init_helper: initialization complete");
    return mp_const_none;
}

// CAN(bus, ...) No argument to get the object
// If no arguments are provided, the initialized object will be returned
static mp_obj_t esp32_can_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    // check arguments
    mp_arg_check_num(n_args, n_kw, 1, MP_OBJ_FUN_ARGS_MAX, true);
    if (mp_obj_is_int(args[0]) != true) {
        mp_raise_TypeError(MP_ERROR_TEXT("bus must be a number"));
    }

    // work out port
    mp_uint_t can_idx = mp_obj_get_int(args[0]);
    if (can_idx > SOC_TWAI_CONTROLLER_NUM - 1) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("out of CAN controllers:%d"), SOC_TWAI_CONTROLLER_NUM);
    }

    esp32_can_obj_t *self = &esp32_can_obj;
    ESP_LOGD(TAG, "make_new: bus=%lu, n_args=%zu, n_kw=%zu, initialized=%d", 
             (unsigned long)can_idx, n_args, n_kw, self->config->initialized);
    
    self->status.state = STOPPED;
    if (!self->config->initialized || n_args > 1 || n_kw > 0) {
        if (self->config->initialized) {
            // The caller is requesting a reconfiguration of the hardware
            // this can only be done if the hardware is in init mode
            ESP_LOGD(TAG, "make_new: reconfiguration requested, deinitializing first");
            can_deinit(self);
        }
        self->tx_callback = mp_const_none;
        self->rx_callback = mp_const_none;
        self->irq_handler = NULL;
        self->rx_state = RX_STATE_FIFO_EMPTY;

        if (n_args > 1 || n_kw > 0) {
            // start the peripheral
            mp_map_t kw_args;
            mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);
            esp32_can_init_helper(self, n_args - 1, args + 1, &kw_args);
        }
    }
    return MP_OBJ_FROM_PTR(self);
}

// init(tx, rx, bitrate, mode=NORMAL, tx_queue=2, rx_queue=5)
static mp_obj_t esp32_can_init(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    esp32_can_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    if (self->config->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Device is already initialized"));
        return mp_const_none;
    }
    return esp32_can_init_helper(self, n_args - 1, pos_args + 1, kw_args);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esp32_can_init_obj, 4, esp32_can_init);

// deinit()
static mp_obj_t esp32_can_deinit(const mp_obj_t self_in) {
    esp32_can_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->config->initialized != true) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Device is not initialized"));
        return mp_const_none;
    }
    can_deinit(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esp32_can_deinit_obj, esp32_can_deinit);

// CAN.restart()
// Force a software restart of the controller, to allow transmission after a bus error
static mp_obj_t esp32_can_restart(mp_obj_t self_in) {
    esp32_can_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_esp_err(twai_get_status_info_v2(self->handle, &self->status));

    if (!self->config->initialized || self->status.state != TWAI_STATE_BUS_OFF) {
        return mp_const_none;
        // mp_raise_ValueError(NULL);
    }

    self->bus_recovery_success = -1;
    check_esp_err(twai_initiate_recovery_v2(self->handle));
    mp_hal_delay_ms(200); // FIXME: replace it with a smarter solution

    while (self->bus_recovery_success < 0) {
        MICROPY_EVENT_POLL_HOOK
    }

    if (self->bus_recovery_success) {
        check_esp_err(twai_start_v2(self->handle));
    } else {
        mp_raise_OSError(MP_EIO);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esp32_can_restart_obj, esp32_can_restart);

// Get the state of the controller
static mp_obj_t esp32_can_state(mp_obj_t self_in) {
    esp32_can_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->config->initialized) {
        check_esp_err(twai_get_status_info_v2(self->handle, &self->status));
    }
    return mp_obj_new_int(self->status.state);
}
static MP_DEFINE_CONST_FUN_OBJ_1(esp32_can_state_obj, esp32_can_state);

// info() -- Get info about error states and TX/RX buffers
static mp_obj_t esp32_can_info(size_t n_args, const mp_obj_t *args) {
    esp32_can_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    check_esp_err(twai_get_status_info_v2(self->handle, &self->status));
    mp_obj_t dict = mp_obj_new_dict(0);
    #define dict_key(key) mp_obj_new_str(#key, strlen(#key))
    #define dict_value(key) MP_OBJ_NEW_SMALL_INT(self->status.key)
    #define dict_store(key) mp_obj_dict_store(dict, dict_key(key), dict_value(key));
    dict_store(state);
    dict_store(msgs_to_tx);
    dict_store(msgs_to_rx);
    dict_store(tx_error_counter);
    dict_store(rx_error_counter);
    dict_store(tx_failed_count);
    dict_store(rx_missed_count);
    dict_store(arb_lost_count);
    dict_store(bus_error_count);
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esp32_can_info_obj, 1, 2, esp32_can_info);

// Get Alert info
static mp_obj_t esp32_can_alert(mp_obj_t self_in) {
    const esp32_can_obj_t *self = MP_OBJ_TO_PTR(self_in);
    uint32_t alerts;
    check_esp_err(twai_read_alerts_v2(self->handle, &alerts, 0));
    return mp_obj_new_int(alerts);
}
static MP_DEFINE_CONST_FUN_OBJ_1(esp32_can_alert_obj, esp32_can_alert);

// any() - return `True` if any message waiting, else `False`
static mp_obj_t esp32_can_any(mp_obj_t self_in) {
    esp32_can_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    // Check RX queue (frames from manager callback)
    if (self->rx_queue != NULL) {
        UBaseType_t queue_messages = uxQueueMessagesWaiting(self->rx_queue);
        if (queue_messages > 0) {
            return mp_const_true;
        }
    }
    
    // Fallback: check driver status if handle exists (for backward compatibility)
    if (self->handle != NULL) {
        check_esp_err(twai_get_status_info_v2(self->handle, &self->status));
        return mp_obj_new_bool((self->status.msgs_to_rx) > 0);
    }
    
    return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esp32_can_any_obj, esp32_can_any);

// send([data], id, *, timeout=0, rtr=false, extframe=false)
// CAN.send(identifier, data, flags=0, fifo_equal=True)
// Helper function for lazy activation of CAN module
static void ensure_can_activated(esp32_can_obj_t *self) {
    if (can_module_client_handle != NULL && self->handle == NULL) {
        // Create RX queue if not already created (for frames from manager callback)
        if (self->rx_queue == NULL) {
            // Use same queue size as RX queue configured in CAN init
            uint32_t queue_size = self->config->general.rx_queue_len;
            if (queue_size == 0) {
                queue_size = 20;  // Default if not set
            }
            self->rx_queue = xQueueCreate(queue_size, sizeof(twai_message_t));
            if (self->rx_queue == NULL) {
                mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to create RX queue"));
                return;
            }
            ESP_LOGD(TAG, "ensure_can_activated: Created RX queue (size=%lu)", (unsigned long)queue_size);
        }
        
        // Set RX callback to receive frames from manager
        can_set_rx_callback(can_module_client_handle, can_module_rx_callback, self);
        
        esp_err_t ret_activate = can_activate(can_module_client_handle);
        if (ret_activate != ESP_OK) {
            mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to activate CAN module"));
            return;
        }
        // Wait for manager to install/start driver
        int retries = 0;
        while (self->handle == NULL && retries < 50) {
            vTaskDelay(pdMS_TO_TICKS(10));
            retries++;
        }
        if (self->handle == NULL) {
            mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("CAN driver not installed by manager"));
            return;
        }
        // Create IRQ task if not already created
        if (self->irq_handler == NULL) {
            if (xTaskCreatePinnedToCore(esp32_can_irq_task, "can_irq_task", CAN_TASK_STACK_SIZE, self, CAN_TASK_PRIORITY, (TaskHandle_t *)&self->irq_handler, MP_TASK_COREID) != pdPASS) {
                mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("failed to create can irq task handler"));
                return;
            }
        }
    }
}

static mp_obj_t esp32_can_send(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_data, ARG_id, ARG_timeout, ARG_rtr, ARG_extframe };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_data,     MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_id,       MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_timeout,  MP_ARG_KW_ONLY | MP_ARG_INT,  {.u_int = 0} },
        { MP_QSTR_rtr,      MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_extframe, MP_ARG_BOOL,                  {.u_bool = false} },
    };

    // parse args
    esp32_can_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    
    // Lazy activation: activate CAN module if not already activated
    ensure_can_activated(self);

    // populate message
    twai_message_t tx_msg;

    size_t length;
    mp_obj_t *items;
    mp_obj_get_array(args[ARG_data].u_obj, &length, &items);
    if (length > CAN_MAX_DATA_FRAME) {
        mp_raise_ValueError(MP_ERROR_TEXT("CAN data field too long"));
    }
    tx_msg.data_length_code = length;
    tx_msg.flags = (args[ARG_rtr].u_bool ? TWAI_MSG_FLAG_RTR : TWAI_MSG_FLAG_NONE);

    if (args[ARG_extframe].u_bool) {
        tx_msg.identifier = args[ARG_id].u_int & 0x1FFFFFFF;
        tx_msg.flags |= TWAI_MSG_FLAG_EXTD;
    } else {
        tx_msg.identifier = args[ARG_id].u_int & 0x7FF;
    }
    if (self->loopback) {
        tx_msg.flags |= TWAI_MSG_FLAG_SELF;
    }

    for (uint8_t i = 0; i < length; i++) {
        tx_msg.data[i] = mp_obj_get_int(items[i]);
    }

    check_esp_err(twai_get_status_info_v2(self->handle, &self->status));
    ESP_LOGD(TAG, "send: current state=%d, ID=0x%lx, DLC=%d", self->status.state, (unsigned long)tx_msg.identifier, tx_msg.data_length_code);
    
    if (self->status.state == TWAI_STATE_RUNNING) {
        uint32_t timeout_ms = args[ARG_timeout].u_int;
   
        if (timeout_ms != 0) {
            self->last_tx_success = -1;
            uint32_t start = mp_hal_ticks_us();
            ESP_LOGD(TAG, "send: transmitting with timeout=%u ms", (unsigned int)timeout_ms);
            esp_err_t ret = twai_transmit_v2(self->handle, &tx_msg, pdMS_TO_TICKS(timeout_ms));
            
            // Check if transmit returned timeout - check BUS_OFF state before raising error
            if (ret == ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "send: transmit timeout, checking bus state");
                check_esp_err(twai_get_status_info_v2(self->handle, &self->status));
                ESP_LOGD(TAG, "send: timeout state=%d", self->status.state);
                if (self->status.state == TWAI_STATE_BUS_OFF) {
                    ESP_LOGE(TAG, "send: BUS_OFF detected after timeout");
                    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("CAN bus is BUS_OFF - check transceiver and termination"));
                }
                ESP_LOGW(TAG, "send: timeout without BUS_OFF");
                mp_raise_OSError(MP_ETIMEDOUT);
            }
            // Check for other errors
            check_esp_err(ret);
            ESP_LOGD(TAG, "send: transmit queued, waiting for completion");
            
            while (self->last_tx_success < 0) {
                // Check if bus went BUS_OFF during transmission
                check_esp_err(twai_get_status_info_v2(self->handle, &self->status));
                if (self->status.state == TWAI_STATE_BUS_OFF) {
                    ESP_LOGE(TAG, "send: BUS_OFF detected during wait");
                    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("CAN bus is BUS_OFF - check transceiver and termination"));
                }
                
                if (timeout_ms != portMAX_DELAY) {
                    if (mp_hal_ticks_us() - start >= timeout_ms) {
                        // Check state one more time before timing out
                        ESP_LOGW(TAG, "send: timeout in wait loop");
                        check_esp_err(twai_get_status_info_v2(self->handle, &self->status));
                        ESP_LOGD(TAG, "send: final state check=%d", self->status.state);
                        if (self->status.state == TWAI_STATE_BUS_OFF) {
                            ESP_LOGE(TAG, "send: BUS_OFF detected at final check");
                            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("CAN bus is BUS_OFF - check transceiver and termination"));
                        }
                        mp_raise_OSError(MP_ETIMEDOUT);
                    }
                }
                MICROPY_EVENT_POLL_HOOK
            }

            ESP_LOGD(TAG, "send: tx_success=%d", self->last_tx_success);
            if (!self->last_tx_success) {
                // Check if failure was due to BUS_OFF
                ESP_LOGW(TAG, "send: transmission failed, checking state");
                check_esp_err(twai_get_status_info_v2(self->handle, &self->status));
                ESP_LOGD(TAG, "send: failure state=%d", self->status.state);
                if (self->status.state == TWAI_STATE_BUS_OFF) {
                    ESP_LOGE(TAG, "send: BUS_OFF detected after failure");
                    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("CAN bus is BUS_OFF - check transceiver and termination"));
                }
                mp_raise_OSError(MP_EIO);
            }
            ESP_LOGD(TAG, "send: transmission successful");
        } else {
            // IMPORTANT: Never use portMAX_DELAY for transmit - it can block forever
            // if no ACK is received (e.g., no transceiver, physical loopback only).
            // Use a reasonable default timeout to prevent system lockup.
            ESP_LOGD(TAG, "send: transmitting with default timeout (1000ms)");
            esp_err_t tx_ret = twai_transmit_v2(self->handle, &tx_msg, pdMS_TO_TICKS(1000));
            
            if (tx_ret == ESP_ERR_TIMEOUT) {
                // Check if bus went to error state
                ESP_LOGW(TAG, "send: transmit timeout (no timeout specified), checking bus state");
                check_esp_err(twai_get_status_info_v2(self->handle, &self->status));
                ESP_LOGD(TAG, "send: timeout state=%d, tx_err=%lu, arb_lost=%lu", 
                         self->status.state, 
                         (unsigned long)self->status.tx_error_counter,
                         (unsigned long)self->status.arb_lost_count);
                         
                if (self->status.state == TWAI_STATE_BUS_OFF) {
                    ESP_LOGE(TAG, "send: BUS_OFF detected - no ACK on bus");
                    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("CAN bus is BUS_OFF - check transceiver, termination, or use LOOPBACK mode"));
                }
                // Provide helpful error message for physical loopback without transceiver
                if (self->status.tx_error_counter > 0) {
                    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("CAN TX timeout - no ACK received. Use LOOPBACK mode for testing without transceiver"));
                }
                mp_raise_OSError(MP_ETIMEDOUT);
            }
            check_esp_err(tx_ret);
            ESP_LOGD(TAG, "send: transmission queued");
        }

        return mp_const_none;
    } else if (self->status.state == TWAI_STATE_BUS_OFF) {
        ESP_LOGE(TAG, "send: bus is BUS_OFF, cannot send");
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("CAN bus is BUS_OFF - use restart() to recover"));
    } else {
        ESP_LOGE(TAG, "send: device not ready, state=%d", self->status.state);
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Device is not ready"));
    }
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esp32_can_send_obj, 3, esp32_can_send);

// CAN.recv(list=None, *, timeout=5000)
static mp_obj_t esp32_can_recv(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    esp32_can_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    enum { ARG_list, ARG_timeout };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_list, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 5000} },
    };

    // parse args
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    
    // Lazy activation: activate CAN module if not already activated
    ensure_can_activated(self);

    // Receive frame from manager's RX queue (instead of reading directly from driver)
    // This allows frames to be duplicated to all clients (GVRET, MP CAN, etc.)
    twai_message_t rx_msg;
    TickType_t timeout_ticks = pdMS_TO_TICKS(args[ARG_timeout].u_int);
    
    if (self->rx_queue == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("RX queue not initialized"));
        return mp_const_none;
    }
    
    if (xQueueReceive(self->rx_queue, &rx_msg, timeout_ticks) != pdTRUE) {
        // Timeout - raise OSError
        mp_raise_OSError(MP_ETIMEDOUT);
        return mp_const_none;
    }
    
    uint32_t rx_dlc = rx_msg.data_length_code;

    // Create the tuple, or get the list, that will hold the return values
    // Also populate the fourth element, either a new bytes or reuse existing memoryview
    mp_obj_t ret_obj = args[ARG_list].u_obj;
    mp_obj_t *items;
    if (ret_obj == mp_const_none) {
        ret_obj = mp_obj_new_tuple(4, NULL);
        items = ((mp_obj_tuple_t *)MP_OBJ_TO_PTR(ret_obj))->items;
        items[3] = mp_obj_new_bytes(rx_msg.data, rx_dlc);
    } else {
        // User should provide a list of length at least 4 to hold the values
        if (!mp_obj_is_type(ret_obj, &mp_type_list)) {
            mp_raise_TypeError(NULL);
        }
        mp_obj_list_t *list = MP_OBJ_TO_PTR(ret_obj);
        if (list->len < 4) {
            mp_raise_ValueError(NULL);
        }
        items = list->items;
        // Fourth element must be a memoryview which we assume points to a
        // byte-like array which is large enough, and then we resize it inplace
        if (!mp_obj_is_type(items[3], &mp_type_memoryview)) {
            mp_raise_TypeError(NULL);
        }
        mp_obj_array_t *mv = MP_OBJ_TO_PTR(items[3]);
        if (!(mv->typecode == (MP_OBJ_ARRAY_TYPECODE_FLAG_RW | BYTEARRAY_TYPECODE) || (mv->typecode | 0x20) == (MP_OBJ_ARRAY_TYPECODE_FLAG_RW | 'b'))) {
            mp_raise_ValueError(NULL);
        }
        mv->len = rx_dlc;
        memcpy(mv->items, rx_msg.data, rx_dlc);
    }
    items[0] = MP_OBJ_NEW_SMALL_INT(rx_msg.identifier);
    items[1] = rx_msg.extd ? mp_const_true : mp_const_false;
    items[2] = rx_msg.rtr ? mp_const_true : mp_const_false;

    // Return the result
    return ret_obj;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esp32_can_recv_obj, 1, esp32_can_recv);

// Clear filters setting
static mp_obj_t esp32_can_clearfilter(mp_obj_t self_in) {
    esp32_can_obj_t *self = MP_OBJ_TO_PTR(self_in);

    // Defaults from TWAI_FILTER_CONFIG_ACCEPT_ALL
    self->config->filter = f_config; // TWAI_FILTER_CONFIG_ACCEPT_ALL(); //

    // Apply filter
    check_esp_err(twai_stop_v2(self->handle));
    check_esp_err(twai_driver_uninstall_v2(self->handle));
    check_esp_err(twai_driver_install_v2(
        &self->config->general,
        &self->config->timing,
        &self->config->filter, &self->handle));
    check_esp_err(twai_start_v2(self->handle));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esp32_can_clearfilter_obj, esp32_can_clearfilter);

// bank: 0 only
// mode: FILTER_RAW_SINGLE, FILTER_RAW_DUAL or FILTER_ADDR_SINGLE or FILTER_ADDR_DUAL
// params: [id, mask]
// rtr: ignored if FILTER_RAW
// Set CAN HW filter
// CAN.set_filters(filters)
static mp_obj_t esp32_can_set_filters(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_bank, ARG_mode, ARG_params, ARG_rtr, ARG_extframe };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_bank,     MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_mode,     MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_params,   MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_rtr,      MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_bool = false} },
        { MP_QSTR_extframe, MP_ARG_BOOL,                  {.u_bool = false} },
    };

    // parse args
    esp32_can_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    const int can_idx = args[ARG_bank].u_int;

    if (can_idx != 0) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("Bank (%d) doesn't exist"), can_idx);
    }

    size_t len;
    mp_obj_t *params;
    mp_obj_get_array(args[ARG_params].u_obj, &len, &params);
    const int mode = args[ARG_mode].u_int;

    uint32_t id = mp_obj_get_int(params[0]);
    uint32_t mask = mp_obj_get_int(params[1]); // FIXME: Overflow in case 0xFFFFFFFF for mask
    if (mode == FILTER_RAW_SINGLE || mode == FILTER_RAW_DUAL) {
        if (len != 2) {
            mp_raise_ValueError(MP_ERROR_TEXT("params must be a 2-values list"));
        }
        self->config->filter.single_filter = (mode == FILTER_RAW_SINGLE);
        self->config->filter.acceptance_code = id;
        self->config->filter.acceptance_mask = mask;
    } else {
        self->config->filter.single_filter = self->extframe;
        // esp32_can_set_filters(self, id, mask, args[ARG_bank].u_int, args[ARG_rtr].u_int);
        // Check if bank is allowed
        int bank = 0;
        if (bank > ((self->extframe && self->config->filter.single_filter) ? 0 : 1)) {
            mp_raise_ValueError(MP_ERROR_TEXT("CAN filter parameter error"));
        }
        uint32_t preserve_mask;
        int addr = 0;
        int rtr = 0;
        if (self->extframe) {
            addr = (addr & 0x1FFFFFFF) << 3 | (rtr ? 0x04 : 0);
            mask = (mask & 0x1FFFFFFF) << 3 | 0x03;
            preserve_mask = 0;
        } else if (self->config->filter.single_filter) {
            addr = (((addr & 0x7FF) << 5) | (rtr ? 0x10 : 0));
            mask = ((mask & 0x7FF) << 5);
            mask |= 0xFFFFF000;
            preserve_mask = 0;
        } else {
            addr = (((addr & 0x7FF) << 5) | (rtr ? 0x10 : 0));
            mask = ((mask & 0x7FF) << 5);
            preserve_mask = 0xFFFF << (bank == 0 ? 16 : 0);
            if ((self->config->filter.acceptance_mask & preserve_mask) == (0xFFFF << (bank == 0 ? 16 : 0))) {
                // Other filter accepts all; it will replaced duplicating current filter
                addr = addr | (addr << 16);
                mask = mask | (mask << 16);
                preserve_mask = 0;
            } else {
                addr = addr << (bank == 1 ? 16 : 0);
                mask = mask << (bank == 1 ? 16 : 0);
            }
        }
        self->config->filter.acceptance_code &= preserve_mask;
        self->config->filter.acceptance_code |= addr;
        self->config->filter.acceptance_mask &= preserve_mask;
        self->config->filter.acceptance_mask |= mask;
    }
    // Apply filter
    if (self->handle) {
        check_esp_err(twai_stop_v2(self->handle));
        check_esp_err(twai_driver_uninstall_v2(self->handle));
    }
    check_esp_err(twai_driver_install_v2(
        &self->config->general,
        &self->config->timing,
        &self->config->filter,
        &self->handle
        ));
    check_esp_err(twai_start_v2(self->handle));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esp32_can_set_filters_obj, 1, esp32_can_set_filters);

// CAN.irq_recv(callback, hard=False)
static mp_obj_t esp32_can_irq_recv(mp_obj_t self_in, mp_obj_t callback_in) {
    esp32_can_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    // Lazy activation: activate CAN module if callback is being set (not disabled)
    if (callback_in != mp_const_none && mp_obj_is_callable(callback_in)) {
        ensure_can_activated(self);
    }
    
    if (callback_in == mp_const_none) {
        // disable callback
        self->rx_callback = mp_const_none;
    } else if (mp_obj_is_callable(callback_in)) {
        // set up interrupt
        self->rx_callback = callback_in;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(esp32_can_irq_recv_obj, esp32_can_irq_recv);

// CAN.irq_send(callback, hard=False)
static mp_obj_t esp32_can_irq_send(mp_obj_t self_in, mp_obj_t callback_in) {
    esp32_can_obj_t *self = MP_OBJ_TO_PTR(self_in);
    // mp_obj_t callback_in = NULL;
    if (callback_in == mp_const_none) {
        // disable callback
        self->tx_callback = mp_const_none;
    } else if (mp_obj_is_callable(callback_in)) {
        // set up interrupt
        self->tx_callback = callback_in;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(esp32_can_irq_send_obj, esp32_can_irq_send);

// CAN.get_state()
static mp_obj_t esp32_can_get_state(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esp32_can_get_state_obj, 3, esp32_can_get_state);

// CAN.get_counters([list] /)
static mp_obj_t esp32_can_get_counters(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esp32_can_get_counters_obj, 3, esp32_can_get_counters);

// CAN.get_timings([list])
static mp_obj_t esp32_can_get_timings(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esp32_can_get_timings_obj, 3, esp32_can_get_timings);

// CAN.reset(mode=CAN.Mode.NORMAL)
static mp_obj_t esp32_can_reset(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esp32_can_reset_obj, 3, esp32_can_reset);

// CAN.mode([mode])
static mp_obj_t esp32_can_mode(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esp32_can_mode_obj, 3, esp32_can_mode);

// Clear TX Queue
static mp_obj_t esp32_can_clear_tx_queue(mp_obj_t self_in) {
    const esp32_can_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(twai_clear_transmit_queue_v2(self->handle) == ESP_OK);
}
static MP_DEFINE_CONST_FUN_OBJ_1(esp32_can_clear_tx_queue_obj, esp32_can_clear_tx_queue);

// Clear RX Queue
static mp_obj_t esp32_can_clear_rx_queue(mp_obj_t self_in) {
    const esp32_can_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(twai_clear_receive_queue_v2(self->handle) == ESP_OK);
}
static MP_DEFINE_CONST_FUN_OBJ_1(esp32_can_clear_rx_queue_obj, esp32_can_clear_rx_queue);

// ============================================================================
// CAN Manager Python API - Module-Level Functions
// ============================================================================

// Python wrapper for can_register()
// Usage: handle = CAN.register(mode)
// Returns: handle (integer) or raises exception
static mp_obj_t mp_can_register(mp_obj_t mode_obj) {
    // Convert mode argument to integer (mp_obj_get_int handles type checking)
    mp_int_t mode_int = mp_obj_get_int(mode_obj);
    
    can_handle_t handle = can_register((can_client_mode_t)mode_int);
    if (handle == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to register CAN client"));
    }
    // Return handle as integer (pointer cast)
    return mp_obj_new_int((mp_int_t)handle);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_can_register_fun_obj, mp_can_register);
static MP_DEFINE_CONST_STATICMETHOD_OBJ(mp_can_register_obj, MP_ROM_PTR(&mp_can_register_fun_obj));

// Python wrapper for can_activate()
// Usage: CAN.activate(handle)
// Returns: None or raises exception
static mp_obj_t mp_can_activate(mp_obj_t handle_obj) {
    can_handle_t handle = (can_handle_t)mp_obj_get_int(handle_obj);
    esp_err_t ret = can_activate(handle);
    if (ret != ESP_OK) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to activate CAN client"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_can_activate_fun_obj, mp_can_activate);
static MP_DEFINE_CONST_STATICMETHOD_OBJ(mp_can_activate_obj, MP_ROM_PTR(&mp_can_activate_fun_obj));

// Python wrapper for can_deactivate()
// Usage: CAN.deactivate(handle)
// Returns: None or raises exception
static mp_obj_t mp_can_deactivate(mp_obj_t handle_obj) {
    can_handle_t handle = (can_handle_t)mp_obj_get_int(handle_obj);
    esp_err_t ret = can_deactivate(handle);
    if (ret != ESP_OK) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to deactivate CAN client"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_can_deactivate_fun_obj, mp_can_deactivate);
static MP_DEFINE_CONST_STATICMETHOD_OBJ(mp_can_deactivate_obj, MP_ROM_PTR(&mp_can_deactivate_fun_obj));

// Python wrapper for can_unregister()
// Usage: CAN.unregister(handle)
// Returns: None
static mp_obj_t mp_can_unregister(mp_obj_t handle_obj) {
    can_handle_t handle = (can_handle_t)mp_obj_get_int(handle_obj);
    can_unregister(handle);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_can_unregister_fun_obj, mp_can_unregister);
static MP_DEFINE_CONST_STATICMETHOD_OBJ(mp_can_unregister_obj, MP_ROM_PTR(&mp_can_unregister_fun_obj));

// RX frame queue item (raw data, no Python objects)
typedef struct {
    twai_message_t msg;
    mp_obj_t callback;  // Stored callback (already a Python object, just passing pointer)
} can_rx_queue_item_t;

// Queue for RX frames (populated by background task, processed by main task)
static QueueHandle_t can_rx_queue = NULL;

// Scheduled processor function (runs in MicroPython main task)
// This is called via mp_sched_schedule() and processes ALL queued frames
static mp_obj_t can_process_rx_queue(mp_obj_t unused) {
    can_rx_queue_item_t item;
    
    // Process all queued frames
    while (can_rx_queue != NULL && xQueueReceive(can_rx_queue, &item, 0) == pdTRUE) {
        if (item.callback == mp_const_none || item.callback == NULL) {
            continue;
        }
        
        // NOW it's safe to create Python objects (we're in the main task)
        mp_obj_t dict = mp_obj_new_dict(4);
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_id), mp_obj_new_int(item.msg.identifier));
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_extended), mp_obj_new_bool(item.msg.extd));
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_rtr), mp_obj_new_bool(item.msg.rtr));
        
        mp_obj_t data = mp_obj_new_bytes(item.msg.data, item.msg.data_length_code);
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_data), data);
        
        // Call Python callback
        mp_call_function_1(item.callback, dict);
    }
    
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(can_process_rx_queue_obj, can_process_rx_queue);

// RX callback wrapper - called by C manager from background task
// Note: C callback signature is (const twai_message_t *frame, void *arg)
static void mp_can_rx_callback_wrapper(const twai_message_t *msg, void *arg) {
    mp_obj_t callback = (mp_obj_t)arg;
    if (callback == mp_const_none || callback == NULL) {
        return;
    }
    
    // Create queue if needed
    if (can_rx_queue == NULL) {
        can_rx_queue = xQueueCreate(32, sizeof(can_rx_queue_item_t));
        if (can_rx_queue == NULL) {
            return; // Out of memory
        }
    }
    
    // Queue the raw frame data (NO Python object allocation here!)
    can_rx_queue_item_t item = {
        .msg = *msg,
        .callback = callback
    };
    
    if (xQueueSend(can_rx_queue, &item, 0) == pdTRUE) {
        // Schedule processing in main task
        mp_sched_schedule(MP_OBJ_FROM_PTR(&can_process_rx_queue_obj), mp_const_none);
    }
}

// Python wrapper for can_set_rx_callback()
// Usage: CAN.set_rx_callback(handle, callback)
// callback receives: {'id': int, 'data': bytes, 'extended': bool, 'rtr': bool}
static mp_obj_t mp_can_set_rx_callback(mp_obj_t handle_obj, mp_obj_t callback_obj) {
    can_handle_t handle = (can_handle_t)mp_obj_get_int(handle_obj);
    
    // Store Python callback (NULL if callback is None)
    void *arg = (callback_obj == mp_const_none) ? NULL : (void *)callback_obj;
    
    can_set_rx_callback(handle, mp_can_rx_callback_wrapper, arg);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mp_can_set_rx_callback_fun_obj, mp_can_set_rx_callback);
static MP_DEFINE_CONST_STATICMETHOD_OBJ(mp_can_set_rx_callback_obj, MP_ROM_PTR(&mp_can_set_rx_callback_fun_obj));

// Python wrapper for can_transmit()
// Usage: CAN.transmit(handle, frame_dict)
// frame_dict: {'id': int, 'data': bytes, 'extended': bool (optional), 'rtr': bool (optional)}
// Returns: None or raises exception
static mp_obj_t mp_can_transmit(mp_obj_t handle_obj, mp_obj_t frame_obj) {
    can_handle_t handle = (can_handle_t)mp_obj_get_int(handle_obj);
    
    // Parse frame dict (required fields)
    mp_obj_t id_obj = mp_obj_dict_get(frame_obj, MP_OBJ_NEW_QSTR(MP_QSTR_id));
    mp_obj_t data_obj = mp_obj_dict_get(frame_obj, MP_OBJ_NEW_QSTR(MP_QSTR_data));
    
    // Optional fields - use lookup with exception handling
    mp_obj_t extended_obj = mp_const_false;
    mp_obj_t rtr_obj = mp_const_false;
    
    mp_obj_dict_t *dict = MP_OBJ_TO_PTR(frame_obj);
    mp_map_elem_t *elem;
    
    elem = mp_map_lookup(&dict->map, MP_OBJ_NEW_QSTR(MP_QSTR_extended), MP_MAP_LOOKUP);
    if (elem != NULL) {
        extended_obj = elem->value;
    }
    
    elem = mp_map_lookup(&dict->map, MP_OBJ_NEW_QSTR(MP_QSTR_rtr), MP_MAP_LOOKUP);
    if (elem != NULL) {
        rtr_obj = elem->value;
    }
    
    // Build TWAI message
    twai_message_t msg = {0};
    msg.identifier = mp_obj_get_int(id_obj);
    msg.extd = mp_obj_is_true(extended_obj);
    msg.rtr = mp_obj_is_true(rtr_obj);
    
    // Set self-reception flag if loopback is enabled globally
    if (esp32_can_obj.loopback) {
        msg.self = 1;
    }
    
    // Allow overriding flags via dictionary
    elem = mp_map_lookup(&dict->map, MP_OBJ_NEW_QSTR(MP_QSTR_self), MP_MAP_LOOKUP);
    if (elem != NULL) {
        msg.self = mp_obj_is_true(elem->value);
    }
    
    elem = mp_map_lookup(&dict->map, MP_OBJ_NEW_QSTR(MP_QSTR_ss), MP_MAP_LOOKUP);
    if (elem != NULL) {
        msg.ss = mp_obj_is_true(elem->value);
    }
    
    // Get data bytes
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_obj, &bufinfo, MP_BUFFER_READ);
    msg.data_length_code = bufinfo.len > 8 ? 8 : bufinfo.len;
    memcpy(msg.data, bufinfo.buf, msg.data_length_code);
    
    // Transmit
    esp_err_t ret = can_transmit(handle, &msg);
    if (ret != ESP_OK) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to transmit CAN frame"));
    }
    
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mp_can_transmit_fun_obj, mp_can_transmit);
static MP_DEFINE_CONST_STATICMETHOD_OBJ(mp_can_transmit_obj, MP_ROM_PTR(&mp_can_transmit_fun_obj));

// Python wrapper for can_set_loopback()
// Usage: CAN.set_loopback(True/False)
// Returns: None
static mp_obj_t mp_can_set_loopback(mp_obj_t enabled_obj) {
    bool enabled = mp_obj_is_true(enabled_obj);
    can_set_loopback(enabled);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_can_set_loopback_fun_obj, mp_can_set_loopback);
static MP_DEFINE_CONST_STATICMETHOD_OBJ(mp_can_set_loopback_obj, MP_ROM_PTR(&mp_can_set_loopback_fun_obj));


static const mp_rom_map_elem_t esp32_can_locals_dict_table[] = {
    // CAN_ATTRIBUTES
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_CAN) },
    // Micropython Generic API
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&esp32_can_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&esp32_can_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_restart), MP_ROM_PTR(&esp32_can_restart_obj) },
    { MP_ROM_QSTR(MP_QSTR_state), MP_ROM_PTR(&esp32_can_state_obj) },
    { MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&esp32_can_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_any), MP_ROM_PTR(&esp32_can_any_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&esp32_can_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_recv), MP_ROM_PTR(&esp32_can_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_irq_send), MP_ROM_PTR(&esp32_can_irq_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_irq_recv), MP_ROM_PTR(&esp32_can_irq_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_filters), MP_ROM_PTR(&esp32_can_set_filters_obj) },
    { MP_ROM_QSTR(MP_QSTR_clearfilter), MP_ROM_PTR(&esp32_can_clearfilter_obj) },

    { MP_ROM_QSTR(MP_QSTR_get_state), MP_ROM_PTR(&esp32_can_get_state_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_counters), MP_ROM_PTR(&esp32_can_get_counters_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_timings), MP_ROM_PTR(&esp32_can_get_timings_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&esp32_can_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_mode), MP_ROM_PTR(&esp32_can_mode_obj) },
    // ESP32 Specific API
    { MP_OBJ_NEW_QSTR(MP_QSTR_clear_tx_queue), MP_ROM_PTR(&esp32_can_clear_tx_queue_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_clear_rx_queue), MP_ROM_PTR(&esp32_can_clear_rx_queue_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_get_alerts), MP_ROM_PTR(&esp32_can_alert_obj) },
    // CAN_MODE
    // class CAN.Mode
    { MP_ROM_QSTR(MP_QSTR_NORMAL), MP_ROM_INT(MODE_NORMAL) },
    { MP_ROM_QSTR(MP_QSTR_SLEEP), MP_ROM_INT(MODE_SLEEP) },
    { MP_ROM_QSTR(MP_QSTR_LOOPBACK), MP_ROM_INT(MODE_LOOPBACK) },
    { MP_ROM_QSTR(MP_QSTR_SILENT), MP_ROM_INT(MODE_SILENT) },
    { MP_ROM_QSTR(MP_QSTR_SILENT_LOOPBACK), MP_ROM_INT(MODE_SILENT_LOOPBACK) },
    { MP_ROM_QSTR(MP_QSTR_LISTEN_ONLY), MP_ROM_INT(MODE_LISTEN_ONLY) },



    //  SILENT_LOOPBACK - > set loopback to true and SILENT mode
    /* esp32 can modes
        NORMAL -> TWAI_MODE_NORMAL      - Normal operating mode where TWAI controller can send/receive/acknowledge messages
        SILENT -> TWAI_MODE_NO_ACK      - Transmission does not require acknowledgment. Use this mode for self testing. // This mode is useful when self testing the TWAI controller (loopback of transmissions).
        LISTEN_ONLY -> TWAI_MODE_LISTEN_ONLY - The TWAI controller will not influence the bus (No transmissions or acknowledgments) but can receive messages. // This mode is suited for bus monitor applications.
    */

    /* stm32 can modes
    #define CAN_MODE_NORMAL             FDCAN_MODE_NORMAL
    #define CAN_MODE_LOOPBACK           FDCAN_MODE_EXTERNAL_LOOPBACK
    #define CAN_MODE_SILENT             FDCAN_MODE_BUS_MONITORING
    #define CAN_MODE_SILENT_LOOPBACK    FDCAN_MODE_INTERNAL_LOOPBACK
    */

    // CAN_STATE
    // class CAN.State
    { MP_ROM_QSTR(MP_QSTR_STOPPED), MP_ROM_INT(TWAI_STATE_STOPPED) },
    { MP_ROM_QSTR(MP_QSTR_ERROR_ACTIVE), MP_ROM_INT(TWAI_STATE_RUNNING) },
    { MP_ROM_QSTR(MP_QSTR_ERROR_WARNING), MP_ROM_INT(-1) },
    { MP_ROM_QSTR(MP_QSTR_ERROR_PASSIVE), MP_ROM_INT(-1) },
    { MP_ROM_QSTR(MP_QSTR_BUS_OFF), MP_ROM_INT(TWAI_STATE_BUS_OFF) },
    { MP_ROM_QSTR(MP_QSTR_RECOVERING), MP_ROM_INT(TWAI_STATE_RECOVERING) }, // esp32 specific

    // class CAN.MessageFlags
    { MP_ROM_QSTR(MP_QSTR_RTR), MP_ROM_INT(RTR) },
    { MP_ROM_QSTR(MP_QSTR_EXTENDED_ID), MP_ROM_INT(EXTENDED_ID) },
    { MP_ROM_QSTR(MP_QSTR_FD_F), MP_ROM_INT(FD_F) },
    { MP_ROM_QSTR(MP_QSTR_BRS), MP_ROM_INT(BRS) },
    // class CAN.RecvErrors
    { MP_ROM_QSTR(MP_QSTR_CRC), MP_ROM_INT(CRC) },
    { MP_ROM_QSTR(MP_QSTR_FORM), MP_ROM_INT(FORM) },
    { MP_ROM_QSTR(MP_QSTR_OVERRUN), MP_ROM_INT(OVERRUN) },
    { MP_ROM_QSTR(MP_QSTR_ESI), MP_ROM_INT(ESI) },
    // class CAN.SendErrors
    { MP_ROM_QSTR(MP_QSTR_ARB), MP_ROM_INT(ARB) },
    { MP_ROM_QSTR(MP_QSTR_NACK), MP_ROM_INT(NACK) },
    { MP_ROM_QSTR(MP_QSTR_ERR), MP_ROM_INT(ERR) },
    // CAN_FILTER_MODE
    { MP_ROM_QSTR(MP_QSTR_FILTER_RAW_SINGLE), MP_ROM_INT(FILTER_RAW_SINGLE) },
    { MP_ROM_QSTR(MP_QSTR_FILTER_RAW_DUAL), MP_ROM_INT(FILTER_RAW_DUAL) },
    { MP_ROM_QSTR(MP_QSTR_FILTER_ADDRESS), MP_ROM_INT(FILTER_ADDRESS) },
    // CAN_ALERT
    { MP_ROM_QSTR(MP_QSTR_ALERT_ALL), MP_ROM_INT(TWAI_ALERT_ALL) },
    { MP_ROM_QSTR(MP_QSTR_ALERT_TX_IDLE), MP_ROM_INT(TWAI_ALERT_TX_IDLE) },
    { MP_ROM_QSTR(MP_QSTR_ALERT_TX_SUCCESS), MP_ROM_INT(TWAI_ALERT_TX_SUCCESS) },
    { MP_ROM_QSTR(MP_QSTR_ALERT_BELOW_ERR_WARN), MP_ROM_INT(TWAI_ALERT_BELOW_ERR_WARN) },
    { MP_ROM_QSTR(MP_QSTR_ALERT_ERR_ACTIVE), MP_ROM_INT(TWAI_ALERT_ERR_ACTIVE) },
    { MP_ROM_QSTR(MP_QSTR_ALERT_RECOVERY_IN_PROGRESS), MP_ROM_INT(TWAI_ALERT_RECOVERY_IN_PROGRESS) },
    { MP_ROM_QSTR(MP_QSTR_ALERT_BUS_RECOVERED), MP_ROM_INT(TWAI_ALERT_BUS_RECOVERED) },
    { MP_ROM_QSTR(MP_QSTR_ALERT_ARB_LOST), MP_ROM_INT(TWAI_ALERT_ARB_LOST) },
    { MP_ROM_QSTR(MP_QSTR_ALERT_ABOVE_ERR_WARN), MP_ROM_INT(TWAI_ALERT_ABOVE_ERR_WARN) },
    { MP_ROM_QSTR(MP_QSTR_ALERT_BUS_ERROR), MP_ROM_INT(TWAI_ALERT_BUS_ERROR) },
    { MP_ROM_QSTR(MP_QSTR_ALERT_TX_FAILED), MP_ROM_INT(TWAI_ALERT_TX_FAILED) },
    { MP_ROM_QSTR(MP_QSTR_ALERT_RX_QUEUE_FULL), MP_ROM_INT(TWAI_ALERT_RX_QUEUE_FULL) },
    { MP_ROM_QSTR(MP_QSTR_ALERT_ERR_PASS), MP_ROM_INT(TWAI_ALERT_ERR_PASS) },
    { MP_ROM_QSTR(MP_QSTR_ALERT_BUS_OFF), MP_ROM_INT(TWAI_ALERT_BUS_OFF) },
    
    // CAN Manager API - Clean function names
    { MP_ROM_QSTR(MP_QSTR_register), MP_ROM_PTR(&mp_can_register_obj) },
    { MP_ROM_QSTR(MP_QSTR_activate), MP_ROM_PTR(&mp_can_activate_obj) },
    { MP_ROM_QSTR(MP_QSTR_deactivate), MP_ROM_PTR(&mp_can_deactivate_obj) },
    { MP_ROM_QSTR(MP_QSTR_unregister), MP_ROM_PTR(&mp_can_unregister_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_rx_callback), MP_ROM_PTR(&mp_can_set_rx_callback_obj) },
    { MP_ROM_QSTR(MP_QSTR_transmit), MP_ROM_PTR(&mp_can_transmit_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_loopback), MP_ROM_PTR(&mp_can_set_loopback_obj) },
    
    // CAN Manager client mode constants - Clean names
    { MP_ROM_QSTR(MP_QSTR_TX_ENABLED), MP_ROM_INT(CAN_CLIENT_MODE_TX_ENABLED) },
    { MP_ROM_QSTR(MP_QSTR_RX_ONLY), MP_ROM_INT(CAN_CLIENT_MODE_RX_ONLY) }
};

static MP_DEFINE_CONST_DICT(esp32_can_locals_dict, esp32_can_locals_dict_table);

// Python object definition
MP_DEFINE_CONST_OBJ_TYPE(
    machine_can_type,
    MP_QSTR_CAN,
    MP_TYPE_FLAG_NONE,
    make_new, esp32_can_make_new,
    print, esp32_can_print,
    locals_dict, (mp_obj_dict_t *)&esp32_can_locals_dict
    );

MP_REGISTER_MODULE(MP_QSTR_CAN, machine_can_type);

// ============================================================================
// CAN Manager Implementation
// ============================================================================

// Internal mutex for thread-safe client list access
static SemaphoreHandle_t can_manager_mutex = NULL;

// Initialize manager mutex (called once)
static void can_manager_init_mutex(void) {
    if (can_manager_mutex == NULL) {
        can_manager_mutex = xSemaphoreCreateMutex();
        if (can_manager_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create CAN manager mutex");
        }
    }
}

// TX queue message structure
typedef struct {
    can_handle_t client_handle;
    twai_message_t msg;
} can_tx_queue_item_t;

// RX dispatcher task function
static void can_rx_dispatcher_task(void *arg);

// TX queue task function
static void can_tx_queue_task(void *arg);

// Internal function to find client by handle
static can_client_t* find_client(can_handle_t h) {
    can_client_t *client = esp32_can_obj.clients;
    while (client != NULL) {
        if (client == h) {
            return client;
        }
        client = client->next;
    }
    return NULL;
}

// Reference counting functions for safe callback execution
static void client_addref(can_client_t *client) {
    if (client != NULL) {
        __sync_fetch_and_add(&client->refcount, 1);
    }
}

static void client_release(can_client_t *client) {
    if (client != NULL) {
        // Save client ID before decrementing (in case structure is freed immediately after)
        uint32_t client_id = client->client_id;
        bool was_pending = client->pending_delete;
        
        // Atomically decrement refcount
        int old_ref = __sync_fetch_and_sub(&client->refcount, 1);
        
        // Log only if we brought refcount to 0 (don't access client structure after this!)
        if (old_ref == 1 && was_pending) {
            ESP_LOGD(TAG, "client_release: Client %lu refcount reached 0, can be freed", 
                     (unsigned long)client_id);
        }
    }
}

// Process deferred client frees (called by RX dispatcher at safe points)
static void deferred_free_clients(void) {
    if (xSemaphoreTake(can_manager_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }
    
    can_client_t *client = esp32_can_obj.pending_free_clients;
    can_client_t *prev = NULL;
    
    while (client != NULL) {
        can_client_t *next = client->next;
        
        // Check if client refcount is 0 and can be freed
        int refcount = __sync_fetch_and_add(&client->refcount, 0); // Atomic read
        if (refcount == 0) {
            // Remove from pending_free list
            if (prev == NULL) {
                esp32_can_obj.pending_free_clients = next;
            } else {
                prev->next = next;
            }
            
            ESP_LOGI(TAG, "deferred_free_clients: Freeing client %lu", (unsigned long)client->client_id);
            free(client);
            client = next;
        } else {
            // Still has references, keep in list
            ESP_LOGD(TAG, "deferred_free_clients: Client %lu still has refcount=%d", 
                     (unsigned long)client->client_id, refcount);
            prev = client;
            client = next;
        }
    }
    
    xSemaphoreGive(can_manager_mutex);
}

// Register a new CAN client (two-stage: bus stays STOPPED)
can_handle_t can_register(can_client_mode_t mode) {
    can_manager_init_mutex();
    
    if (xSemaphoreTake(can_manager_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "can_register: Failed to take mutex");
        return NULL;
    }
    
    // Allocate new client
    can_client_t *client = (can_client_t*)malloc(sizeof(can_client_t));
    if (client == NULL) {
        ESP_LOGE(TAG, "can_register: Failed to allocate client");
        xSemaphoreGive(can_manager_mutex);
        return NULL;
    }
    
    // Initialize client
    client->client_id = esp32_can_obj.next_client_id++;
    client->is_registered = true;
    client->is_activated = false;
    client->mode = mode;
    client->rx_callback = NULL;
    client->rx_callback_arg = NULL;
    client->next = esp32_can_obj.clients;
    client->refcount = 1;  // Start with refcount 1 (owned by manager)
    client->pending_delete = false;
    
    // Add to list
    esp32_can_obj.clients = client;
    esp32_can_obj.registered_clients++;
    
    ESP_LOGI(TAG, "can_register: Client %lu registered (mode=%d), total registered=%lu, activated=%lu", 
             (unsigned long)client->client_id, mode, 
             (unsigned long)esp32_can_obj.registered_clients,
             (unsigned long)esp32_can_obj.activated_clients);
    
    // IMPORTANT: Registration does NOT activate the bus
    // Bus state remains unchanged - if no clients are activated, bus stays STOPPED
    // This is the two-stage registration: register → activate
    
    xSemaphoreGive(can_manager_mutex);
    return (can_handle_t)client;
}

// Activate a registered client (bus activates if needed)
esp_err_t can_activate(can_handle_t h) {
    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(can_manager_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    
    can_client_t *client = find_client(h);
    if (client == NULL || !client->is_registered) {
        ESP_LOGE(TAG, "can_activate: Invalid or unregistered client");
        xSemaphoreGive(can_manager_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    
    if (client->is_activated) {
        ESP_LOGW(TAG, "can_activate: Client %lu already activated", (unsigned long)client->client_id);
        xSemaphoreGive(can_manager_mutex);
        return ESP_OK;
    }
    
    // Activate client
    client->is_activated = true;
    esp32_can_obj.activated_clients++;
    if (client->mode == CAN_CLIENT_MODE_TX_ENABLED) {
        esp32_can_obj.activated_transmitting_clients++;
    }
    
    ESP_LOGI(TAG, "can_activate: Client %lu activated (mode=%d), activated=%lu, tx=%lu",
             (unsigned long)client->client_id, client->mode,
             (unsigned long)esp32_can_obj.activated_clients,
             (unsigned long)esp32_can_obj.activated_transmitting_clients);
    
    xSemaphoreGive(can_manager_mutex);
    
    // IMPORTANT: Only activated clients affect bus state
    // State machine: TX clients → NORMAL, RX-only → LISTEN_ONLY, none → STOPPED
    // Update bus state (may start driver)
    update_bus_state();
    
    return ESP_OK;
}

// Deactivate a client (bus may go to STOPPED)
esp_err_t can_deactivate(can_handle_t h) {
    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(can_manager_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    
    can_client_t *client = find_client(h);
    if (client == NULL || !client->is_registered) {
        ESP_LOGE(TAG, "can_deactivate: Invalid or unregistered client");
        xSemaphoreGive(can_manager_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!client->is_activated) {
        ESP_LOGW(TAG, "can_deactivate: Client %lu not activated", (unsigned long)client->client_id);
        xSemaphoreGive(can_manager_mutex);
        return ESP_OK;
    }
    
    // Deactivate client
    client->is_activated = false;
    esp32_can_obj.activated_clients--;
    if (client->mode == CAN_CLIENT_MODE_TX_ENABLED) {
        esp32_can_obj.activated_transmitting_clients--;
    }
    
    ESP_LOGI(TAG, "can_deactivate: Client %lu deactivated, activated=%lu, tx=%lu",
             (unsigned long)client->client_id,
             (unsigned long)esp32_can_obj.activated_clients,
             (unsigned long)esp32_can_obj.activated_transmitting_clients);
    
    xSemaphoreGive(can_manager_mutex);
    
    // Update bus state (may stop driver)
    update_bus_state();
    
    return ESP_OK;
}

// Unregister a client completely (with deferred free for safety)
void can_unregister(can_handle_t h) {
    if (h == NULL) {
        return;
    }
    
    if (xSemaphoreTake(can_manager_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    
    can_client_t *client = find_client(h);
    if (client == NULL || !client->is_registered) {
        ESP_LOGE(TAG, "can_unregister: Invalid or unregistered client");
        xSemaphoreGive(can_manager_mutex);
        return;
    }
    
    // Deactivate first if needed
    if (client->is_activated) {
        client->is_activated = false;
        esp32_can_obj.activated_clients--;
        if (client->mode == CAN_CLIENT_MODE_TX_ENABLED) {
            esp32_can_obj.activated_transmitting_clients--;
        }
    }
    
    // Mark as unregistered and pending delete
    // Callback pointer will NOT be cleared yet - pending_delete flag prevents new callbacks
    client->is_registered = false;
    client->pending_delete = true;
    
    // Remove from active clients list
    if (esp32_can_obj.clients == client) {
        esp32_can_obj.clients = client->next;
    } else {
        can_client_t *prev = esp32_can_obj.clients;
        while (prev != NULL && prev->next != client) {
            prev = prev->next;
        }
        if (prev != NULL) {
            prev->next = client->next;
        }
    }
    
    // Add to pending_free list for deferred cleanup
    client->next = esp32_can_obj.pending_free_clients;
    esp32_can_obj.pending_free_clients = client;
    
    // Release manager's reference (refcount starts at 1)
    // If no callbacks are executing, refcount will be 0 and client can be freed
    // Otherwise, it will be freed when last callback completes
    client_release(client);
    
    esp32_can_obj.registered_clients--;
    
    ESP_LOGI(TAG, "can_unregister: Client %lu marked for deferred free, registered=%lu",
             (unsigned long)client->client_id, (unsigned long)esp32_can_obj.registered_clients);
    
    xSemaphoreGive(can_manager_mutex);
    
    // Update bus state (may stop driver if no clients left)
    update_bus_state();
    
    // Note: Client will be freed by deferred_free_clients() when refcount reaches 0
}

// Set RX callback for a client
void can_set_rx_callback(can_handle_t h, can_rx_callback_t cb, void *arg) {
    if (h == NULL) {
        return;
    }
    
    if (xSemaphoreTake(can_manager_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    
    can_client_t *client = find_client(h);
    if (client != NULL && client->is_registered) {
        client->rx_callback = cb;
        client->rx_callback_arg = arg;
        ESP_LOGD(TAG, "can_set_rx_callback: Client %lu callback set", (unsigned long)client->client_id);
    }
    
    xSemaphoreGive(can_manager_mutex);
}

// Add filter for a client (optional - not implemented yet)
esp_err_t can_add_filter(can_handle_t h, uint32_t id, uint32_t mask) {
    // TODO: Implement per-client filtering
    return ESP_ERR_NOT_SUPPORTED;
}

// Set client mode dynamically (with conflict checking)
esp_err_t can_set_mode(can_handle_t h, can_client_mode_t mode) {
    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(can_manager_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    
    can_client_t *client = find_client(h);
    if (client == NULL || !client->is_registered) {
        ESP_LOGE(TAG, "can_set_mode: Invalid or unregistered client");
        xSemaphoreGive(can_manager_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Check for conflicts when switching to RX_ONLY
    if (mode == CAN_CLIENT_MODE_RX_ONLY && client->mode == CAN_CLIENT_MODE_TX_ENABLED) {
        // Check if other activated clients have TX_ENABLED
        can_client_t *other = esp32_can_obj.clients;
        while (other != NULL) {
            if (other != client && other->is_activated && other->mode == CAN_CLIENT_MODE_TX_ENABLED) {
                ESP_LOGW(TAG, "can_set_mode: Cannot switch client %lu to RX_ONLY - other TX clients active",
                         (unsigned long)client->client_id);
                xSemaphoreGive(can_manager_mutex);
                return ESP_ERR_INVALID_STATE;
            }
            other = other->next;
        }
    }
    
    // Update counts if client is activated
    if (client->is_activated) {
        if (client->mode == CAN_CLIENT_MODE_TX_ENABLED) {
            esp32_can_obj.activated_transmitting_clients--;
        }
        if (mode == CAN_CLIENT_MODE_TX_ENABLED) {
            esp32_can_obj.activated_transmitting_clients++;
        }
    }
    
    // Update mode
    can_client_mode_t old_mode = client->mode;
    client->mode = mode;
    
    ESP_LOGI(TAG, "can_set_mode: Client %lu mode changed %d -> %d",
             (unsigned long)client->client_id, old_mode, mode);
    
    xSemaphoreGive(can_manager_mutex);
    
    // Update bus state if client is activated
    if (client->is_activated) {
        update_bus_state();
    }
    
    return ESP_OK;
}

// Check if client is registered
bool can_is_registered(can_handle_t h) {
    if (h == NULL) {
        return false;
    }
    
    if (xSemaphoreTake(can_manager_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    
    can_client_t *client = find_client(h);
    bool registered = (client != NULL && client->is_registered);
    
    xSemaphoreGive(can_manager_mutex);
    return registered;
}

// Set loopback mode (for testing/development)
// NOTE: This is a global bus setting - affects all clients
// Should be called BEFORE activating any clients for it to take effect
void can_set_loopback(bool enabled) {
    esp32_can_obj.loopback = enabled;
    ESP_LOGI(TAG, "can_set_loopback: Loopback mode %s", enabled ? "ENABLED" : "DISABLED");
    
    // If bus is already running, trigger reconfiguration
    if (esp32_can_obj.config->initialized && esp32_can_obj.handle != NULL) {
        ESP_LOGI(TAG, "can_set_loopback: Bus running - triggering reconfiguration");
        update_bus_state();
    }
}

// Transmit a CAN frame (queued)
esp_err_t can_transmit(can_handle_t h, const twai_message_t *msg) {
    if (h == NULL || msg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(can_manager_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    
    can_client_t *client = find_client(h);
    if (client == NULL || !client->is_registered || !client->is_activated) {
        ESP_LOGE(TAG, "can_transmit: Invalid, unregistered, or inactive client");
        xSemaphoreGive(can_manager_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    
    if (client->mode != CAN_CLIENT_MODE_TX_ENABLED) {
        ESP_LOGE(TAG, "can_transmit: Client %lu not in TX_ENABLED mode", (unsigned long)client->client_id);
        xSemaphoreGive(can_manager_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    
    // Capture handle while holding mutex
    twai_handle_t handle = esp32_can_obj.handle;
    
    xSemaphoreGive(can_manager_mutex);
    
    // Transmit directly using TWAI driver (no background task needed)
    if (handle == NULL) {
        ESP_LOGE(TAG, "can_transmit: Driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = twai_transmit_v2(handle, msg, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "can_transmit: Transmit failed: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

// Update bus state based on activated clients
static void update_bus_state(void) {
    if (xSemaphoreTake(can_manager_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    
    twai_mode_t target_mode;
    bool should_be_running = false;
    
    // Determine target mode based on ACTIVATED clients only (not registered)
    // This is the key part of the two-stage registration:
    // - Registered clients don't affect bus state
    // - Only activated clients determine bus mode
    uint32_t activated_tx = esp32_can_obj.activated_transmitting_clients;
    uint32_t activated_total = esp32_can_obj.activated_clients;
    uint32_t registered_total = esp32_can_obj.registered_clients;
    
    // Check if loopback mode should be enabled
    bool use_loopback = esp32_can_obj.loopback;
    
    ESP_LOGI(TAG, "update_bus_state: registered=%lu, activated=%lu, activated_tx=%lu, loopback=%d",
             (unsigned long)registered_total, (unsigned long)activated_total, (unsigned long)activated_tx,
             (int)use_loopback);
    
    if (activated_tx > 0) {
        // IMPORTANT: If loopback mode is configured, use TWAI_MODE_NO_ACK instead of NORMAL
        // TWAI_MODE_NO_ACK allows self-reception of transmitted frames without requiring
        // external ACK (essential for loopback testing without transceiver)
        if (use_loopback) {
            target_mode = TWAI_MODE_NO_ACK;
            ESP_LOGI(TAG, "update_bus_state: → NO_ACK mode (loopback enabled, TX clients active)");
        } else {
            target_mode = TWAI_MODE_NORMAL;
            ESP_LOGI(TAG, "update_bus_state: → NORMAL mode (TX clients active)");
        }
        should_be_running = true;
    } else if (activated_total > 0) {
        target_mode = TWAI_MODE_LISTEN_ONLY;
        should_be_running = true;
        ESP_LOGI(TAG, "update_bus_state: → LISTEN_ONLY mode (RX-only clients active)");
    } else {
        // No activated clients - bus should be STOPPED
        // Registered but not activated clients don't affect bus state
        target_mode = TWAI_MODE_NORMAL; // Doesn't matter, we'll stop
        should_be_running = false;
        ESP_LOGI(TAG, "update_bus_state: → STOPPED (no activated clients, %lu registered)", 
                 (unsigned long)registered_total);
    }
    
    xSemaphoreGive(can_manager_mutex);
    
    // Check current state
    bool driver_running = (esp32_can_obj.handle != NULL && esp32_can_obj.config->initialized);
    twai_mode_t current_mode = esp32_can_obj.config->general.mode;
    
    // If no clients activated, stop and uninstall driver
    if (!should_be_running) {
        if (driver_running) {
            ESP_LOGI(TAG, "update_bus_state: Stopping driver (no activated clients)");
            
            // Stop RX dispatcher task gracefully first
            if (esp32_can_obj.rx_dispatcher_task != NULL) {
                ESP_LOGI(TAG, "update_bus_state: Requesting RX dispatcher to stop");
                esp32_can_obj.rx_dispatcher_should_stop = true;
                
                // Wait for graceful exit
                int retries = 0;
                while (esp32_can_obj.rx_dispatcher_task != NULL && retries < 100) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    retries++;
                }
                
                if (esp32_can_obj.rx_dispatcher_task != NULL) {
                    ESP_LOGW(TAG, "update_bus_state: RX dispatcher did not exit, forcing deletion");
                    vTaskDelete(esp32_can_obj.rx_dispatcher_task);
                    esp32_can_obj.rx_dispatcher_task = NULL;
                }
                
                esp32_can_obj.rx_dispatcher_should_stop = false;
            }
            
            // Stop TX task
            if (esp32_can_obj.tx_task_handle != NULL) {
                vTaskDelete(esp32_can_obj.tx_task_handle);
                esp32_can_obj.tx_task_handle = NULL;
            }
            
            // Flush TX queue to prevent stale client handles
            if (esp32_can_obj.tx_queue != NULL) {
                can_tx_queue_item_t item;
                while (xQueueReceive(esp32_can_obj.tx_queue, &item, 0) == pdTRUE) {
                    // Discard queued messages
                }
                ESP_LOGD(TAG, "update_bus_state: Flushed TX queue");
            }
            
            // Now safe to stop and uninstall driver
            if (esp32_can_obj.handle != NULL) {
                twai_stop_v2(esp32_can_obj.handle);
                twai_driver_uninstall_v2(esp32_can_obj.handle);
                esp32_can_obj.handle = NULL;
            }
            esp32_can_obj.config->initialized = false;
        }
        return;
    }
    
    // Need to start or reconfigure driver
    if (!driver_running) {
        // Driver not running - need to install and start
        ESP_LOGI(TAG, "update_bus_state: Installing driver (mode=%d)", target_mode);
        
        // Configure mode
        esp32_can_obj.config->general.mode = target_mode;
        
        // Install driver
        esp_err_t ret = twai_driver_install_v2(&esp32_can_obj.config->general,
                                               &esp32_can_obj.config->timing,
                                               &esp32_can_obj.config->filter,
                                               &esp32_can_obj.handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "update_bus_state: Failed to install driver: %s", esp_err_to_name(ret));
            return;
        }
        
        // Start driver
        ret = twai_start_v2(esp32_can_obj.handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "update_bus_state: Failed to start driver: %s", esp_err_to_name(ret));
            twai_driver_uninstall_v2(esp32_can_obj.handle);
            esp32_can_obj.handle = NULL;
            return;
        }
        
        esp32_can_obj.config->initialized = true;
        
        // Start RX dispatcher task to poll for received frames
        if (esp32_can_obj.rx_dispatcher_task == NULL) {
            BaseType_t ret = xTaskCreate(can_rx_dispatcher_task, "can_rx_disp", 
                                        CAN_TASK_STACK_SIZE, NULL, CAN_TASK_PRIORITY,
                                        &esp32_can_obj.rx_dispatcher_task);
            if (ret != pdPASS) {
                ESP_LOGE(TAG, "update_bus_state: Failed to create RX dispatcher task");
            }
        }
        
        ESP_LOGI(TAG, "update_bus_state: Driver started (mode=%d)", target_mode);
        
    } else if (current_mode != target_mode) {
        // Driver running but mode needs to change - stop, reconfigure, restart
        ESP_LOGI(TAG, "update_bus_state: Reconfiguring driver %d -> %d", current_mode, target_mode);
        
        // CRITICAL: Stop RX dispatcher task gracefully to ensure no callbacks are executing
        // This prevents crashes during driver stop/uninstall
        TaskHandle_t old_rx_task = esp32_can_obj.rx_dispatcher_task;
        if (old_rx_task != NULL) {
            ESP_LOGI(TAG, "update_bus_state: Requesting RX dispatcher task to stop");
            
            // Set stop flag to request graceful shutdown
            esp32_can_obj.rx_dispatcher_should_stop = true;
            
            // Wait for task to acknowledge and exit (task will set rx_dispatcher_task to NULL)
            int retries = 0;
            const int max_retries = 100; // 1 second timeout
            while (esp32_can_obj.rx_dispatcher_task != NULL && retries < max_retries) {
                vTaskDelay(pdMS_TO_TICKS(10));
                retries++;
            }
            
            if (esp32_can_obj.rx_dispatcher_task != NULL) {
                ESP_LOGW(TAG, "update_bus_state: RX dispatcher task did not exit gracefully after %d ms, forcing stop", retries * 10);
                // Stop driver to force task to exit on next receive
                twai_stop_v2(esp32_can_obj.handle);
                // Wait a bit more
                retries = 0;
                while (esp32_can_obj.rx_dispatcher_task != NULL && retries < 50) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    retries++;
                }
                if (esp32_can_obj.rx_dispatcher_task != NULL) {
                    ESP_LOGE(TAG, "update_bus_state: RX dispatcher task still running, forcing deletion (unsafe!)");
                    vTaskDelete(esp32_can_obj.rx_dispatcher_task);
                    esp32_can_obj.rx_dispatcher_task = NULL;
                }
            } else {
                ESP_LOGI(TAG, "update_bus_state: RX dispatcher task exited gracefully after %d ms", retries * 10);
                // Stop driver now that task has exited
                twai_stop_v2(esp32_can_obj.handle);
            }
            
            // Clear stop flag for next start
            esp32_can_obj.rx_dispatcher_should_stop = false;
        } else {
            // No RX task, just stop driver
            twai_stop_v2(esp32_can_obj.handle);
        }
        
        // Now safe to uninstall driver (no callbacks can be executing)
        twai_driver_uninstall_v2(esp32_can_obj.handle);
        
        // Configure new mode
        esp32_can_obj.config->general.mode = target_mode;
        
        // Reinstall with new mode
        esp_err_t ret = twai_driver_install_v2(&esp32_can_obj.config->general,
                                               &esp32_can_obj.config->timing,
                                               &esp32_can_obj.config->filter,
                                               &esp32_can_obj.handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "update_bus_state: Failed to reinstall driver: %s", esp_err_to_name(ret));
            esp32_can_obj.handle = NULL;
            esp32_can_obj.config->initialized = false;
            return;
        }
        
        // Restart driver
        ret = twai_start_v2(esp32_can_obj.handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "update_bus_state: Failed to restart driver: %s", esp_err_to_name(ret));
            twai_driver_uninstall_v2(esp32_can_obj.handle);
            esp32_can_obj.handle = NULL;
            esp32_can_obj.config->initialized = false;
            return;
        }
        
        // Create new RX dispatcher task for new driver instance
        if (esp32_can_obj.rx_dispatcher_task == NULL) {
            BaseType_t task_ret = xTaskCreate(can_rx_dispatcher_task, "can_rx_disp", 
                                        CAN_TASK_STACK_SIZE, NULL, CAN_TASK_PRIORITY,
                                        &esp32_can_obj.rx_dispatcher_task);
            if (task_ret != pdPASS) {
                ESP_LOGE(TAG, "update_bus_state: Failed to create RX dispatcher task after reconfiguration");
            }
        }
        
        ESP_LOGI(TAG, "update_bus_state: Driver reconfigured (mode=%d)", target_mode);
    }
}

// RX dispatcher task - dispatches frames to all activated clients
static void can_rx_dispatcher_task(void *arg) {
    twai_message_t rx_msg;
    
    ESP_LOGI(TAG, "RX dispatcher task started");
    
    while (1) {
        // Check if we should stop (set by update_bus_state)
        if (esp32_can_obj.rx_dispatcher_should_stop) {
            ESP_LOGI(TAG, "RX dispatcher task exiting (stop requested)");
            break;
        }
        
        // Process deferred client frees at safe point (before receiving new frame)
        deferred_free_clients();
        
        // Wait for message from TWAI driver
        esp_err_t ret = twai_receive_v2(esp32_can_obj.handle, &rx_msg, pdMS_TO_TICKS(100));
        if (ret != ESP_OK) {
            if (ret == ESP_ERR_INVALID_STATE) {
                // Driver stopped - exit task
                ESP_LOGI(TAG, "RX dispatcher task exiting (driver stopped)");
                break;
            }
            if (ret == ESP_ERR_TIMEOUT) {
                // Timeout - check stop flag again
                continue;
            }
            continue;
        }
        
        // Dispatch to all activated clients with reference counting for safety
        // Build a list of callbacks to call while holding mutex, with refcounts incremented
        #define MAX_CLIENTS_PER_FRAME 8
        struct {
            can_client_t *client;  // Only used for releasing reference, NOT dereferenced
            can_rx_callback_t cb;
            void *cb_arg;
            bool should_call;  // Captured while holding mutex
        } callback_list[MAX_CLIENTS_PER_FRAME];
        int callback_count = 0;
        
        if (xSemaphoreTake(can_manager_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            can_client_t *client = esp32_can_obj.clients;
            while (client != NULL && callback_count < MAX_CLIENTS_PER_FRAME) {
                // Check if client is valid, activated, not pending delete, and has callback
                // IMPORTANT: Capture should_call flag while holding mutex - don't check later!
                bool should_call = (client->is_activated && client->is_registered && 
                                   !client->pending_delete && client->rx_callback != NULL);
                
                if (should_call) {
                    // Add reference before releasing mutex (prevents free during callback)
                    client_addref(client);
                    
                    // Save callback info - capture everything while holding mutex
                    // DO NOT dereference client pointer after releasing mutex!
                    callback_list[callback_count].client = client;  // Only for client_release()
                    callback_list[callback_count].cb = client->rx_callback;
                    callback_list[callback_count].cb_arg = client->rx_callback_arg;
                    callback_list[callback_count].should_call = should_call;
                    callback_count++;
                }
                
                // Move to next client (safe because we're holding mutex)
                client = client->next;
            }
            xSemaphoreGive(can_manager_mutex);
        }
        
        // Now call all callbacks without holding mutex (safe due to refcounting)
        // CRITICAL: Do NOT dereference client pointer - only use captured callback info!
        for (int i = 0; i < callback_count; i++) {
            // Call callback unconditionally if it was valid when we captured it
            // The refcount prevents the client structure from being freed
            if (callback_list[i].should_call && callback_list[i].cb != NULL) {
                callback_list[i].cb(&rx_msg, callback_list[i].cb_arg);
            }
            
            // Release reference after callback completes
            // client_release() is safe because it doesn't dereference after decrement
            client_release(callback_list[i].client);
        }
    }
    
    // Clear stop flag before exiting
    esp32_can_obj.rx_dispatcher_should_stop = false;
    
    ESP_LOGI(TAG, "RX dispatcher task deleted");
    esp32_can_obj.rx_dispatcher_task = NULL;
    vTaskDelete(NULL);
}

// TX queue task - processes TX queue
static void can_tx_queue_task(void *arg) {
    QueueHandle_t tx_queue = (QueueHandle_t)arg;
    can_tx_queue_item_t item;
    
    ESP_LOGI(TAG, "TX queue task started");
    
    while (1) {
        // Wait for message from queue
        if (xQueueReceive(tx_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        
        // Transmit frame directly (no client validation to avoid use-after-free)
        // If client was unregistered, that's fine - message was queued before unregistration
        if (esp32_can_obj.handle != NULL) {
            esp_err_t ret = twai_transmit_v2(esp32_can_obj.handle, &item.msg, portMAX_DELAY);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "TX queue task: Transmit failed: %s", esp_err_to_name(ret));
            }
        }
    }
    
    ESP_LOGI(TAG, "TX queue task deleted");
    vTaskDelete(NULL);
}
