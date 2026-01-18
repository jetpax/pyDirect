# Architecture Comparison: ESP-IDF vs MicroPython DAP

This document compares the architecture of the ESP-IDF debug adapter with our MicroPython WebDAP implementation.

## ESP-IDF Debug Adapter (esp_debug_adapter)

**Purpose:** Debug native C/C++ ESP32 applications

```
┌───────────────────────────────────────────────────────────────┐
│  VS Code Desktop (Python Extension)                           │
│  ↓ User sets breakpoints in C files                           │
│  ↓ Presses F5 to start debugging                              │
└───────────────────────────────────────────────────────────────┘
                        ↓
                  [TCP: localhost:43474]
                  (DAP Protocol)
                        ↓
┌───────────────────────────────────────────────────────────────┐
│  debug_adapter_main.py                                        │
│  ↓ Translates DAP to GDB/MI commands                          │
│  ↓ Python DAP implementation                                  │
└───────────────────────────────────────────────────────────────┘
                        ↓
                  [TCP: localhost:3333]
                  (GDB/MI Protocol)
                        ↓
┌───────────────────────────────────────────────────────────────┐
│  OpenOCD                                                      │
│  ↓ Translates GDB commands to JTAG signals                    │
│  ↓ Hardware debug adapter                                     │
└───────────────────────────────────────────────────────────────┘
                        ↓
                [JTAG Hardware Interface]
                  (USB-to-JTAG adapter)
                        ↓
┌───────────────────────────────────────────────────────────────┐
│  ESP32 Device                                                 │
│  ↓ Running compiled C/C++ binary                              │
│  ↓ Hardware breakpoints via debug registers                   │
└───────────────────────────────────────────────────────────────┘

Debug Level: Hardware (CPU registers, assembly, memory)
Transport: TCP (local only, requires separate processes)
Target: Native compiled code
Browser Support: ❌ (TCP not accessible from browser)
```

### ESP-IDF Adapter Characteristics

**Strengths:**
- ✅ Hardware-level debugging (registers, assembly, memory dumps)
- ✅ Production-ready (mature, well-tested)
- ✅ Standard DAP-over-TCP (works with any DAP client)
- ✅ No special client modifications needed

**Limitations:**
- ❌ Requires JTAG hardware (USB cable + adapter)
- ❌ Three separate processes (VS Code ↔ Python ↔ OpenOCD ↔ ESP32)
- ❌ Cannot work in browser (TCP blocked by security)
- ❌ Only debugs native code (not Python scripts)

**Use Cases:**
- ESP-IDF framework development
- MicroPython interpreter development (debug C code)
- Performance-critical firmware debugging
- Production firmware debugging

---

## MicroPython WebDAP (modwebDAP.c)

**Purpose:** Debug Python scripts running on MicroPython

```
┌───────────────────────────────────────────────────────────────┐
│  vscode.dev (Browser) or VS Code Desktop                     │
│  ↓ User sets breakpoints in .py files                         │
│  ↓ Presses F5 to start debugging                              │
└───────────────────────────────────────────────────────────────┘
                        ↓
    ┌───────────────────────────────────────────────┐
    │  Desktop: TCP :5678 (via dap-proxy.py)        │
    │  Browser: WebSocket TEXT frames directly      │
    └───────────────────────────────────────────────┘
                        ↓
                [WebSocket: ws://device:8266/webrepl]
                (DAP Protocol on TEXT frames)
                        ↓
┌───────────────────────────────────────────────────────────────┐
│  ESP32 MicroPython (modwebDAP.c)                              │
│  ↓ Parses DAP messages (Content-Length + JSON)                │
│  ↓ Installs sys.settrace() callback                           │
│  ↓ Python-level debugging                                     │
└───────────────────────────────────────────────────────────────┘
                        ↓
                [Same WebSocket Connection]
                (BINARY frames for WebREPL CB)
                        ↓
┌───────────────────────────────────────────────────────────────┐
│  ESP32 MicroPython (modwebrepl.c)                             │
│  ↓ Handles REPL, file operations                              │
│  ↓ CBOR-encoded messages on Channel 1, 2, 23                  │
└───────────────────────────────────────────────────────────────┘

Debug Level: Python script level (via sys.settrace)
Transport: WebSocket (wireless, browser-compatible)
Target: MicroPython scripts (.py files)
Browser Support: ✅ (Native WebSocket API)
```

