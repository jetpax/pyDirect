/**
 * exi_din.c - Minimal DIN 70121 EXI Codec for EVSE Mode
 * 
 * This is a simplified, hardcoded EXI codec for the DIN 70121 schema.
 * It handles only the messages needed for EVSE mode contactor closure.
 * 
 * NOT a full EXI implementation - just pattern matching and template-based encoding.
 * 
 * Based on test vectors from pyPLC (github.com/uhi22/pyPLC)
 * 
 * Supported Messages (EVSE responses):
 * - SupportedAppProtocolRes
 * - SessionSetupRes
 * - ServiceDiscoveryRes  
 * - ServicePaymentSelectionRes
 * - ContractAuthenticationRes
 * - ChargeParameterDiscoveryRes
 * - CableCheckRes
 * - PreChargeRes
 * - PowerDeliveryRes
 * - CurrentDemandRes
 * - WeldingDetectionRes
 * - SessionStopRes
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// EXI Message Types (bit patterns in the EXI stream)
// ============================================================================

// V2GTP Header
#define V2GTP_VERSION       0x01
#define V2GTP_VERSION_INV   0xFE
#define V2GTP_PAYLOAD_EXI   0x8001

// DIN Message IDs (from EXI structure)
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

// Response codes
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
    
    // For SupportedAppProtocolReq
    uint8_t schema_id;
    
    // For ChargeParameterDiscoveryReq
    uint16_t ev_max_voltage;    // in 0.1V units (e.g., 4000 = 400.0V)
    uint16_t ev_max_current;    // in 0.1A units
    
    // For PreChargeReq
    uint16_t ev_target_voltage; // in 0.1V units
    uint16_t ev_target_current; // in 0.1A units
    
    // For CurrentDemandReq
    uint16_t ev_target_current_demand;
    bool charging_complete;
    
    // For PowerDeliveryReq
    bool charge_progress_start;  // true = start, false = stop
    
} din_decoded_msg_t;

// ============================================================================
// Encoded Message Structure  
// ============================================================================

typedef struct {
    din_msg_type_t type;
    din_response_code_t response_code;
    uint8_t session_id[8];
    
    // For ChargeParameterDiscoveryRes
    uint16_t evse_max_voltage;      // in 0.1V units
    uint16_t evse_max_current;      // in 0.1A units
    uint16_t evse_min_voltage;      // in 0.1V units
    uint16_t evse_min_current;      // in 0.1A units
    uint16_t evse_max_power;        // in W
    
    // For PreChargeRes / CurrentDemandRes
    uint16_t evse_present_voltage;  // in 0.1V units
    uint16_t evse_present_current;  // in 0.1A units
    
    // For CableCheckRes
    bool evse_isolation_ok;
    bool processing_finished;       // false = ongoing, true = finished
    
} din_encode_params_t;

// ============================================================================
// V2GTP Header Functions
// ============================================================================

int exi_add_v2gtp_header(uint8_t *buffer, const uint8_t *exi_data, int exi_len) {
    buffer[0] = V2GTP_VERSION;
    buffer[1] = V2GTP_VERSION_INV;
    buffer[2] = 0x80;  // Payload type high byte
    buffer[3] = 0x01;  // Payload type low byte (0x8001 = EXI)
    buffer[4] = (exi_len >> 24) & 0xFF;
    buffer[5] = (exi_len >> 16) & 0xFF;
    buffer[6] = (exi_len >> 8) & 0xFF;
    buffer[7] = exi_len & 0xFF;
    memcpy(buffer + 8, exi_data, exi_len);
    return exi_len + 8;
}

int exi_remove_v2gtp_header(const uint8_t *v2gtp_data, int v2gtp_len, 
                             const uint8_t **exi_data_out, int *exi_len_out) {
    if (v2gtp_len < 8) return -1;
    if (v2gtp_data[0] != V2GTP_VERSION) return -1;
    if (v2gtp_data[1] != V2GTP_VERSION_INV) return -1;
    
    int payload_len = (v2gtp_data[4] << 24) | (v2gtp_data[5] << 16) | 
                      (v2gtp_data[6] << 8) | v2gtp_data[7];
    
    *exi_data_out = v2gtp_data + 8;
    *exi_len_out = payload_len;
    return 0;
}

// ============================================================================
// Message Detection (Pattern Matching)
// ============================================================================

/**
 * Detect message type from EXI data
 * Uses pattern matching on known message prefixes
 */
