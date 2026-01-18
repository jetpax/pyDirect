# Implementation Guide: WebSocket Opcode Multiplexing

This guide explains how the WebREPL CB and DAP protocols coexist on a single WebSocket connection using opcode discrimination.

## Architecture Overview

```
┌──────────────────────────────────────────────────────────┐
│  Single WebSocket Connection                              │
│  ws://device:8266/webrepl or wss://device/webrepl        │
├──────────────────────────────────────────────────────────┤
│                                                           │
│  ┌─────────────────────────────────────────────────┐    │
│  │ TEXT Frames (Opcode 0x01)                       │    │
│  │ → Debug Adapter Protocol (DAP)                  │    │
│  │ → Format: Content-Length: N\r\n\r\n{JSON}       │    │
│  │ → Commands: initialize, setBreakpoints, etc.    │    │
│  └─────────────────────────────────────────────────┘    │
│                                                           │
│  ┌─────────────────────────────────────────────────┐    │
│  │ BINARY Frames (Opcode 0x02)                     │    │
│  │ → WebREPL Channelized Binary (WBP)              │    │
│  │ → Format: [channel, opcode, ...fields] (CBOR)   │    │
│  │ → Channels: 0=Events, 1=REPL, 2=M2M, 23=Files  │    │
│  └─────────────────────────────────────────────────┘    │
│                                                           │
└──────────────────────────────────────────────────────────┘
```

## Key Files

### Specifications
- **`webrepl_cb_rfc.md`** - WebREPL Binary Protocol specification (CBOR, channels)
- **`dap_websocket_advocacy.md`** - Technical rationale for DAP over WebSocket
- **`webrepl_rfc.txt`** - Legacy WebREPL protocol (for reference)

### Implementation (pyDirect repository)
- **`httpserver/modwebDAP.c`** - DAP protocol handler (TEXT frames)
- **`httpserver/modwebrepl.c`** - WebREPL CB handler (BINARY frames)
- **`httpserver/modwsserver.c`** - WebSocket transport layer (opcode routing)

### Tools
- **`tools/dap-proxy.py`** - TCP-to-WebSocket bridge for VS Code Desktop
- **`tools/requirements.txt`** - Python dependencies for tools

### Examples
- **`examples/browser_debug_example.html`** - Browser-based demo of both protocols

## How Opcode Discrimination Works

### Server-Side (C code in modwsserver.c)

```c
// WebSocket frame handler
static esp_err_t ws_handler(httpd_req_t *req) {
    httpd_ws_frame_t ws_pkt;
    httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    
    // Opcode discrimination
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        // Route to DAP handler (modwebDAP.c)
        dap_on_message(client_id, data, len, false);
    } 
    else if (ws_pkt.type == HTTPD_WS_TYPE_BINARY) {
        // Route to WebREPL CB handler (modwebrepl.c)
        wbp_on_message(client_id, data, len, true);
    }
}
```

### Handler Modules

**modwebDAP.c** filters by `is_binary` flag:
```c
static void dap_on_message(int client_id, const uint8_t *data, 
                           size_t len, bool is_binary) {
    if (is_binary) {
        return; // Ignore BINARY frames, let WebREPL handle them
    }
    // Parse DAP message (Content-Length + JSON)
    // ...
}
```

**modwebrepl.c** filters by `is_binary` flag:
```c
static void wbp_on_message(int client_id, const uint8_t *data, 
                           size_t len, bool is_binary) {
    if (!is_binary) {
        ESP_LOGW(TAG, "Received TEXT frame (expected BINARY)");
        return; // Ignore TEXT frames, let DAP handle them
    }
    // Parse CBOR message [channel, opcode, ...fields]
    // ...
}
```

**Result:** Clean separation, zero conflicts, no overhead.

## Client-Side Usage

### Browser-Based (JavaScript)

