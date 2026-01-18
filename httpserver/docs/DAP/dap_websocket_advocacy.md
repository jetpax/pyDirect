# Debug Adapter Protocol over WebSocket: Design Rationale

**Protocol Name:** DAP-over-WebSocket  
**Use Case:** MicroPython debugging on ESP32 via browser-based IDEs  
**Status:** Proposed  
**Date:** December 2025  
**Author:** Jonathan Peace, Scripto Studio

---

## Executive Summary

This document explains the design choice to implement the Debug Adapter Protocol (DAP) over WebSocket TEXT frames (opcode 0x01) for MicroPython on ESP32, multiplexed with WebREPL Binary Protocol on the same WebSocket connection using opcode discrimination.

**Key Points:**
- Browser-based IDEs cannot access raw TCP sockets
- WebSocket is the only viable transport for browser debugging
- Opcode multiplexing enables elegant protocol coexistence
- This approach is novel in the embedded/MicroPython ecosystem
- Desktop IDE users can use a simple proxy adapter

---

## Table of Contents

1. [The Browser Security Model](#the-browser-security-model)
2. [Why WebSocket for DAP](#why-websocket-for-dap)
3. [Protocol Multiplexing via WebSocket Opcodes](#protocol-multiplexing-via-websocket-opcodes)
4. [Comparison with Existing Solutions](#comparison-with-existing-solutions)
5. [Client Compatibility Strategy](#client-compatibility-strategy)
6. [Implementation Benefits](#implementation-benefits)
7. [Trade-offs and Considerations](#trade-offs-and-considerations)
8. [Future Directions](#future-directions)

---

## The Browser Security Model

### Browser Network Access Restrictions

Modern browsers enforce strict security policies that fundamentally constrain network access:

#### What Browsers CAN Access:
- ✅ **HTTP/HTTPS** - Standard web requests
- ✅ **WebSocket** - Bidirectional communication (ws:// and wss://)
- ✅ **WebRTC** - Peer-to-peer data channels (requires signaling)

#### What Browsers CANNOT Access:
- ❌ **Raw TCP sockets** - Direct socket connections blocked
- ❌ **Raw UDP sockets** - Direct datagram access blocked
- ❌ **Arbitrary ports** - Cannot scan or probe network

This is **fundamental browser security** - allowing raw TCP access would enable:
- Port scanning of local networks
- Unauthorized service connections
- Cross-protocol attacks
- Privacy violations

### Real-World Examples

#### VS Code Web (vscode.dev)
```
Browser ←[WebSocket]→ GitHub Codespaces (Cloud VM)
                           ↓
                      [Local TCP debugger]
```
**vscode.dev cannot debug local applications directly** - it requires either:
1. Cloud backend (Codespaces) to run debuggers
2. Desktop VS Code tunnel as a bridge
3. File System Access API (local files only, no debugging)

#### Arduino Web Editor
- Uses WebSerial API (Chrome-only, requires user permission)
- No debugging support at all
- Only serial upload and monitor

#### CircuitPython Web Workflow
- Uses WebSocket for REPL
- No debugger integration

**No browser-based IDE has native TCP debugging** - they all face the same constraint.

---

## Why WebSocket for DAP

### Standard DAP Transports

The Debug Adapter Protocol specification defines:

1. **stdin/stdout** - Subprocess communication (local only)
2. **TCP sockets** - Network debugging (common for remote debugging)

**WebSocket is NOT in the official DAP specification.**

### Why WebSocket is Necessary for Browser IDEs

For **browser-based development tools** (like ScriptO Studio):

```javascript
// This WORKS (browser-native):
const ws = new WebSocket('ws://192.168.4.1:8266/webrepl');
ws.send(dap_message); // Can send DAP over WebSocket

// This FAILS (browser security blocks it):
const socket = net.connect(5678, '192.168.4.1'); // ❌ Not available
```

**WebSocket is not just convenient - it's the ONLY option** for browser-based debugging.

### Why WebSocket Makes Sense for ESP32

Even beyond browser constraints, WebSocket offers advantages for ESP32:

#### Traditional Approach (TCP DAP):
```
ESP32 Resources:
- HTTP/HTTPS server (for web interface)
- WebSocket server (for WebREPL)  
- TCP server (for DAP)           ← Extra server needed
- MDNS service
```

#### WebSocket Opcode Multiplexing:
```
ESP32 Resources:
- HTTP/HTTPS server (for web interface)
- WebSocket server (for WebREPL + DAP) ← Shared resource
- MDNS service
```

**Benefits:**
- ✅ Fewer server processes (lower memory)
- ✅ Single port (simpler networking/firewall)
- ✅ Unified connection state
- ✅ Built-in TLS support via wss://
- ✅ Native browser support

---

## Protocol Multiplexing via WebSocket Opcodes

### WebSocket Frame Types (RFC 6455)

WebSocket defines frame types (opcodes) for different message semantics:

| Opcode | Type | Purpose |
|--------|------|---------|
| 0x00 | Continuation | Multi-frame message continuation |
| **0x01** | **Text** | UTF-8 text data |
| **0x02** | **Binary** | Binary data |
| 0x08 | Close | Connection termination |
| 0x09 | Ping | Keep-alive ping |
| 0x0A | Pong | Keep-alive response |

### Our Multiplexing Strategy

We use **opcode discrimination** to run multiple protocols on one WebSocket:

```
┌───────────────────────────────────────┐
│  ws://device:8266/webrepl             │
├───────────────────────────────────────┤
│  TEXT frames (0x01):                  │
│  → DAP Protocol (JSON with headers)   │
│  → Content-Length: N\r\n\r\n{...}     │
├───────────────────────────────────────┤
│  BINARY frames (0x02):                │
│  → WebREPL Binary Protocol (CBOR arrays)  │
│  → [channel, opcode, ...fields]       │
└───────────────────────────────────────┘
```

### Implementation Pattern

**Server-side discrimination** (C code):

```c
// WebSocket frame handler
static void ws_handler(httpd_req_t *req) {
    httpd_ws_frame_t ws_pkt;
    httpd_ws_recv_frame(req, &ws_pkt, ...);
    
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        // Route to DAP handler
        dap_on_message(client_id, data, len, false);
    } else if (ws_pkt.type == HTTPD_WS_TYPE_BINARY) {
        // Route to WebREPL CB handler  
        wbp_on_message(client_id, data, len, true);
    }
}
```

**Client-side discrimination** (JavaScript):

```javascript
// Single WebSocket, two protocols
const ws = new WebSocket('ws://device/webrepl');

// Send DAP message (TEXT frame)
function sendDAP(msg) {
    const json = JSON.stringify(msg);
    const dap = `Content-Length: ${json.length}\r\n\r\n${json}`;
    ws.send(dap); // String → TEXT frame
}

// Send WebREPL message (BINARY frame)
function sendWebREPL(channel, opcode, data) {
    const cbor = CBOR.encode([channel, opcode, data]);
    ws.send(cbor); // ArrayBuffer → BINARY frame
}

// Unified receive handler
ws.onmessage = (event) => {
    if (typeof event.data === 'string') {
        handleDAP(event.data);     // TEXT frame
    } else {
        handleWebREPL(event.data); // BINARY frame
    }
};
```

### Why This is Elegant

1. **No ambiguity** - Opcode discrimination is 100% reliable
2. **No overhead** - WebSocket already tracks frame types
3. **No conflicts** - Protocols never see each other's messages
4. **Native support** - Browsers handle opcode automatically
5. **Clean code** - Simple if/else, no complex parsing

---

## Comparison with Existing Solutions

### ESP-IDF Debug Adapter (esp_debug_adapter)

**Architecture:**
```python
# Python DAP server bridges VS Code to OpenOCD
# Port: 43474 (TCP)
# Debugs: Native C/C++ ESP32 applications via GDB/JTAG
debug_adapter_main.py --port 43474 --device-name Esp32
```

**Pros:**
- ✅ Full-featured DAP implementation (reference implementation)
- ✅ VS Code Desktop works natively
- ✅ Standard DAP transport (TCP)
- ✅ Mature, production-ready
- ✅ GDB-level debugging (registers, assembly, etc.)

**Cons:**
- ❌ Browser IDEs cannot connect (TCP security)
- ❌ Debugs native code, not Python/MicroPython scripts
- ❌ Requires OpenOCD + JTAG hardware
- ❌ Two separate connections (debug + serial)

**Target Use Case:**
- ESP-IDF C/C++ application development
- Hardware-level debugging
- Production firmware debugging

**Client Compatibility:**
- ✅ VS Code Desktop
- ✅ Any desktop IDE with DAP support
- ❌ vscode.dev (browser)
- ❌ Any web-based IDE

**Relationship to Our Project:**

ESP-IDF debug adapter and our WebDAP serve **different purposes at different layers**:

```
┌────────────────────────────────────────────────────┐
│  ESP32 Software Stack                              │
├────────────────────────────────────────────────────┤
│  Python Scripts (.py files)                        │
│  ↑ Debugged by: WebDAP (sys.settrace)            │
├────────────────────────────────────────────────────┤
│  MicroPython Interpreter (C code)                  │
│  ↑ Debugged by: esp_debug_adapter (GDB)           │
├────────────────────────────────────────────────────┤
│  ESP-IDF Framework (C code)                        │
│  ↑ Debugged by: esp_debug_adapter (GDB)           │
├────────────────────────────────────────────────────┤
│  Hardware (CPU, peripherals)                       │
└────────────────────────────────────────────────────┘
```

Both can coexist! You could use:
- **esp_debug_adapter** to debug MicroPython interpreter itself (C level)
- **WebDAP** to debug Python scripts running in MicroPython

### mp_debugpy (TCP Approach)

**Architecture:**
```python
# ESP32 runs TCP server for DAP
import socket
dap_server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
dap_server.bind(('0.0.0.0', 5678))
dap_server.listen(1)
```

**Pros:**
- ✅ VS Code Desktop works natively
- ✅ Standard DAP transport
- ✅ Well-documented approach

**Cons:**
- ❌ Browser IDEs cannot connect (security)
- ❌ Requires separate TCP server
- ❌ Two ports to manage (WebREPL + DAP)
- ❌ Two connections = separate state

**Client Compatibility:**
- ✅ VS Code Desktop
- ✅ PyCharm, IntelliJ
- ❌ ScriptO Studio (browser)
- ❌ Any web-based IDE

### Arduino Lab / ESP Launchpad (WebSerial)

**Architecture:**
```javascript
// Chrome-only WebSerial API
const port = await navigator.serial.requestPort();
await port.open({ baudRate: 115200 });
```

**Pros:**
- ✅ Direct serial access from browser
- ✅ No server needed on device

**Cons:**
- ❌ Chrome-only (WebSerial not in all browsers)
- ❌ Requires user permission prompt
- ❌ Serial-only (no WiFi debugging)
- ❌ No DAP support at all

### CircuitPython Web Workflow (WebSocket REPL)

**Architecture:**
- WebSocket for REPL only
- No debugger integration
- No protocol multiplexing

**Pros:**
- ✅ Browser-native WebSocket
- ✅ Wireless REPL

**Cons:**
- ❌ No debugging support
- ❌ REPL only (no file transfer optimization)

### Our Approach (WebSocket Opcode Multiplexing)

**Architecture:**
```
Single WebSocket with:
- TEXT frames (0x01) → DAP
- BINARY frames (0x02) → WebREPL CB
```

**Pros:**
- ✅ Browser-native (no plugins, no prompts)
- ✅ Single connection for REPL + Files + Debug
- ✅ Works over WiFi (wireless debugging)
- ✅ WSS encryption built-in
- ✅ No separate ports/servers
- ✅ Novel in MicroPython ecosystem

**Cons:**
- ⚠️ VS Code Desktop needs proxy adapter
- ⚠️ Not in official DAP spec (custom transport)

**Client Compatibility:**
- ✅ ScriptO Studio (native)
- ✅ Any browser-based IDE (native)
- ⚠️ VS Code Desktop (via simple proxy)
- ⚠️ Desktop IDEs (via proxy or extension)

### Comparison Matrix

| Feature | ESP-IDF Adapter | mp_debugpy (TCP) | WebSerial | Our WebSocket |
|---------|-----------------|------------------|-----------|---------------|
| **Target** | C/C++ native | Python scripts | Serial only | Python scripts |
| **Debug Level** | Hardware/GDB | Python/settrace | N/A | Python/settrace |
| **Browser IDE Support** | ❌ TCP | ❌ TCP | ⚠️ Chrome only | ✅ All browsers |
| **Desktop IDE Support** | ✅ Native | ✅ Native | ❌ | ⚠️ Via proxy |
| **Wireless Debugging** | ⚠️ Network GDB | ✅ | ❌ Serial only | ✅ |
| **Single Connection** | ❌ | ❌ | N/A | ✅ |
| **Encryption** | ⚠️ Manual TLS | ⚠️ Manual TLS | ❌ | ✅ WSS built-in |
| **User Permissions** | ❌ | ❌ | ⚠️ Serial prompt | ❌ |
| **Protocol Multiplexing** | ❌ | ❌ | N/A | ✅ |
| **JTAG Required** | ✅ | ❌ | ❌ | ❌ |

---

## Client Compatibility Strategy

### For Browser-Based IDEs (ScriptO Studio)

**Native support - no proxy needed:**

```javascript
class ScriptODebugger {
    constructor(wsConnection) {
        this.ws = wsConnection; // Existing WebREPL connection
        this.seq = 1;
    }
    
    sendDAPRequest(command, args) {
        const msg = {
            seq: this.seq++,
            type: 'request',
            command: command,
            arguments: args
        };
        
        const json = JSON.stringify(msg);
        const dap_msg = `Content-Length: ${json.length}\r\n\r\n${json}`;
        
        // Send as TEXT frame (DAP)
        this.ws.send(dap_msg);
    }
    
    handleMessage(event) {
        if (typeof event.data === 'string') {
            // TEXT frame = DAP
            this.handleDAPMessage(event.data);
        } else {
            // BINARY frame = WebREPL CB
            this.handleWebREPLMessage(event.data);
        }
    }
}
```

**Result:** One WebSocket connection provides:
- REPL (terminal)
- File transfer
- Debugging

### For Desktop IDEs (VS Code, PyCharm)

**Simple TCP-to-WebSocket proxy:**

```python
#!/usr/bin/env python3
"""
DAP WebSocket Proxy
Bridges VS Code (TCP) to ESP32 (WebSocket)
"""
import asyncio
import websockets
import socket

async def tcp_to_ws_bridge(tcp_reader, tcp_writer, websocket):
    """Forward TCP data to WebSocket as TEXT frames"""
    try:
        while True:
            data = await tcp_reader.read(8192)
            if not data:
                break
            await websocket.send(data.decode('utf-8'))
    except Exception as e:
        print(f"TCP→WS error: {e}")

async def ws_to_tcp_bridge(websocket, tcp_writer):
    """Forward WebSocket TEXT frames to TCP"""
    try:
        async for message in websocket:
            if isinstance(message, str):  # Only forward TEXT frames
                tcp_writer.write(message.encode('utf-8'))
                await tcp_writer.drain()
    except Exception as e:
        print(f"WS→TCP error: {e}")

async def handle_client(tcp_reader, tcp_writer):
    """Handle incoming TCP connection from VS Code"""
    uri = "ws://192.168.4.1:8266/webrepl"
    async with websockets.connect(uri) as websocket:
        await asyncio.gather(
            tcp_to_ws_bridge(tcp_reader, tcp_writer, websocket),
            ws_to_tcp_bridge(websocket, tcp_writer)
        )

async def main():
    server = await asyncio.start_server(
        handle_client, '127.0.0.1', 5678
    )
    print("DAP WebSocket Proxy")
    print("Listening on: 127.0.0.1:5678")
    print("Connect VS Code to: localhost:5678")
    print("Forwarding to: ws://192.168.4.1:8266/webrepl")
    async with server:
        await server.serve_forever()

if __name__ == '__main__':
    asyncio.run(main())
```

**VS Code Configuration:**
```json
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
```

**User Experience:**
1. Run proxy once: `python dap-proxy.py`
2. Connect VS Code normally (localhost:5678)
3. Proxy handles WebSocket translation transparently

**Complexity:** ~50 lines of Python, no dependencies beyond `websockets`

---

## Implementation Benefits

### Single Connection Architecture

**Traditional multi-server approach:**
```
Client maintains:
- WebSocket to :8266 (WebREPL)
- TCP to :5678 (DAP debugger)

Problems:
❌ Connection state synchronization
❌ Two authentication flows
❌ Port conflicts possible
❌ Firewall rules more complex
❌ Browser can only use one
```

**Opcode multiplexing approach:**
```
Client maintains:
- One WebSocket to :8266

Advantages:
✅ Single connection state
✅ One authentication
✅ One port to configure
✅ Browser can use everything
✅ Simpler code
```

### Memory Efficiency

**ESP32 Memory Comparison:**

| Approach | HTTP Server | WS Server | TCP Server | Total |
|----------|-------------|-----------|------------|-------|
| Separate | ~20KB | ~8KB | ~8KB | **~36KB** |
| Multiplexed | ~20KB | ~8KB | - | **~28KB** |

**Savings:** ~8KB RAM (significant on embedded systems)

### Developer Experience

**For ScriptO Studio developers:**
```javascript
// One connection for everything
const device = new ESP32Device('ws://device/webrepl');

await device.connect();            // One connection
await device.authenticate();       // One auth

// Now use any feature:
device.repl.execute('print("hello")');  // BINARY frames
device.files.upload('/main.py', code); // BINARY frames  
device.debug.setBreakpoint('main.py', 10); // TEXT frames
```

**Result:** Cleaner API, simpler state management, better UX

### Security Benefits

**Single TLS termination:**
```
wss://device/webrepl (HTTPS + WSS)
  ↓
All traffic encrypted with one certificate
  ↓
DAP debugging + REPL + Files all secured
```

**vs. Multiple endpoints:**
```
wss://device/webrepl (HTTPS + WSS)
tcp://device:5678 (DAP - unencrypted?)
  ↓
Need to secure DAP separately
  ↓  
More attack surface
```

---

## Trade-offs and Considerations

### Advantages

1. **Browser-First**
   - Native support for browser IDEs
   - No plugins, no permissions, no backend
   - Future-proof as web development grows

2. **Architectural Elegance**
   - Single connection for all protocols
   - Clean opcode discrimination
   - Lower memory footprint
   - Simpler port management

3. **Security**
   - Built-in TLS via wss://
   - Single authentication point
   - Reduced attack surface

4. **Innovation**
   - Novel in MicroPython ecosystem
   - Positions project as forward-thinking
   - Demonstrates deep protocol understanding

### Disadvantages

1. **Desktop IDE Compatibility**
   - VS Code Desktop needs proxy
   - PyCharm/IntelliJ need proxy
   - Extra step for desktop users

2. **Non-Standard Transport**
   - Not in official DAP specification
   - May confuse developers familiar with standard DAP
   - Need clear documentation

3. **Limited Precedents**
   - Few examples to reference
   - Community unfamiliarity
   - Need to prove the concept

### Mitigations

**For desktop IDE users:**
- Provide well-tested proxy script
- Clear documentation with examples
- Consider VS Code extension in future

**For documentation:**
- Explain rationale clearly (this document!)
- Show comparison with alternatives
- Provide working examples

**For community adoption:**
- Open source everything
- Write blog posts / tutorials
- Present at conferences (PyCon, etc.)

---

## Future Directions

### Phase 1: Foundation (Current)
- ✅ Protocol specification (WebREPL CB RFC)
- ✅ WebSocket opcode multiplexing architecture
- ✅ Basic DAP message handling (initialize, breakpoints)
- ⏳ ScriptO Studio integration

### Phase 2: Full DAP Implementation
- ⏳ sys.settrace() integration for breakpoints
- ⏳ Stack frame inspection
- ⏳ Variable evaluation
- ⏳ Step in/out/over
- ⏳ Exception handling

### Phase 3: Desktop IDE Support
- ⏳ Polished proxy script
- ⏳ VS Code extension (optional)
- ⏳ PyCharm plugin (optional)

### Phase 4: Advanced Features
- ⏳ Conditional breakpoints
- ⏳ Watch expressions
- ⏳ Hot code reload
- ⏳ Performance profiling integration

### Phase 5: Community & Ecosystem
- ⏳ Publish papers/blog posts
- ⏳ Submit to MicroPython upstream (if appropriate)
- ⏳ Build community around approach
- ⏳ Influence DAP spec to recognize WebSocket (long-term)

---

## Conclusion

**WebSocket opcode multiplexing for DAP is the right choice** because:

1. **It's the only option for browser IDEs** - TCP is blocked by browser security
2. **It's more elegant** - One connection, lower memory, simpler architecture
3. **It's secure by default** - Built-in TLS via wss://
4. **It's future-facing** - Web-based development is growing
5. **Desktop users get acceptable UX** - Simple proxy bridges the gap

The fact that DAP-over-WebSocket is rare doesn't mean it's wrong - it means we're solving a problem (browser-based ESP32 debugging) that others haven't tackled this way yet.

**We're not fighting the browser security model - we're embracing it.**

---

## References

- **DAP Specification:** https://microsoft.github.io/debug-adapter-protocol/
- **WebSocket RFC 6455:** https://www.rfc-editor.org/rfc/rfc6455
- **WebREPL CB RFC:** `webrepl_cb_rfc.md` (this repository)
- **VS Code Integration Guide:** `docs/VSCODE_INTEGRATION.md` (this repository)
- **ESP-IDF Debug Adapter:** https://github.com/espressif/vscode-esp-idf-extension/tree/master/esp_debug_adapter
- **mp_debugpy:** https://github.com/Josverl/mp_debugpy
- **Browser Security Model:** https://developer.mozilla.org/en-US/docs/Web/Security
- **WebSocket API:** https://developer.mozilla.org/en-US/docs/Web/API/WebSocket

---

**Author Contact:**  
Jonathan Peace  
Email: jep@alphabetiq.com
GitHub: [@sjetpax](https://github.com/jetpax)
