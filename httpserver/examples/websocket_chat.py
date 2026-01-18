"""
WebSocket Server Example

Demonstrates:
- Starting WebSocket server
- Handling client connections
- Broadcasting messages to all clients
"""

import httpserver
import wsserver
import time

# Track connected clients
clients = set()

def on_connect(client_id):
    """Called when client connects"""
    print(f"Client {client_id} connected")
    clients.add(client_id)
    # Send welcome message
    wsserver.send(client_id, f"Welcome! You are client #{client_id}")
    # Broadcast to others
    for cid in clients:
        if cid != client_id:
            wsserver.send(cid, f"Client {client_id} joined")

def on_disconnect(client_id):
    """Called when client disconnects"""
    print(f"Client {client_id} disconnected")
    clients.discard(client_id)
    # Notify others
    for cid in clients:
        wsserver.send(cid, f"Client {client_id} left")

def on_message(client_id, message):
    """Called when message received"""
    print(f"Client {client_id}: {message}")
    # Broadcast to all clients
    for cid in clients:
        wsserver.send(cid, f"Client {client_id}: {message}")

# Start HTTP server (required for WebSocket)
httpserver.start(80)

# Register WebSocket handlers
wsserver.register_handler('/ws', on_message)
wsserver.on('connect', on_connect)
wsserver.on('disconnect', on_disconnect)

print("WebSocket server running on ws://device-ip/ws")
print("Connect from browser:")
print("  const ws = new WebSocket('ws://device-ip/ws');")
print("  ws.onmessage = (e) => console.log(e.data);")
print("  ws.send('Hello!');")

# Main loop
try:
    while True:
        httpserver.process_queue()
        time.sleep_ms(10)
except KeyboardInterrupt:
    print("\nStopping server...")
    httpserver.stop()
