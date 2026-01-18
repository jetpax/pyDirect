/**
 * exi_din.h - DIN 70121 EXI Codec Header
 */

#ifndef EXI_DIN_H
#define EXI_DIN_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// Message Types
// ============================================================================

typedef enum {
    DIN_MSG_UNKNOWN = 0,
    DIN_MSG_SUPPORTED_APP_PROTOCOL_REQ,
    DIN_MSG_SUPPORTED_APP_PROTOCOL_RES,
    DIN_MSG_SESSION_SETUP_REQ,
    DIN_MSG_SESSION_SETUP_RES,
    DIN_MSG_SERVICE_DISCOVERY_REQ,
    DIN_MSG_SERVICE_DISCOVERY_RES,
    DIN_MSG_SERVICE_PAYMENT_SELECTION_REQ,
    DIN_MSG_SERVICE_PAYMENT_SELECTION_RES,
    DIN_MSG_CONTRACT_AUTHENTICATION_REQ,
    DIN_MSG_CONTRACT_AUTHENTICATION_RES,
    DIN_MSG_CHARGE_PARAMETER_DISCOVERY_REQ,
    DIN_MSG_CHARGE_PARAMETER_DISCOVERY_RES,
    DIN_MSG_CABLE_CHECK_REQ,
    DIN_MSG_CABLE_CHECK_RES,
    DIN_MSG_PRE_CHARGE_REQ,
    DIN_MSG_PRE_CHARGE_RES,
    DIN_MSG_POWER_DELIVERY_REQ,
    DIN_MSG_POWER_DELIVERY_RES,
    DIN_MSG_CURRENT_DEMAND_REQ,
    DIN_MSG_CURRENT_DEMAND_RES,
    DIN_MSG_WELDING_DETECTION_REQ,
    DIN_MSG_WELDING_DETECTION_RES,
    DIN_MSG_SESSION_STOP_REQ,
    DIN_MSG_SESSION_STOP_RES,
} din_msg_type_t;

typedef enum {
    DIN_RESPONSE_OK = 0,
    DIN_RESPONSE_OK_NEW_SESSION = 0,
    DIN_RESPONSE_OK_OLD_SESSION = 1,
    DIN_RESPONSE_FAILED = 2,
} din_response_code_t;

// ============================================================================
// Decoded Message Structure
// ============================================================================

typedef struct {
    din_msg_type_t type;
    uint8_t session_id[8];
    bool session_id_valid;
    
    uint8_t schema_id;
    
    uint16_t ev_max_voltage;
    uint16_t ev_max_current;
    uint16_t ev_target_voltage;
    uint16_t ev_target_current;
    uint16_t ev_target_current_demand;
    bool charging_complete;
    bool charge_progress_start;
    
} din_decoded_msg_t;

// ============================================================================
// Encode Parameters Structure
// ============================================================================

typedef struct {
    din_msg_type_t type;
    din_response_code_t response_code;
    uint8_t session_id[8];
    
    uint16_t evse_max_voltage;
    uint16_t evse_max_current;
    uint16_t evse_min_voltage;
    uint16_t evse_min_current;
    uint16_t evse_max_power;
    
    uint16_t evse_present_voltage;
    uint16_t evse_present_current;
    
    bool evse_isolation_ok;
    bool processing_finished;
    
} din_encode_params_t;

// ============================================================================
// V2GTP Header Functions
// ============================================================================

/**
 * Add V2GTP header to EXI data
 * @param buffer Output buffer (must be at least exi_len + 8 bytes)
 * @param exi_data EXI data to wrap
 * @param exi_len Length of EXI data
 * @return Total length (exi_len + 8)
 */
int exi_add_v2gtp_header(uint8_t *buffer, const uint8_t *exi_data, int exi_len);

/**
 * Remove V2GTP header from data
 * @param v2gtp_data Input data with V2GTP header
 * @param v2gtp_len Length of input data
 * @param exi_data_out Output pointer to EXI data (within v2gtp_data)
 * @param exi_len_out Output EXI length
 * @return 0 on success, -1 on error
 */
int exi_remove_v2gtp_header(const uint8_t *v2gtp_data, int v2gtp_len, 
                             const uint8_t **exi_data_out, int *exi_len_out);

// ============================================================================
// Message Detection and Decoding
// ============================================================================

/**
 * Detect message type from EXI data
 * @param exi EXI data (without V2GTP header)
 * @param len Length of EXI data
 * @return Message type
 */
din_msg_type_t exi_detect_message_type(const uint8_t *exi, int len);

/**
 * Decode EXI message
 * @param exi EXI data (without V2GTP header)
 * @param len Length of EXI data
 * @param msg Output decoded message
 * @return 0 on success, -1 on error
 */
int exi_decode(const uint8_t *exi, int len, din_decoded_msg_t *msg);

// ============================================================================
// Message Encoding
// ============================================================================

/**
 * Encode a DIN message response
 * @param params Encode parameters
 * @param buffer Output buffer
 * @param buffer_size Size of output buffer
 * @return Length of encoded data, or -1 on error
 */
int exi_encode(din_encode_params_t *params, uint8_t *buffer, int buffer_size);

/**
 * Encode message with V2GTP header
 * @param params Encode parameters
 * @param buffer Output buffer (must be at least encoded_len + 8 bytes)
 * @param buffer_size Size of output buffer
 * @return Total length including header, or -1 on error
 */
int exi_encode_with_header(din_encode_params_t *params, uint8_t *buffer, int buffer_size);

// ============================================================================
// Utilities
// ============================================================================

/**
 * Get human-readable message type name
 */
const char* exi_msg_type_name(din_msg_type_t type);

#endif // EXI_DIN_H
