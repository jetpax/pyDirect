"""
pyDirect - Main Device Orchestrator with WiFi Onboarding
==========================================================

Main entry point for pyDirect firmware with captive portal WiFi onboarding.

Boot flow:
1. Check for WiFi credentials
2. If no credentials: Start captive portal (AP mode)
3. If credentials exist: Connect to WiFi
4. Start HTTP server + WebREPL
5. Start mDNS responder
6. Enter main loop

Captive Portal:
- AP SSID: pyDirect-XXXX (where XXXX = last 4 chars of MAC)
- Setup URL: http://192.168.4.1/setup
- After provisioning: Device connects to WiFi and is available at http://pyDirect-XXXX.local
"""

import network
import time
import httpserver
import webrepl_binary as webrepl
import json

# Import board-specific modules
from lib import board_config
from lib.wifi_manager import WiFiManager
from lib.wifi_onboarding import CaptivePortal

# Configuration
HTTP_PORT = 80
WEBREPL_PASSWORD = "password"  # Change this!
HTTPS_ENABLED = False
HTTPS_CERT_FILE = "/certs/servercert.pem"
HTTPS_KEY_FILE = "/certs/prvtkey.pem"

# Global state
wifi_mgr = None
captive_portal = None
onboarding_mode = False

def start_mdns():
    """Start mDNS responder"""
    try:
        import mdns
        hostname = board_config.get_hostname()
        
        # Start mDNS server
        mdns_server = mdns.Server()
        mdns_server.start(hostname, "pyDirect device")
        
        # Advertise services
        for service_name, service_type, port in board_config.BoardConfig.get_mdns_services():
            mdns_server.advertise_service(service_name, service_type, port)
        
        print(f"✓ mDNS started: {hostname}.local")
        return mdns_server
    except ImportError:
        print("⚠ mDNS not available (module not found)")
        return None
    except Exception as e:
        print(f"⚠ mDNS failed to start: {e}")
        return None

def start_servers():
    """Start HTTP server and WebREPL"""
    print("\nStarting servers...")
    
    # Start HTTP server
    try:
        if HTTPS_ENABLED:
            httpserver.start(HTTP_PORT, cert_file=HTTPS_CERT_FILE, key_file=HTTPS_KEY_FILE)
            print(f"✓ HTTP/HTTPS server started on ports {HTTP_PORT}/443")
        else:
            httpserver.start(HTTP_PORT)
            print(f"✓ HTTP server started on port {HTTP_PORT}")
    except Exception as e:
        print(f"✗ Failed to start HTTP server: {e}")
        return False
    
    # Start WebREPL
    try:
        webrepl.start(password=WEBREPL_PASSWORD, path="/webrepl")
        print(f"✓ WebREPL started")
    except Exception as e:
        print(f"✗ Failed to start WebREPL: {e}")
        return False
    
    return True

