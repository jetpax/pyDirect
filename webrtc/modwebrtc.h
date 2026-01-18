/*
 * modwebrtc.h - WebRTC Module C API
 * 
 * Provides C-level access to WebRTC peer objects for use by other C modules.
 * 
 * Copyright (c) 2025 Jonathan Peace
 * SPDX-License-Identifier: MIT
 */

#ifndef MOD_WEBRTC_H
#define MOD_WEBRTC_H

#include "py/obj.h"
#include "esp_peer.h"

/**
 * Extract the ESP peer handle from a MicroPython webrtc.Peer object
 * 
 * @param peer_obj MicroPython peer object (webrtc.Peer instance)
 * @return ESP peer handle, or NULL if invalid
 */
esp_peer_handle_t webrtc_peer_get_handle(mp_obj_t peer_obj);

/**
 * Check if the data channel is open for a peer object
 * 
 * @param peer_obj MicroPython peer object (webrtc.Peer instance)
 * @return true if data channel is open, false otherwise
 */
bool webrtc_peer_is_data_channel_open(mp_obj_t peer_obj);

/**
 * Send raw bytes via WebRTC DataChannel (C-to-C, no Python overhead)
 * 
 * This is a convenience wrapper around esp_peer_send_data() that constructs
 * the frame structure automatically. Useful for C modules that need to send
 * data from background tasks without Python/GIL overhead.
 * 
 * @param handle ESP peer handle (from webrtc_peer_get_handle())
 * @param data Raw bytes to send
 * @param len Length of data
 * @return 0 on success, non-zero on error
 */
int webrtc_peer_send_raw(esp_peer_handle_t handle, const uint8_t *data, size_t len);

#endif // MOD_WEBRTC_H
