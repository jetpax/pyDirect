"""
Improv WiFi Serial Protocol Implementation for MicroPython
https://www.improv-wifi.com/serial/

Enables WiFi provisioning via serial connection for ESP Web Tools.

Copyright (c) 2025 Jonathan Peace
SPDX-License-Identifier: MIT

"""

import sys
import struct
import time
import network
import machine

# Packet types
TYPE_CURRENT_STATE = 0x01
TYPE_ERROR_STATE = 0x02
TYPE_RPC_COMMAND = 0x03
TYPE_RPC_RESULT = 0x04

# RPC Commands
RPC_SEND_WIFI_SETTINGS = 0x01
RPC_REQUEST_CURRENT_STATE = 0x02
RPC_REQUEST_DEVICE_INFO = 0x03
RPC_REQUEST_SCAN_NETWORKS = 0x04

# States
STATE_AUTHORIZATION_REQUIRED = 0x01
STATE_AUTHORIZED = 0x02
STATE_PROVISIONING = 0x03
STATE_PROVISIONED = 0x04

# Error codes
ERROR_NONE = 0x00
ERROR_INVALID_RPC = 0x01
ERROR_UNKNOWN_COMMAND = 0x02
ERROR_UNABLE_TO_CONNECT = 0x03

# Global state
current_state = STATE_AUTHORIZED  # Auto-authorize for simplicity
current_error = ERROR_NONE


def calculate_checksum(data):
    """Calculate Improv packet checksum (sum of all bytes)."""
    return sum(data) & 0xFF


def send_packet(packet_type, data=b''):
    """Send an Improv packet to the client."""
    # Build packet: IMPROV + version + type + length + data + checksum
    header = b'IMPROV'
    version = bytes([0x01])
    ptype = bytes([packet_type])
    length = bytes([len(data)])
    
    # Calculate checksum over version + type + length + data
    checksum_data = version + ptype + length + data
    checksum = bytes([calculate_checksum(checksum_data)])
    
    packet = header + checksum_data + checksum
    sys.stdout.buffer.write(packet)
    sys.stdout.buffer.flush()


def send_state(state):
    """Send current state packet."""
    global current_state
    current_state = state
    send_packet(TYPE_CURRENT_STATE, bytes([state]))


def send_error(error_code):
    """Send error state packet."""
    global current_error
    current_error = error_code
    send_packet(TYPE_ERROR_STATE, bytes([error_code]))


def send_rpc_result(strings):
    """Send RPC result packet with list of strings."""
    data = b''
    for s in strings:
        s_bytes = s.encode('utf-8')
        data += bytes([len(s_bytes)]) + s_bytes
    send_packet(TYPE_RPC_RESULT, data)


def parse_strings(data):
    """Parse length-prefixed strings from data."""
    strings = []
    offset = 0
    while offset < len(data):
        if offset >= len(data):
            break
        length = data[offset]
        offset += 1
        if offset + length > len(data):
            break
        string = data[offset:offset + length].decode('utf-8')
        strings.append(string)
        offset += length
    return strings


def get_device_info():
    """Get device information for RPC response."""
    # Firmware name
    firmware_name = "pyDirect"
    
    # Firmware version (read from .micropython-version if available)
    try:
        with open('/.micropython-version', 'r') as f:
            firmware_version = f.read().strip()
    except:
        firmware_version = "1.27.0"
    
    # Hardware chip
    chip_type = "ESP32-S3"  # Could detect dynamically if needed
    
    # Device name (use MAC address for uniqueness)
    mac = network.WLAN(network.STA_IF).config('mac')
    device_name = "pyDirect-{:02X}{:02X}".format(mac[4], mac[5])
    
    return [firmware_name, firmware_version, chip_type, device_name]


def scan_wifi_networks():
    """Scan for WiFi networks and return results."""
    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    
    try:
        networks = wlan.scan()
        results = []
        
        for ssid, bssid, channel, rssi, authmode, hidden in networks:
            ssid_str = ssid.decode('utf-8') if isinstance(ssid, bytes) else ssid
            rssi_str = str(rssi)
            auth_str = "YES" if authmode != 0 else "NO"
            results.append([ssid_str, rssi_str, auth_str])
        
        return results
    except Exception as e:
        print(f"WiFi scan error: {e}")
        return []


