"""
pyDirect - Minimal Main Script
================================

Minimal device orchestrator that starts HTTP server and WebREPL.
For full Scripto Studio integration, use install-scripto-studio.sh

This script:
1. Connects to WiFi (if configured)
2. Starts HTTP server on port 80
3. Starts WebREPL
4. Enters main loop to process queues

Copyright (c) 2025 Jonathan Peace
SPDX-License-Identifier: MIT
"""

import network
import time
import httpserver
import webrepl_binary as webrepl

# ============================================================================
# Configuration
# ============================================================================

# Server Configuration
HTTP_PORT = 80
WEBREPL_PASSWORD = "password"  # Change this!

# HTTPS Configuration (optional)
HTTPS_ENABLED = False
HTTPS_CERT_FILE = "/certs/servercert.pem"
HTTPS_KEY_FILE = "/certs/prvtkey.pem"

# ============================================================================
# Network Setup
# ============================================================================

def get_ap_name():
    """Generate AP name from MAC address"""
    import ubinascii
    wlan = network.WLAN(network.AP_IF)
    mac = ubinascii.hexlify(wlan.config('mac'), ':').decode()
    # Use last 4 chars of MAC for unique AP name
    suffix = mac.replace(':', '')[-4:].upper()
    return f"pyDirect-{suffix}"

def start_ap_mode():
    """Start AP mode with captive portal for WiFi onboarding"""
    ap = network.WLAN(network.AP_IF)
    ap.active(True)
    
    ap_name = get_ap_name()
    ap.config(essid=ap_name)
    ap.config(authmode=network.AUTH_OPEN)  # Open AP for easy onboarding
    
    # Wait for AP to be active
    while not ap.active():
        time.sleep(0.1)
    
    ip = ap.ifconfig()[0]
    print(f"AP Mode started: {ap_name}")
    print(f"AP IP: {ip}")
    print(f"Connect to '{ap_name}' and navigate to http://{ip}/")
    return ip

def load_wifi_config():
    """Load WiFi credentials from saved config file"""
    try:
        with open('/wifi_config.txt', 'r') as f:
            lines = f.readlines()
            if len(lines) >= 2:
                return lines[0].strip(), lines[1].strip()
    except:
        pass
    return None, None

def connect_wifi():
    """Connect to WiFi if configured, otherwise start AP mode"""
    # Try to load saved WiFi config
    ssid, password = load_wifi_config()
    
    if not ssid:
        print("WiFi not configured - starting AP mode for onboarding")
        return start_ap_mode()
    
    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    
    if wlan.isconnected():
        print(f"Already connected: {wlan.ifconfig()[0]}")
        return wlan.ifconfig()[0]
    
    print(f"Connecting to WiFi: {ssid}...")
    wlan.connect(ssid, password)
    
    # Wait for connection (30 second timeout)
    timeout = 30
    while not wlan.isconnected() and timeout > 0:
        time.sleep(1)
        timeout -= 1
    
    if wlan.isconnected():
        ip = wlan.ifconfig()[0]
        print(f"WiFi connected: {ip}")
        return ip
    else:
        print("WiFi connection failed - starting AP mode")
        return start_ap_mode()


# ============================================================================
# Server Startup
# ============================================================================

def start_servers():
    """Start HTTP server and WebREPL"""
    print("Starting servers...")
    
    # Start HTTP server
    try:
        if HTTPS_ENABLED:
            httpserver.start(HTTP_PORT, cert_file=HTTPS_CERT_FILE, key_file=HTTPS_KEY_FILE)
            print(f"HTTP/HTTPS server started on ports {HTTP_PORT}/443")
        else:
            httpserver.start(HTTP_PORT)
            print(f"HTTP server started on port {HTTP_PORT}")
    except Exception as e:
        print(f"Failed to start HTTP server: {e}")
        return False
    
    # Start WebREPL
    try:
        webrepl.start(password=WEBREPL_PASSWORD, path="/webrepl")
        print("WebREPL started")
    except Exception as e:
        print(f"Failed to start WebREPL: {e}")
        return False
    
    return True

# ============================================================================
# Main Loop
# ============================================================================

def main():
    """Main entry point"""
    print("")
    print("=" * 60)
    print("pyDirect - Minimal Device Orchestrator")
    print("=" * 60)
    print("")
    
    # Check for Improv WiFi provisioning mode
    # Only run if WiFi is not configured
    try:
        import improv_serial
        if not improv_serial.is_wifi_configured():
            print("No WiFi configured - starting Improv provisioning...")
            print("Use ESP Web Tools to configure WiFi via browser")
            print("")
            
            # Try Improv provisioning (30 second timeout)
            if improv_serial.start_listener(timeout=30):
                print("WiFi provisioned via Improv!")
            else:
                print("Improv provisioning timeout - continuing with AP mode")
            print("")
    except ImportError:
        pass  # improv_serial module not available
    except Exception as e:
        print(f"Improv error: {e}")

    
    # Connect to network
    ip = connect_wifi()
    
    # Start servers
    if not start_servers():
        print("ERROR: Failed to start servers")
        return
    
    # Print connection info
    print("")
    print("=" * 60)
    print("pyDirect Ready!")
    print("=" * 60)
    if ip:
        print(f"Device IP:    {ip}")
        print(f"HTTP Server:  http://{ip}/")
        print(f"WebREPL:      ws://{ip}/webrepl")
    else:
        print("No network connection - servers running on localhost only")
    print(f"Password:     {WEBREPL_PASSWORD}")
    print("")
    print("For full Scripto Studio integration:")
    print("  ./install-scripto-studio.sh /dev/ttyUSB0")
    print("=" * 60)
    print("")
    
    # Main loop - process queues
    print("Entering main loop (Ctrl+C to exit)...")
    try:
        while True:
            httpserver.process_queue()
            webrepl.process_queue()
            time.sleep_ms(10)
    except KeyboardInterrupt:
        print("\nStopping servers...")
        httpserver.stop()
        webrepl.stop()
        print("Goodbye!")

# Auto-run if executed as main
if __name__ == "__main__":
    main()