```javascript
// Single WebSocket for everything
const ws = new WebSocket('ws://192.168.4.1:8266/webrepl');
ws.binaryType = 'arraybuffer';

// === DAP Debugging (TEXT frames) ===
function sendDAP(command, args) {
    const msg = {
        seq: seq++,
        type: 'request',
        command: command,
        arguments: args
    };
    const json = JSON.stringify(msg);
    const dapFrame = `Content-Length: ${json.length}\r\n\r\n${json}`;
    
    ws.send(dapFrame); // String → TEXT frame (0x01)
}

// === WebREPL CB (BINARY frames) ===
function sendWebREPL(channel, opcode, data) {
    const message = CBOR.encode([channel, opcode, data]);
    ws.send(message); // ArrayBuffer → BINARY frame (0x02)
}

// === Unified message handler ===
ws.onmessage = (event) => {
    if (typeof event.data === 'string') {
        handleDAP(event.data);     // TEXT frame = DAP
    } else {
        handleWebREPL(event.data); // BINARY frame = WebREPL CB
    }
};

// Use both protocols simultaneously
sendDAP('initialize', { clientID: 'browser-ide' });
sendWebREPL(1, 0, "print('hello')"); // Channel 1, Execute
```

### Desktop IDE (VS Code via Proxy)

```bash
# Terminal 1: Start proxy
python tools/dap-proxy.py --device 192.168.4.1:8266

# Terminal 2: VS Code connects to localhost:5678
# launch.json:
{
    "type": "python",
    "request": "attach",
    "connect": {
        "host": "127.0.0.1",
        "port": 5678
    }
}
```

The proxy transparently bridges TCP ↔ WebSocket TEXT frames.

## Device Setup (MicroPython)

```python
# boot.py on ESP32

import webrepl
import webdap

# Start WebREPL Binary Protocol (BINARY frames)
webrepl.start(password='mypassword')

# Start DAP protocol (TEXT frames)
# Both use the same WebSocket connection!
webdap.start()

print("WebSocket server ready on port 8266")
print("- DAP debugging: TEXT frames (opcode 0x01)")
print("- WebREPL CB: BINARY frames (opcode 0x02)")
```

## Message Flow Examples

### Example 1: Simultaneous REPL and Debugging

```
Browser                     ESP32
  │                           │
  ├─ TEXT: initialize ────────►│ modwebDAP.c handles
  │◄──── TEXT: initialized ────┤
  │                           │
  ├─ BINARY: [1,0,"print()"]─►│ modwebrepl.c handles
  │◄──── BINARY: [1,0,"OK"] ───┤
  │                           │
  ├─ TEXT: setBreakpoints ────►│ modwebDAP.c handles
  │◄──── TEXT: verified ───────┤
  │                           │
  ├─ BINARY: [23,2,"/file"]──►│ modwebrepl.c handles (file upload)
  │◄──── BINARY: [23,4,0] ─────┤
```

**No conflicts** - opcodes discriminate perfectly.

### Example 2: VS Code via Proxy

```
VS Code                Proxy              ESP32
  │                      │                  │
  ├─ TCP: DAP msg ──────►│                  │
  │                      ├─ TEXT: DAP ─────►│ modwebDAP.c
  │                      │◄─ TEXT: response ┤
  │◄──── TCP: response ──┤                  │
```

Proxy only forwards TEXT frames, ignores BINARY.

## Benefits

### 1. Single Connection
- One WebSocket for all operations
- Single authentication
- Unified connection state
- Lower memory footprint

### 2. Browser-Native
- No plugins or permissions needed
- Direct WebSocket access
- Works in all modern browsers
- Native debugging support