din_msg_type_t exi_detect_message_type(const uint8_t *exi, int len) {
    if (len < 2) return DIN_MSG_UNKNOWN;
    
    // SupportedAppProtocolReq starts with 0x80 0x00
    // (Handshake namespace)
    if (exi[0] == 0x80 && exi[1] == 0x00) {
        // Check for "dbab" pattern (common in handshake)
        if (len >= 4 && exi[2] == 0xdb && exi[3] == 0xab) {
            return DIN_MSG_SUPPORTED_APP_PROTOCOL_REQ;
        }
        if (len >= 4 && exi[2] == 0xeb && exi[3] == 0xab) {
            return DIN_MSG_SUPPORTED_APP_PROTOCOL_REQ;
        }
    }
    
    // SupportedAppProtocolRes starts with 0x80 0x40
    if (exi[0] == 0x80 && exi[1] == 0x40) {
        return DIN_MSG_SUPPORTED_APP_PROTOCOL_RES;
    }
    
    // DIN messages start with 0x809a
    if (len >= 4 && exi[0] == 0x80 && exi[1] == 0x9a) {
        // Byte 2-3 encode the message type
        uint8_t msg_byte = exi[2];
        
        // Request messages (from car)
        if (msg_byte == 0x00) {
            uint8_t sub = exi[3] >> 4;
            switch (sub) {
                case 0x1: // 0x1x = various requests
                    if ((exi[3] & 0x0F) == 0x1) return DIN_MSG_SESSION_SETUP_REQ;      // 0x11
                    if ((exi[3] & 0x0F) == 0x9) return DIN_MSG_SERVICE_DISCOVERY_REQ;  // 0x19
                    if ((exi[3] & 0x0F) == 0x0) return DIN_MSG_CABLE_CHECK_REQ;        // 0x10
                    if ((exi[3] & 0x0F) == 0x5) return DIN_MSG_PRE_CHARGE_REQ;         // 0x15
                    if ((exi[3] & 0x0F) == 0x3) return DIN_MSG_POWER_DELIVERY_REQ;     // 0x13
                    if ((exi[3] & 0x0F) == 0xd) return DIN_MSG_CURRENT_DEMAND_REQ;     // 0x0d
                    if ((exi[3] & 0x0F) == 0x2) return DIN_MSG_WELDING_DETECTION_REQ;  // 0x12
                    if ((exi[3] & 0x0F) == 0xf) return DIN_MSG_SESSION_STOP_REQ;       // 0x1f
                    break;
            }
            // More patterns
            if (exi[3] == 0xb2) return DIN_MSG_SERVICE_PAYMENT_SELECTION_REQ;
            if (exi[3] == 0xb8 || exi[3] == 0x72) return DIN_MSG_CONTRACT_AUTHENTICATION_REQ;
            if ((exi[3] & 0xF0) == 0x70) return DIN_MSG_CHARGE_PARAMETER_DISCOVERY_REQ;
            if (exi[3] == 0xd0) return DIN_MSG_SESSION_SETUP_REQ;
        }
        
        // Look for specific patterns for robust detection
        // SessionSetupReq: 809a0011d0 or similar
        if (len >= 5 && exi[2] == 0x00 && exi[3] == 0x11 && (exi[4] & 0xF0) == 0xd0) {
            return DIN_MSG_SESSION_SETUP_REQ;
        }
        // ServiceDiscoveryReq: 809a001198
        if (len >= 5 && exi[2] == 0x00 && exi[3] == 0x11 && exi[4] == 0x98) {
            return DIN_MSG_SERVICE_DISCOVERY_REQ;
        }
        // ServicePaymentSelectionReq: contains 0xb2
        if (len >= 5 && exi[2] == 0x00 && exi[3] == 0x11 && exi[4] == 0xb2) {
            return DIN_MSG_SERVICE_PAYMENT_SELECTION_REQ;
        }
        // ContractAuthenticationReq: 809a0010b8 or similar
        if (len >= 5 && exi[2] == 0x00 && (exi[3] == 0x10 || exi[3] == 0x11) && 
            (exi[4] == 0xb8 || (exi[4] & 0xF0) == 0xb0)) {
            return DIN_MSG_CONTRACT_AUTHENTICATION_REQ;
        }
        // ChargeParameterDiscoveryReq: 809a0010721... or 809a0211c1...
        if (len >= 5 && (exi[3] == 0x07 || (exi[3] & 0x0F) == 0x07 || 
            (exi[2] == 0x02 && exi[3] == 0x11))) {
            return DIN_MSG_CHARGE_PARAMETER_DISCOVERY_REQ;
        }
        // CableCheckReq: 809a001010
        if (len >= 5 && exi[2] == 0x00 && exi[3] == 0x10 && exi[4] == 0x10) {
            return DIN_MSG_CABLE_CHECK_REQ;
        }
        // PreChargeReq: 809a001150
        if (len >= 5 && exi[2] == 0x00 && exi[3] == 0x11 && exi[4] == 0x50) {
            return DIN_MSG_PRE_CHARGE_REQ;
        }
        // PowerDeliveryReq: 809a001130 or 809a00113060
        if (len >= 5 && exi[2] == 0x00 && exi[3] == 0x11 && (exi[4] & 0xF0) == 0x30) {
            return DIN_MSG_POWER_DELIVERY_REQ;
        }
        // CurrentDemandReq: 809a0010d0
        if (len >= 5 && exi[2] == 0x00 && exi[3] == 0x10 && exi[4] == 0xd0) {
            return DIN_MSG_CURRENT_DEMAND_REQ;
        }
        // WeldingDetectionReq: 809a001210
        if (len >= 5 && exi[2] == 0x00 && exi[3] == 0x12 && exi[4] == 0x10) {
            return DIN_MSG_WELDING_DETECTION_REQ;
        }
        // SessionStopReq: 809a0011f0
        if (len >= 5 && exi[2] == 0x00 && exi[3] == 0x11 && exi[4] == 0xf0) {
            return DIN_MSG_SESSION_STOP_REQ;
        }
    }
    
    return DIN_MSG_UNKNOWN;
}

