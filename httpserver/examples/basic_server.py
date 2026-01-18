"""
Basic HTTP Server Example

Demonstrates:
- Starting HTTP server on port 80
- Registering a simple request handler
- Processing requests in main loop
"""

import httpserver
import time

def handle_hello(request):
    """Handle /hello endpoint"""
    return {
        'status': 200,
        'headers': {'Content-Type': 'text/plain'},
        'body': 'Hello from pyDirect!'
    }

def handle_json(request):
    """Handle /api/status endpoint"""
    import json
    status = {
        'uptime': time.time(),
        'free_memory': gc.mem_free(),
        'modules': ['httpserver', 'webfiles', 'wsserver']
    }
    return {
        'status': 200,
        'headers': {'Content-Type': 'application/json'},
        'body': json.dumps(status)
    }

# Start HTTP server
print("Starting HTTP server on port 80...")
httpserver.start(80)

# Register handlers
httpserver.register_handler('/hello', handle_hello, method='GET')
httpserver.register_handler('/api/status', handle_json, method='GET')

print("Server running!")
print("Try: http://device-ip/hello")
print("Try: http://device-ip/api/status")

# Main loop
try:
    while True:
        httpserver.process_queue()
        time.sleep_ms(10)
except KeyboardInterrupt:
    print("\nStopping server...")
    httpserver.stop()
