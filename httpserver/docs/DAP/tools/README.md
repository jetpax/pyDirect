# WebREPL Tools

This directory contains helper tools for WebREPL development and debugging.

## dap-proxy.py

**DAP WebSocket Proxy** - Bridges standard DAP clients (VS Code, PyCharm) to ESP32 WebSocket transport.

### Purpose

The Debug Adapter Protocol (DAP) is typically transported over TCP sockets. Our ESP32 implementation uses WebSocket TEXT frames for DAP to enable browser-based debugging. This proxy bridges the gap for desktop IDE users.

### Requirements

```bash
pip install websockets
```

### Usage

Start the proxy:

```bash
# Basic usage
python dap-proxy.py --device 192.168.4.1:8266

# Custom port
python dap-proxy.py --device 192.168.4.1:8266 --port 5678

# Using mDNS hostname
python dap-proxy.py --device esp32.local:8266
```

Configure VS Code (`launch.json`):

```json
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "ESP32 MicroPython Debug",
            "type": "python",
            "request": "attach",
            "connect": {
                "host": "127.0.0.1",
                "port": 5678
            },
            "pathMappings": [{
                "localRoot": "${workspaceFolder}",
                "remoteRoot": "/"
            }]
        }
    ]
}
```

Press F5 to start debugging!

### How It Works

```
VS Code (TCP :5678)
    ↓
dap-proxy.py
    ↓
ESP32 (WebSocket :8266)
    ├─ TEXT frames (0x01) → DAP debugging
    └─ BINARY frames (0x02) → WebREPL CB (REPL/files)
```

The proxy:
1. Accepts TCP connections from VS Code on localhost:5678
2. Connects to ESP32 WebSocket endpoint
3. Forwards VS Code DAP messages as WebSocket TEXT frames
4. Forwards WebSocket TEXT frames back to VS Code
5. Ignores BINARY frames (WebREPL Binary Protocol)

### Features

- ✅ Bidirectional message forwarding
- ✅ Multiple concurrent connections
- ✅ Automatic frame type discrimination
- ✅ Clean error handling and logging
- ✅ Zero configuration (sensible defaults)

### Troubleshooting

**Connection refused:**
- Check ESP32 is powered on and WebREPL is started
- Verify device address (use `ping 192.168.4.1` or mDNS)
- Check firewall settings

**VS Code can't connect:**
- Verify proxy is running and shows "Listening on..."
- Check port 5678 isn't already in use
- Confirm VS Code launch.json has correct host/port

**Binary frames appearing in log:**
- This is normal! WebREPL CB uses binary frames
- Only TEXT frames are forwarded to VS Code
- Binary frames are for REPL/file operations

### For Browser-Based IDEs

**If you're using ScriptO Studio or another web-based IDE:**

You don't need this proxy! Browser-based IDEs can connect directly to the WebSocket:

```javascript
const ws = new WebSocket('ws://192.168.4.1:8266/webrepl');

// Send DAP as TEXT frames
ws.send(`Content-Length: ${json.length}\r\n\r\n${json}`);

// Send WebREPL as BINARY frames
ws.send(CBOR.encode([channel, opcode, data]));
```

See `dap_websocket_advocacy.md` for the full rationale.

## Future Tools

- `webrepl-client.py` - CLI WebREPL client
- `file-sync.py` - Bidirectional file synchronization
- `mpy-compiler.py` - Local .mpy compilation

---

**See also:**
- [WebREPL CB RFC](../webrepl_cb_rfc.md) - Protocol specification
- [DAP WebSocket Advocacy](../dap_websocket_advocacy.md) - Why WebSocket for DAP