// ============================================================================
// Message Decoding (Minimal parsing)
// ============================================================================

/**
 * Decode PreChargeReq to extract target voltage/current
 * Format: 809a001150400000c80006400000
 *         ^^^^^^^^ header
 *                 ^^^^^^^^ target voltage (encoded)
 *                         ^^^^^^^^ target current (encoded)
 */
static void decode_pre_charge_req(const uint8_t *exi, int len, din_decoded_msg_t *msg) {
    msg->type = DIN_MSG_PRE_CHARGE_REQ;
    
    // Simplified extraction - real EXI parsing would be more complex
    // The voltage/current are in the later bytes, bit-packed
    // For now, use defaults that will work
    msg->ev_target_voltage = 4000;  // 400.0V default
    msg->ev_target_current = 100;   // 10.0A default
    
    // Try to extract from known positions (varies by message)
    if (len >= 12) {
        // Voltage is typically around byte 8-10
        // This is a simplified heuristic
        uint8_t v_hi = exi[8];
        uint8_t v_lo = exi[9];
        if (v_hi > 0 || v_lo > 0) {
            msg->ev_target_voltage = (v_hi << 8) | v_lo;
            if (msg->ev_target_voltage > 10000) {
                msg->ev_target_voltage = 4000;  // Sanity check
            }
        }
    }
}

/**
 * Decode CurrentDemandReq
 */
static void decode_current_demand_req(const uint8_t *exi, int len, din_decoded_msg_t *msg) {
    msg->type = DIN_MSG_CURRENT_DEMAND_REQ;
    msg->ev_target_voltage = 4000;
    msg->ev_target_current_demand = 500;  // 50.0A default
    msg->charging_complete = false;
}

/**
 * Decode PowerDeliveryReq
 */
static void decode_power_delivery_req(const uint8_t *exi, int len, din_decoded_msg_t *msg) {
    msg->type = DIN_MSG_POWER_DELIVERY_REQ;
    
    // Check for start/stop in the message
    // 809a00113060 = stop, 809a00113020 = start (from pyPLC)
    msg->charge_progress_start = true;  // Default to start
    
    if (len >= 6) {
        // Last nibble often indicates start(0)/stop(6)
        if ((exi[5] & 0x0F) == 0x06) {
            msg->charge_progress_start = false;
        }
    }
}