### 3. Security
- Single TLS endpoint (wss://)
- Built-in encryption
- Reduced attack surface
- Standard WebSocket security

### 4. Clean Architecture
- Protocol separation via opcodes
- No message inspection needed
- Zero overhead (WebSocket already tracks opcodes)
- Simple, maintainable code

## Comparison with Alternatives

### Alternative 1: Separate TCP Servers
```
ESP32:
- WebSocket :8266 (WebREPL)
- TCP :5678 (DAP)

Problems:
❌ Browser can't access TCP (security)
❌ Two servers = more memory
❌ Two ports = firewall complexity
❌ Two connections = state sync issues
```

### Alternative 2: Message Inspection
```
One WebSocket, discriminate by message content:
- Parse each message to determine protocol
- Check for magic bytes/headers

Problems:
❌ Overhead (must parse every message)
❌ Ambiguity (what if messages look similar?)
❌ Complexity (state machines)
❌ Slower performance
```

### Our Approach: Opcode Multiplexing
```
One WebSocket, discriminate by WebSocket opcode:
- TEXT (0x01) = DAP
- BINARY (0x02) = WebREPL CB

Advantages:
✅ Zero overhead (opcode already in frame)
✅ 100% reliable (no ambiguity)
✅ Browser-native support
✅ Clean code (simple if/else)
```

## Testing the Implementation

### 1. Test WebSocket Connection
```bash
# Use websocat or wscat
websocat ws://192.168.4.1:8266/webrepl
```

### 2. Test DAP Protocol
```bash
# Start proxy
python tools/dap-proxy.py --device 192.168.4.1:8266

# Connect VS Code to localhost:5678
# Set breakpoints, step through code
```

### 3. Test WebREPL CB
```python
import cbor2
import websocket

ws = websocket.create_connection('ws://192.168.4.1:8266/webrepl')

# Send execute command: [channel=1, opcode=0, data="print('test')"]
msg = cbor2.dumps([1, 0, "print('test')"])
ws.send_binary(msg)

response = ws.recv()
print(cbor2.loads(response))
```

### 4. Test Both Simultaneously
Open `examples/browser_debug_example.html` in a browser and try both protocols.

## Troubleshooting

### Issue: TEXT frames not reaching DAP handler
**Solution:** Check that modwebDAP.c is compiled and registered with wsserver.

### Issue: BINARY frames not reaching WebREPL handler
**Solution:** Check that modwebrepl.c is filtering correctly (`if (!is_binary) return;`).

### Issue: Proxy can't connect to device
**Solution:** Verify device IP, check that wsserver is started, check firewall.

### Issue: VS Code can't connect to proxy
**Solution:** Verify proxy is listening on 127.0.0.1:5678, check no other service using port.

## Performance Considerations

### Message Size
- **DAP (TEXT)**: ~20-200 bytes overhead (Content-Length header + JSON)
- **WebREPL CB (BINARY)**: ~4-15 bytes overhead (CBOR positional arrays)

### Memory Usage
- Single WebSocket server: ~8KB
- modwebDAP handler: ~4KB (static data)
- modwebrepl handler: ~12KB (ring buffer + state)
- **Total: ~24KB** vs. ~36KB for separate servers

### Latency
- Opcode discrimination: < 1μs (simple integer compare)
- No parsing overhead until after discrimination
- Near-zero impact on performance

## Future Enhancements

### Phase 1 (Current)
- ✅ Protocol specifications
- ✅ Opcode multiplexing architecture
- ✅ Basic DAP handlers
- ✅ Proxy tool for desktop IDEs

### Phase 2 (In Progress)
- ⏳ sys.settrace() integration for breakpoints
- ⏳ Stack frame inspection
- ⏳ Variable evaluation
- ⏳ ScriptO Studio integration

### Phase 3 (Planned)
- Full DAP protocol support
- Conditional breakpoints
- Watch expressions
- Hot code reload

### Phase 4 (Future)
- VS Code extension (native WebSocket DAP)
- Community adoption
- Influence DAP spec to recognize WebSocket transport

## References

- **WebSocket RFC 6455**: https://www.rfc-editor.org/rfc/rfc6455
- **DAP Specification**: https://microsoft.github.io/debug-adapter-protocol/
- **CBOR RFC 8949**: https://www.rfc-editor.org/rfc/rfc8949
- **WebREPL CB RFC**: `webrepl_cb_rfc.md`
- **DAP WebSocket Advocacy**: `dap_websocket_advocacy.md`

---

**Questions or Issues?**

Jonathan Peace  
Email: jep@alphabetiq.com
GitHub: [@sjetpax](https://github.com/jetpax)