### WebDAP Characteristics

**Strengths:**
- ✅ Browser-native (works in vscode.dev, ScriptO Studio)
- ✅ Single connection (DEBUG + REPL + FILES on one WebSocket)
- ✅ Wireless debugging (WiFi, no cables needed)
- ✅ Protocol multiplexing (opcode discrimination)
- ✅ Built-in TLS (wss:// for security)

**Limitations:**
- ⚠️ Desktop IDEs need proxy (simple TCP-to-WebSocket bridge)
- ⚠️ Python-level only (cannot debug C interpreter itself)
- ⚠️ Novel approach (not widely adopted yet)

**Use Cases:**
- MicroPython application development
- Browser-based IDEs (vscode.dev, ScriptO Studio)
- Wireless development workflows
- IoT device debugging (no physical access)

---

## Side-by-Side Comparison

### Architecture Layers

```
┌─────────────────────────────────────────────────────────────────────┐
│                         ESP-IDF Adapter                             │
├─────────────────────────────────────────────────────────────────────┤
│  VS Code Desktop                                                    │
│    ↓ [TCP Socket]                                                   │
│  debug_adapter_main.py (Python)                                     │
│    ↓ [TCP Socket, GDB/MI]                                           │
│  OpenOCD                                                            │
│    ↓ [JTAG Hardware]                                                │
│  ESP32 CPU Debug Registers                                          │
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│                         MicroPython WebDAP                          │
├─────────────────────────────────────────────────────────────────────┤
│  vscode.dev or VS Code Desktop                                      │
│    ↓ [WebSocket TEXT frames or TCP via proxy]                      │
│  modwebDAP.c (C module in ESP32 firmware)                           │
│    ↓ [sys.settrace() Python API]                                    │
│  MicroPython Interpreter                                            │
└─────────────────────────────────────────────────────────────────────┘
```

### Transport Comparison

| Aspect | ESP-IDF Adapter | MicroPython WebDAP |
|--------|-----------------|-------------------|
| **Primary Transport** | TCP sockets | WebSocket |
| **Secondary Transport** | GDB/MI to OpenOCD | None (direct) |
| **Browser Compatible** | ❌ No | ✅ Yes |
| **Requires Hardware** | ✅ JTAG adapter | ❌ WiFi only |
| **Encryption** | Manual setup | WSS built-in |
| **Connection Count** | 2+ (DAP + GDB + Serial) | 1 (multiplexed) |
| **Proxy Needed** | ❌ No (desktop) | ⚠️ Yes (desktop) |

### Debugging Capabilities

| Feature | ESP-IDF Adapter | MicroPython WebDAP |
|---------|-----------------|-------------------|
| **Breakpoints** | Hardware (CPU) | Software (Python) |
| **Step In/Out/Over** | Assembly level | Python line level |
| **Variable Inspection** | Memory addresses | Python objects |
| **Stack Traces** | C call stack | Python call stack |
| **Watch Expressions** | C expressions | Python expressions |
| **Disassembly View** | ✅ Native | ❌ N/A |
| **Memory View** | ✅ Full access | ❌ Limited |
| **Register View** | ✅ All CPU regs | ❌ N/A |
| **Hot Code Reload** | ❌ No | ✅ Possible |

### Use Case Decision Tree

```
┌─────────────────────────────────────────────┐
│  What are you debugging?                    │
└─────────────────────────────────────────────┘
            ↓
    ┌───────┴───────┐
    │               │
    ↓               ↓
┌─────────┐   ┌──────────────┐
│ C/C++   │   │ Python (.py) │
│ Native  │   │ Scripts      │
└─────────┘   └──────────────┘
    ↓               ↓
    │               │
    ↓               ↓
ESP-IDF         MicroPython
Adapter         WebDAP
    ↓               ↓
    │               │
    ↓               ↓
Requires:       Requires:
- JTAG HW       - WiFi only
- OpenOCD       - modwebDAP.c
- Desktop IDE   - Any IDE
```

### Client Compatibility

```
┌──────────────────────────────────────────────────────────────┐
│  Client Application                                          │
├──────────────────────────────────────────────────────────────┤
│  VS Code Desktop                                             │
│    ├─ ESP-IDF Adapter:    ✅ Native (TCP)                    │
│    └─ MicroPython WebDAP: ⚠️ Via proxy (TCP→WebSocket)       │
├──────────────────────────────────────────────────────────────┤
│  vscode.dev (Browser)                                        │
│    ├─ ESP-IDF Adapter:    ❌ Cannot use (TCP blocked)        │
│    └─ MicroPython WebDAP: ✅ Native (WebSocket)              │
├──────────────────────────────────────────────────────────────┤
│  PyCharm / IntelliJ IDEA                                     │
│    ├─ ESP-IDF Adapter:    ✅ Native (TCP)                    │
│    └─ MicroPython WebDAP: ⚠️ Via proxy (TCP→WebSocket)       │
├──────────────────────────────────────────────────────────────┤
│  ScriptO Studio (Browser)                                    │
│    ├─ ESP-IDF Adapter:    ❌ Cannot use (TCP blocked)        │
│    └─ MicroPython WebDAP: ✅ Native (WebSocket)              │
└──────────────────────────────────────────────────────────────┘
```

---

## Can Both Coexist?

**Yes!** They operate at different layers:

```
┌─────────────────────────────────────────────────────────────┐
│  Development Workflow Example                               │
├─────────────────────────────────────────────────────────────┤
│  Scenario 1: Python Script Bug                              │
│  ↓ Use MicroPython WebDAP                                   │
│  ↓ Set breakpoints in .py files                             │
│  ↓ Inspect Python variables, step through lines             │
│  ✅ Fix found in Python logic                               │
├─────────────────────────────────────────────────────────────┤
│  Scenario 2: MicroPython Interpreter Crash                  │
│  ↓ Use ESP-IDF Adapter                                      │
│  ↓ Set breakpoints in mp_*.c files                          │
│  ↓ Inspect CPU registers, stack memory                      │
│  ✅ Fix found in C code (interpreter bug)                   │
├─────────────────────────────────────────────────────────────┤
│  Scenario 3: Performance Issue                              │
│  ↓ Use BOTH simultaneously                                  │
│  ↓ Python profiler shows slow function                      │
│  ↓ Switch to ESP-IDF adapter                                │
│  ↓ Profile C implementation of that function                │
│  ✅ Fix found in C optimization opportunity                 │
└─────────────────────────────────────────────────────────────┘
```

### VS Code Configuration for Both

```json
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "Python Script Debugging",
            "type": "micropython-esp32",
            "request": "launch",
            "program": "/main.py",
            "wsUrl": "ws://192.168.4.1:8266/webrepl"
        },
        {
            "name": "MicroPython Interpreter (C) Debugging",
            "type": "espidf",
            "request": "attach",
            "tcpPort": 43474,
            "program": "${workspaceFolder}/build/micropython.elf"
        }
    ]
}
```

---

## WebSocket as DAP Transport: Why Novel?

### Standard DAP Implementations

Most DAP debug adapters follow this pattern (like ESP-IDF adapter):

```
IDE ←─[TCP]─→ Debug Adapter ←─[Protocol X]─→ Target
```

Where Protocol X might be:
- GDB/MI (for native debuggers)
- Chrome DevTools Protocol (for JavaScript)
- Debug API (for Java/JVM)

### Our WebSocket Pattern

```
Browser IDE ←─[WebSocket]─→ Target
     (DAP directly)
```

**Why this is rare:**

1. **Most debuggers target desktop IDEs** - TCP works fine
2. **Most don't need protocol multiplexing** - Separate connections OK
3. **Most don't prioritize wireless** - USB/JTAG is standard
4. **Most don't target browsers** - Web debugging is different domain

### Examples of WebSocket DAP

**To our knowledge, there are VERY FEW implementations:**

| Project | Status | Notes |
|---------|--------|-------|
| **Our WebDAP** | ✅ Active | MicroPython on ESP32 |
| Chrome DevTools | ❌ Not DAP | Uses CDP protocol instead |
| VS Code Remote | ⚠️ Internal | Proprietary, not documented |
| Language Servers | ❌ Not DAP | LSP is different protocol |

**Why so rare?**

- Desktop debuggers don't need it (TCP works)
- Browser debugging uses Chrome DevTools Protocol (not DAP)
- VS Code's WebSocket usage is internal/proprietary
- DAP spec doesn't mention WebSocket as transport

**Our contribution:**

We're likely the **first open-source implementation** of:
- DAP over WebSocket TEXT frames
- Protocol multiplexing via WebSocket opcodes
- Browser-native debugging for embedded Python

---

## Inspiration from ESP-IDF Adapter

The ESP-IDF adapter taught us:

1. **Full DAP lifecycle is important**
   - Not just `attach`, but also `launch`
   - Proper initialization sequence
   - Event handling (stopped, continued, etc.)

2. **Standard patterns work**
   - Follow DAP spec closely
   - Standard capabilities negotiation
   - Familiar VS Code experience

3. **Transport is flexible**
   - ESP-IDF: DAP→TCP, then GDB/MI→TCP, then JTAG
   - Us: DAP→WebSocket, then direct Python callbacks

4. **Documentation matters**
   - Clear README
   - Usage examples
   - Test infrastructure

**What we do differently:**

- Use WebSocket instead of TCP (browser compatibility)
- Debug Python not C (different level)
- Multiplex protocols on one connection (efficiency)
- Target browser IDEs first (modern workflow)

---

## Future: Bridging the Gap

### Idea: Universal Debug Adapter Registry

```
VS Code Marketplace:
- MicroPython ESP32 Debug Extension
  - Supports both TCP (via proxy) and WebSocket (native)
  - Works in VS Code Desktop and vscode.dev
  - Automatic transport detection
```

### Idea: DAP Specification Enhancement

Propose WebSocket as official DAP transport:

```typescript
// Proposed DAP spec addition
interface DebugAdapterServerDescriptor {
    type: 'tcp' | 'websocket' | 'websocket-secure';
    host?: string;
    port?: number;
    url?: string;  // For WebSocket: ws://host:port/path
}
```

---

## Conclusion

**ESP-IDF Adapter** and **MicroPython WebDAP** are complementary:

| Aspect | ESP-IDF | WebDAP |
|--------|---------|--------|
| **Layer** | Hardware/C | Python |
| **Transport** | TCP | WebSocket |
| **Client** | Desktop | Browser + Desktop |
| **Purpose** | Native debugging | Script debugging |
| **Maturity** | Production | Experimental |

Both are valuable. Both follow DAP. Both enable debugging ESP32.

**The future:** Browser-based development with native debugging support, made possible by WebSocket transport.

---

## See Also

- [VS Code Integration Guide](VSCODE_INTEGRATION.md)
- [DAP WebSocket Advocacy](../dap_websocket_advocacy.md)
- [Implementation Guide](../IMPLEMENTATION_GUIDE.md)
- [ESP-IDF Debug Adapter Source](https://github.com/espressif/vscode-esp-idf-extension/tree/master/esp_debug_adapter)