int exi_decode(const uint8_t *exi, int len, din_decoded_msg_t *msg) {
    memset(msg, 0, sizeof(*msg));
    
    msg->type = exi_detect_message_type(exi, len);
    
    switch (msg->type) {
        case DIN_MSG_SUPPORTED_APP_PROTOCOL_REQ:
            // Extract schema ID if needed
            msg->schema_id = 1;  // DIN
            break;
            
        case DIN_MSG_PRE_CHARGE_REQ:
            decode_pre_charge_req(exi, len, msg);
            break;
            
        case DIN_MSG_CURRENT_DEMAND_REQ:
            decode_current_demand_req(exi, len, msg);
            break;
            
        case DIN_MSG_POWER_DELIVERY_REQ:
            decode_power_delivery_req(exi, len, msg);
            break;
            
        default:
            // Other messages don't need deep parsing for EVSE mode
            break;
    }
    
    return (msg->type != DIN_MSG_UNKNOWN) ? 0 : -1;
}

// ============================================================================
// Message Encoding (Template-based)
// ============================================================================

// Pre-encoded message templates from pyPLC test vectors
// These are complete EXI messages that can be used as-is or with minor modifications

// SupportedAppProtocolRes - Schema accepted, schemaID = 0
static const uint8_t TPL_SUPPORTED_APP_PROTOCOL_RES[] = {0x80, 0x40, 0x00, 0x40};

// SessionSetupRes - ResponseCode OK, NewSessionEstablished
// Template: 809a02004080c1014181c211e0000080
// Bytes 4-11 contain session ID, byte 12+ has EVSE ID and timestamp
static const uint8_t TPL_SESSION_SETUP_RES[] = {
    0x80, 0x9a, 0x02, 0x00, 0x40, 0x80, 0xc1, 0x01, 
    0x41, 0x81, 0xc2, 0x11, 0xe0, 0x00, 0x00, 0x80
};

// ServiceDiscoveryRes - DC charging, external payment
// Template: 809a0011a0012002412104
static const uint8_t TPL_SERVICE_DISCOVERY_RES[] = {
    0x80, 0x9a, 0x00, 0x11, 0xa0, 0x01, 0x20, 0x02, 0x41, 0x21, 0x04
};

// ServicePaymentSelectionRes - OK
// Template: 809a0011c000
static const uint8_t TPL_SERVICE_PAYMENT_SELECTION_RES[] = {
    0x80, 0x9a, 0x00, 0x11, 0xc0, 0x00
};

// ContractAuthenticationRes - Finished (no authentication needed)
// Template: 809a021a3b7c417774813310c00200
static const uint8_t TPL_CONTRACT_AUTHENTICATION_RES[] = {
    0x80, 0x9a, 0x02, 0x1a, 0x3b, 0x7c, 0x41, 0x77,
    0x74, 0x81, 0x33, 0x10, 0xc0, 0x02, 0x00
};

// ChargeParameterDiscoveryRes
// Template: 809a001080004820400000c99002062050193080c0c802064c8010190140c80a20
static const uint8_t TPL_CHARGE_PARAMETER_DISCOVERY_RES[] = {
    0x80, 0x9a, 0x00, 0x10, 0x80, 0x00, 0x48, 0x20,
    0x40, 0x00, 0x00, 0xc9, 0x90, 0x02, 0x06, 0x20,
    0x50, 0x19, 0x30, 0x80, 0xc0, 0xc8, 0x02, 0x06,
    0x4c, 0x80, 0x10, 0x19, 0x01, 0x40, 0xc8, 0x0a, 0x20
};

// CableCheckRes - OK, Finished
// Template: 809a0010200200000000
static const uint8_t TPL_CABLE_CHECK_RES_FINISHED[] = {
    0x80, 0x9a, 0x00, 0x10, 0x20, 0x02, 0x00, 0x00, 0x00, 0x00
};

// CableCheckRes - OK, Processing
static const uint8_t TPL_CABLE_CHECK_RES_ONGOING[] = {
    0x80, 0x9a, 0x00, 0x10, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00
};

// PreChargeRes - OK, with present voltage
// Template: 809a00116002000000320000
// Voltage is encoded in bytes 10-12
static const uint8_t TPL_PRE_CHARGE_RES[] = {
    0x80, 0x9a, 0x00, 0x11, 0x60, 0x02, 0x00, 0x00, 
    0x00, 0x32, 0x00, 0x00
};

