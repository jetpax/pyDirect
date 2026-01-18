#!/usr/bin/env python3
"""
DAP WebSocket Proxy
===================

Bridges VS Code (TCP) to ESP32 WebSocket (TEXT frames) for DAP debugging.

This proxy allows standard DAP clients (VS Code, PyCharm, etc.) to debug
MicroPython on ESP32 via WebSocket transport.

Usage:
    python dap-proxy.py --device 192.168.4.1:8266
    python dap-proxy.py --device 192.168.4.1:8266 --port 5678
    python dap-proxy.py --help

Then configure VS Code to connect to localhost:5678 (or your chosen port).

Copyright (c) 2025 Jonathan Peace
SPDX-License-Identifier: MIT
"""

import asyncio
import argparse
import sys

try:
    import websockets
except ImportError:
    print("Error: 'websockets' module not found")
    print("Install with: pip install websockets")
    sys.exit(1)


class DAPWebSocketProxy:
    """
    Proxy that bridges TCP (from IDE) to WebSocket (to ESP32).
    Only forwards TEXT frames (DAP), ignores BINARY frames (WebREPL CB).
    """
    
    def __init__(self, device_host, device_port, listen_host='127.0.0.1', listen_port=5678):
        self.device_host = device_host
        self.device_port = device_port
        self.listen_host = listen_host
        self.listen_port = listen_port
        self.websocket_uri = f"ws://{device_host}:{device_port}/webrepl"
        self.active_connections = 0
    
    async def tcp_to_ws(self, tcp_reader, websocket):
        """Forward TCP data from IDE to WebSocket as TEXT frames"""
        try:
            while True:
                data = await tcp_reader.read(8192)
                if not data:
                    print(f"[TCP→WS] Connection closed by IDE")
                    break
                
                # Forward as TEXT frame (DAP protocol)
                await websocket.send(data.decode('utf-8'))
                print(f"[TCP→WS] Forwarded {len(data)} bytes")
                
        except Exception as e:
            print(f"[TCP→WS] Error: {e}")
    
    async def ws_to_tcp(self, websocket, tcp_writer):
        """Forward WebSocket TEXT frames to TCP (ignore BINARY frames)"""
        try:
            async for message in websocket:
                # Only forward TEXT frames (DAP)
                # Binary frames are WebREPL Binary Protocol - ignore them
                if isinstance(message, str):
                    data = message.encode('utf-8')
                    tcp_writer.write(data)
                    await tcp_writer.drain()
                    print(f"[WS→TCP] Forwarded {len(data)} bytes (TEXT frame)")
                else:
                    # Binary frame - WebREPL Binary Protocol, ignore
                    print(f"[WS→TCP] Ignored {len(message)} bytes (BINARY frame - WebREPL CB)")
                    
        except websockets.exceptions.ConnectionClosed:
            print(f"[WS→TCP] WebSocket closed by device")
        except Exception as e:
            print(f"[WS→TCP] Error: {e}")
    
    async def handle_client(self, tcp_reader, tcp_writer):
        """Handle incoming TCP connection from IDE"""
        client_addr = tcp_writer.get_extra_info('peername')
        print(f"\n[PROXY] New connection from {client_addr}")
        self.active_connections += 1
        
        try:
            # Connect to ESP32 WebSocket
            print(f"[PROXY] Connecting to {self.websocket_uri}")
            async with websockets.connect(self.websocket_uri) as websocket:
                print(f"[PROXY] WebSocket connected, bridging...")
                
                # Bridge bidirectionally
                await asyncio.gather(
                    self.tcp_to_ws(tcp_reader, websocket),
                    self.ws_to_tcp(websocket, tcp_writer)
                )
                
        except websockets.exceptions.WebSocketException as e:
            print(f"[PROXY] WebSocket error: {e}")
        except Exception as e:
            print(f"[PROXY] Error: {e}")
        finally:
            tcp_writer.close()
            await tcp_writer.wait_closed()
            self.active_connections -= 1
            print(f"[PROXY] Connection closed ({self.active_connections} active)")
    
    async def start(self):
        """Start the proxy server"""
        server = await asyncio.start_server(
            self.handle_client,
            self.listen_host,
            self.listen_port
        )
        
        addr = server.sockets[0].getsockname()
        print("=" * 60)
        print("DAP WebSocket Proxy")
        print("=" * 60)
        print(f"Listening on:  {addr[0]}:{addr[1]}")
        print(f"Forwarding to: {self.websocket_uri}")
        print()
        print("Configure your IDE to connect to:")
        print(f"  Host: {addr[0]}")
        print(f"  Port: {addr[1]}")
        print()
        print("VS Code launch.json example:")
        print('{')
        print('    "name": "ESP32 MicroPython",')
        print('    "type": "python",')
        print('    "request": "attach",')
        print('    "connect": {')
        print(f'        "host": "{addr[0]}",')
        print(f'        "port": {addr[1]}')
        print('    }')
        print('}')
        print()
        print("Press Ctrl+C to stop")
        print("=" * 60)
        print()
        
        async with server:
            await server.serve_forever()


def main():
    parser = argparse.ArgumentParser(
        description='DAP WebSocket Proxy - Bridge VS Code to ESP32 WebSocket',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s --device 192.168.4.1:8266
  %(prog)s --device esp32.local:8266 --port 5678
  
Notes:
  - Only TEXT frames (DAP) are forwarded to IDE
  - BINARY frames (WebREPL CB) are ignored
  - Multiple concurrent connections supported
        """
    )
    
    parser.add_argument(
        '--device',
        required=True,
        metavar='HOST:PORT',
        help='ESP32 device address (e.g., 192.168.4.1:8266 or esp32.local:8266)'
    )
    
    parser.add_argument(
        '--port',
        type=int,
        default=5678,
        metavar='PORT',
        help='Local TCP port to listen on (default: 5678)'
    )
    
    parser.add_argument(
        '--host',
        default='127.0.0.1',
        metavar='HOST',
        help='Local host to listen on (default: 127.0.0.1)'
    )
    
    args = parser.parse_args()
    
    # Parse device address
    try:
        if ':' in args.device:
            device_host, device_port = args.device.rsplit(':', 1)
            device_port = int(device_port)
        else:
            device_host = args.device
            device_port = 8266
    except ValueError:
        print(f"Error: Invalid device address '{args.device}'")
        print("Format: HOST:PORT (e.g., 192.168.4.1:8266)")
        sys.exit(1)
    
    # Create and start proxy
    proxy = DAPWebSocketProxy(
        device_host=device_host,
        device_port=device_port,
        listen_host=args.host,
        listen_port=args.port
    )
    
    try:
        asyncio.run(proxy.start())
    except KeyboardInterrupt:
        print("\n[PROXY] Shutting down...")
    except Exception as e:
        print(f"[PROXY] Fatal error: {e}")
        sys.exit(1)


if __name__ == '__main__':
    main()