def register_onboarding_handlers():
    """Register HTTP handlers for WiFi onboarding"""
    global captive_portal
    
    def handle_setup(request):
        """Serve WiFi setup page"""
        html = captive_portal.get_setup_page_html()
        return {
            'status': 200,
            'headers': {'Content-Type': 'text/html'},
            'body': html
        }
    
    def handle_scan(request):
        """Scan for WiFi networks"""
        networks = wifi_mgr.scan()
        # Convert to JSON-friendly format
        result = []
        for net in networks:
            result.append({
                'ssid': net[0].decode('utf-8') if isinstance(net[0], bytes) else net[0],
                'rssi': net[3],
                'secure': net[4] != 0
            })
        return {
            'status': 200,
            'headers': {'Content-Type': 'application/json'},
            'body': json.dumps(result)
        }
    
    def handle_configure(request):
        """Save WiFi credentials and connect"""
        try:
            # Parse JSON body
            body = request.get('body', '{}')
            data = json.loads(body)
            ssid = data.get('ssid')
            password = data.get('password', '')
            
            if not ssid:
                return {
                    'status': 400,
                    'headers': {'Content-Type': 'application/json'},
                    'body': json.dumps({'success': False, 'error': 'SSID required'})
                }
            
            # Save credentials
            wifi_mgr.save_credentials(ssid, password)
            
            return {
                'status': 200,
                'headers': {'Content-Type': 'application/json'},
                'body': json.dumps({'success': True})
            }
        except Exception as e:
            return {
                'status': 500,
                'headers': {'Content-Type': 'application/json'},
                'body': json.dumps({'success': False, 'error': str(e)})
            }
    
    def handle_success(request):
        """Show success page"""
        # Get SSID from query string
        query = request.get('query', {})
        ssid = query.get('ssid', ['Unknown'])[0]
        
        html = captive_portal.get_success_page_html(ssid)
        return {
            'status': 200,
            'headers': {'Content-Type': 'text/html'},
            'body': html
        }
    
    # Register handlers
    httpserver.register_handler('/setup', handle_setup, method='GET')
    httpserver.register_handler('/api/scan', handle_scan, method='GET')
    httpserver.register_handler('/api/configure', handle_configure, method='POST')
    httpserver.register_handler('/success', handle_success, method='GET')
    
    # Redirect root to setup page
    def handle_root(request):
        return {
            'status': 302,
            'headers': {'Location': '/setup'},
            'body': ''
        }
    httpserver.register_handler('/', handle_root, method='GET')

def main():
    """Main entry point"""
    global wifi_mgr, captive_portal, onboarding_mode
    
    print("\n" + "="*60)
    print("pyDirect - Device Orchestrator")
    print("="*60 + "\n")
    
    # Initialize WiFi manager
    wifi_mgr = WiFiManager()
    hostname = board_config.get_hostname()
    
    print(f"Device: {board_config.BoardConfig.NAME}")
    print(f"Hostname: {hostname}")
    print(f"Chip: {board_config.BoardConfig.CHIP}\n")
    
    # Check for WiFi credentials
    if wifi_mgr.has_credentials():
        print("✓ WiFi credentials found")
        
        # Connect to WiFi
        ip = wifi_mgr.connect()
        
        if ip:
            print(f"✓ WiFi connected: {ip}")
            
            # Start servers
            if not start_servers():
                print("ERROR: Failed to start servers")
                return
            
            # Start mDNS
            mdns_server = start_mdns()
            
            # Print connection info
            print("\n" + "="*60)
            print("pyDirect Ready!")
            print("="*60)
            print(f"Device URL:   http://{hostname}.local/")
            print(f"Device IP:    http://{ip}/")
            print(f"WebREPL:      ws://{hostname}.local/webrepl")
            print(f"Password:     {WEBREPL_PASSWORD}")
            print("="*60 + "\n")
            
            onboarding_mode = False
        else:
            print("✗ WiFi connection failed")
            print("Starting captive portal for re-provisioning...\n")
            wifi_mgr.clear_credentials()
            onboarding_mode = True
    else:
        print("⚠ No WiFi credentials found")
        print("Starting captive portal for provisioning...\n")
        onboarding_mode = True
    
    # Start captive portal if needed
    if onboarding_mode:
        ap_ssid = board_config.get_ap_ssid()
        captive_portal = CaptivePortal(ap_ssid, wifi_mgr, board_config.BoardConfig)
        captive_portal.start()
        
        # Start HTTP server for setup pages
        if not start_servers():
            print("ERROR: Failed to start HTTP server")
            return
        
        # Register onboarding handlers
        register_onboarding_handlers()
    
    # Main loop
    print("Entering main loop (Ctrl+C to exit)...\n")
    try:
        while True:
            # Process HTTP and WebREPL queues
            httpserver.process_queue()
            webrepl.process_queue()
            
            # Process DNS queries if in onboarding mode
            if onboarding_mode and captive_portal:
                captive_portal.process_dns()
            
            time.sleep_ms(10)
            
    except KeyboardInterrupt:
        print("\nShutting down...")
        
        if captive_portal:
            captive_portal.stop()
        
        httpserver.stop()
        webrepl.stop()
        
        print("Goodbye!")

# Auto-run if executed as main
if __name__ == "__main__":
    main()