// PowerDeliveryRes - OK
// Template: 809a0011400420400000
static const uint8_t TPL_POWER_DELIVERY_RES[] = {
    0x80, 0x9a, 0x00, 0x11, 0x40, 0x04, 0x20, 0x40, 0x00, 0x00
};

// CurrentDemandRes - OK
// Template: 809a0010e0020000003200019000000600
static const uint8_t TPL_CURRENT_DEMAND_RES[] = {
    0x80, 0x9a, 0x00, 0x10, 0xe0, 0x02, 0x00, 0x00,
    0x00, 0x32, 0x00, 0x01, 0x90, 0x00, 0x00, 0x06, 0x00
};

// WeldingDetectionRes - OK
// Template: 809a00122002000000320000
static const uint8_t TPL_WELDING_DETECTION_RES[] = {
    0x80, 0x9a, 0x00, 0x12, 0x20, 0x02, 0x00, 0x00, 
    0x00, 0x32, 0x00, 0x00
};

// SessionStopRes - OK
// Template: 809a00120000
static const uint8_t TPL_SESSION_STOP_RES[] = {
    0x80, 0x9a, 0x00, 0x12, 0x00, 0x00
};

/**
 * Encode a DIN message response
 * Returns length of encoded data, or -1 on error
 */
int exi_encode(din_encode_params_t *params, uint8_t *buffer, int buffer_size) {
    int len = 0;
    
    switch (params->type) {
        case DIN_MSG_SUPPORTED_APP_PROTOCOL_RES:
            len = sizeof(TPL_SUPPORTED_APP_PROTOCOL_RES);
            if (buffer_size < len) return -1;
            memcpy(buffer, TPL_SUPPORTED_APP_PROTOCOL_RES, len);
            break;
            
        case DIN_MSG_SESSION_SETUP_RES:
            len = sizeof(TPL_SESSION_SETUP_RES);
            if (buffer_size < len) return -1;
            memcpy(buffer, TPL_SESSION_SETUP_RES, len);
            // Insert session ID at bytes 4-11
            // (In real implementation, would need proper EXI encoding)
            break;
            
        case DIN_MSG_SERVICE_DISCOVERY_RES:
            len = sizeof(TPL_SERVICE_DISCOVERY_RES);
            if (buffer_size < len) return -1;
            memcpy(buffer, TPL_SERVICE_DISCOVERY_RES, len);
            break;
            
        case DIN_MSG_SERVICE_PAYMENT_SELECTION_RES:
            len = sizeof(TPL_SERVICE_PAYMENT_SELECTION_RES);
            if (buffer_size < len) return -1;
            memcpy(buffer, TPL_SERVICE_PAYMENT_SELECTION_RES, len);
            break;
            
        case DIN_MSG_CONTRACT_AUTHENTICATION_RES:
            len = sizeof(TPL_CONTRACT_AUTHENTICATION_RES);
            if (buffer_size < len) return -1;
            memcpy(buffer, TPL_CONTRACT_AUTHENTICATION_RES, len);
            break;
            
        case DIN_MSG_CHARGE_PARAMETER_DISCOVERY_RES:
            len = sizeof(TPL_CHARGE_PARAMETER_DISCOVERY_RES);
            if (buffer_size < len) return -1;
            memcpy(buffer, TPL_CHARGE_PARAMETER_DISCOVERY_RES, len);
            break;
            
        case DIN_MSG_CABLE_CHECK_RES:
            if (params->processing_finished) {
                len = sizeof(TPL_CABLE_CHECK_RES_FINISHED);
                if (buffer_size < len) return -1;
                memcpy(buffer, TPL_CABLE_CHECK_RES_FINISHED, len);
            } else {
                len = sizeof(TPL_CABLE_CHECK_RES_ONGOING);
                if (buffer_size < len) return -1;
                memcpy(buffer, TPL_CABLE_CHECK_RES_ONGOING, len);
            }
            break;
            
        case DIN_MSG_PRE_CHARGE_RES:
            len = sizeof(TPL_PRE_CHARGE_RES);
            if (buffer_size < len) return -1;
            memcpy(buffer, TPL_PRE_CHARGE_RES, len);
            // Modify present voltage in template
            // Voltage is at bytes 8-10 in a specific bit format
            // For simplicity, leave as template (50V = 0x32)
            // TODO: Proper voltage encoding
            break;
            
        case DIN_MSG_POWER_DELIVERY_RES:
            len = sizeof(TPL_POWER_DELIVERY_RES);
            if (buffer_size < len) return -1;
            memcpy(buffer, TPL_POWER_DELIVERY_RES, len);
            break;
            
        case DIN_MSG_CURRENT_DEMAND_RES:
            len = sizeof(TPL_CURRENT_DEMAND_RES);
            if (buffer_size < len) return -1;
            memcpy(buffer, TPL_CURRENT_DEMAND_RES, len);
            break;
            
        case DIN_MSG_WELDING_DETECTION_RES:
            len = sizeof(TPL_WELDING_DETECTION_RES);
            if (buffer_size < len) return -1;
            memcpy(buffer, TPL_WELDING_DETECTION_RES, len);
            break;
            
        case DIN_MSG_SESSION_STOP_RES:
            len = sizeof(TPL_SESSION_STOP_RES);
            if (buffer_size < len) return -1;
            memcpy(buffer, TPL_SESSION_STOP_RES, len);
            break;
            
        default:
            return -1;
    }
    
    return len;
}