def connect_wifi(ssid, password):
    """Connect to WiFi network with given credentials."""
    global current_state
    
    send_state(STATE_PROVISIONING)
    
    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    
    try:
        wlan.connect(ssid, password)
        
        # Wait for connection (max 10 seconds)
        for _ in range(20):
            if wlan.isconnected():
                send_state(STATE_PROVISIONED)
                
                # Get device IP and send redirect URL
                ip = wlan.ifconfig()[0]
                redirect_url = f"http://{ip}/"
                send_rpc_result([redirect_url])
                
                # Save WiFi credentials for next boot
                try:
                    with open('/wifi_config.txt', 'w') as f:
                        f.write(f"{ssid}\n{password}\n")
                except:
                    pass
                
                return True
            time.sleep(0.5)
        
        # Connection timeout
        send_error(ERROR_UNABLE_TO_CONNECT)
        send_state(STATE_AUTHORIZED)
        return False
        
    except Exception as e:
        print(f"WiFi connection error: {e}")
        send_error(ERROR_UNABLE_TO_CONNECT)
        send_state(STATE_AUTHORIZED)
        return False


def handle_rpc_command(command_id, data):
    """Handle incoming RPC command."""
    send_error(ERROR_NONE)  # Clear any previous errors
    
    if command_id == RPC_SEND_WIFI_SETTINGS:
        # Parse SSID and password
        strings = parse_strings(data)
        if len(strings) >= 2:
            ssid, password = strings[0], strings[1]
            print(f"Improv: Connecting to '{ssid}'...")
            return connect_wifi(ssid, password)
        else:
            send_error(ERROR_INVALID_RPC)
            
    elif command_id == RPC_REQUEST_CURRENT_STATE:
        send_state(current_state)
        
    elif command_id == RPC_REQUEST_DEVICE_INFO:
        device_info = get_device_info()
        send_rpc_result(device_info)
        
    elif command_id == RPC_REQUEST_SCAN_NETWORKS:
        networks = scan_wifi_networks()
        # Send each network as a separate RPC result
        for network_info in networks:
            send_rpc_result(network_info)
        # Send empty result to indicate end of list
        send_rpc_result([])
        
    else:
        send_error(ERROR_UNKNOWN_COMMAND)
    
    return False


def read_packet():
    """Read and parse an Improv packet from stdin."""
    # Read header "IMPROV"
    header = sys.stdin.buffer.read(6)
    if header != b'IMPROV':
        return None
    
    # Read version, type, length
    meta = sys.stdin.buffer.read(3)
    if len(meta) != 3:
        return None
    
    version, packet_type, length = meta
    
    # Read data
    data = sys.stdin.buffer.read(length) if length > 0 else b''
    
    # Read checksum
    checksum_byte = sys.stdin.buffer.read(1)
    if len(checksum_byte) != 1:
        return None
    
    # Verify checksum
    expected_checksum = calculate_checksum(meta + data)
    if checksum_byte[0] != expected_checksum:
        print("Improv: Checksum mismatch")
        return None
    
    return (packet_type, data)


def start_listener(timeout=30):
    """
    Start Improv WiFi provisioning listener.
    
    Blocks until WiFi is provisioned or timeout is reached.
    Returns True if WiFi was successfully provisioned.
    """
    print("Improv WiFi Serial: Listening for provisioning commands...")
    print("Timeout: {} seconds".format(timeout))
    
    # Send initial state
    send_state(STATE_AUTHORIZED)
    
    start_time = time.time()
    
    # Set stdin to non-blocking mode
    import select
    poll = select.poll()
    poll.register(sys.stdin, select.POLLIN)
    
    while True:
        # Check timeout
        if time.time() - start_time > timeout:
            print("Improv: Timeout reached, exiting provisioning mode")
            return False
        
        # Poll for input (100ms timeout)
        events = poll.poll(100)
        
        if events:
            try:
                packet = read_packet()
                if packet:
                    packet_type, data = packet
                    
                    if packet_type == TYPE_RPC_COMMAND:
                        if len(data) > 0:
                            command_id = data[0]
                            command_data = data[1:]
                            
                            # Handle command and check if WiFi was provisioned
                            if handle_rpc_command(command_id, command_data):
                                print("Improv: WiFi provisioned successfully!")
                                return True
                                
            except Exception as e:
                print(f"Improv: Packet processing error: {e}")
                send_error(ERROR_INVALID_RPC)


def is_wifi_configured():
    """Check if WiFi credentials are already saved."""
    try:
        with open('/wifi_config.txt', 'r') as f:
            return True
    except:
        return False