// ============================================================================
// Convenience Wrappers
// ============================================================================

/**
 * Encode message with V2GTP header
 */
int exi_encode_with_header(din_encode_params_t *params, uint8_t *buffer, int buffer_size) {
    uint8_t exi_buf[256];
    
    int exi_len = exi_encode(params, exi_buf, sizeof(exi_buf));
    if (exi_len < 0) return -1;
    
    if (buffer_size < exi_len + 8) return -1;
    
    return exi_add_v2gtp_header(buffer, exi_buf, exi_len);
}

/**
 * Get message type name as string
 */
const char* exi_msg_type_name(din_msg_type_t type) {
    switch (type) {
        case DIN_MSG_SUPPORTED_APP_PROTOCOL_REQ: return "SupportedAppProtocolReq";
        case DIN_MSG_SUPPORTED_APP_PROTOCOL_RES: return "SupportedAppProtocolRes";
        case DIN_MSG_SESSION_SETUP_REQ: return "SessionSetupReq";
        case DIN_MSG_SESSION_SETUP_RES: return "SessionSetupRes";
        case DIN_MSG_SERVICE_DISCOVERY_REQ: return "ServiceDiscoveryReq";
        case DIN_MSG_SERVICE_DISCOVERY_RES: return "ServiceDiscoveryRes";
        case DIN_MSG_SERVICE_PAYMENT_SELECTION_REQ: return "ServicePaymentSelectionReq";
        case DIN_MSG_SERVICE_PAYMENT_SELECTION_RES: return "ServicePaymentSelectionRes";
        case DIN_MSG_CONTRACT_AUTHENTICATION_REQ: return "ContractAuthenticationReq";
        case DIN_MSG_CONTRACT_AUTHENTICATION_RES: return "ContractAuthenticationRes";
        case DIN_MSG_CHARGE_PARAMETER_DISCOVERY_REQ: return "ChargeParameterDiscoveryReq";
        case DIN_MSG_CHARGE_PARAMETER_DISCOVERY_RES: return "ChargeParameterDiscoveryRes";
        case DIN_MSG_CABLE_CHECK_REQ: return "CableCheckReq";
        case DIN_MSG_CABLE_CHECK_RES: return "CableCheckRes";
        case DIN_MSG_PRE_CHARGE_REQ: return "PreChargeReq";
        case DIN_MSG_PRE_CHARGE_RES: return "PreChargeRes";
        case DIN_MSG_POWER_DELIVERY_REQ: return "PowerDeliveryReq";
        case DIN_MSG_POWER_DELIVERY_RES: return "PowerDeliveryRes";
        case DIN_MSG_CURRENT_DEMAND_REQ: return "CurrentDemandReq";
        case DIN_MSG_CURRENT_DEMAND_RES: return "CurrentDemandRes";
        case DIN_MSG_WELDING_DETECTION_REQ: return "WeldingDetectionReq";
        case DIN_MSG_WELDING_DETECTION_RES: return "WeldingDetectionRes";
        case DIN_MSG_SESSION_STOP_REQ: return "SessionStopReq";
        case DIN_MSG_SESSION_STOP_RES: return "SessionStopRes";
        default: return "Unknown";
    }
}
