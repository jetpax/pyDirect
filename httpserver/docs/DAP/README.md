# WebREPL Documentation

This directory contains detailed documentation for the WebREPL protocols and debugging features.

## Quick Links

### Protocol Specifications
- [../webrepl_rfc.txt](../webrepl_rfc.txt) - Legacy WebREPL protocol (WA/WB)
- [../webrepl_cb_rfc.md](../webrepl_cb_rfc.md) - WebREPL Channelized Binary (WBP) protocol
- [../dap_websocket_advocacy.md](../dap_websocket_advocacy.md) - DAP over WebSocket rationale

### Implementation Guides
- [VSCODE_INTEGRATION.md](VSCODE_INTEGRATION.md) - VS Code setup (Desktop and vscode.dev)
- [ARCHITECTURE_COMPARISON.md](ARCHITECTURE_COMPARISON.md) - ESP-IDF adapter vs WebDAP
- [../IMPLEMENTATION_GUIDE.md](../IMPLEMENTATION_GUIDE.md) - Technical implementation details

### Tools
- [../tools/dap-proxy.py](../tools/dap-proxy.py) - TCP-to-WebSocket proxy for VS Code Desktop
- [../tools/README.md](../tools/README.md) - Tool documentation

### Examples
- [../examples/browser_debug_example.html](../examples/browser_debug_example.html) - Browser demo

---

## Document Summaries

### Protocol Specifications

#### Legacy WebREPL (webrepl_rfc.txt)
The original WebREPL protocol with:
- Password authentication
- Text frames for REPL
- Binary frames for file transfer (WA/WB magic bytes)
- Fixed 82-byte headers

**Status:** Informational (describes existing protocol)

#### WebREPL CB (webrepl_cb_rfc.md)
Modern channelized protocol with:
- CBOR encoding
- 255 independent channels
- TFTP-based file transfers
- Binary bytecode support
- Event system

**Status:** Draft specification (v1.0)

#### DAP WebSocket (dap_websocket_advocacy.md)
Debug Adapter Protocol over WebSocket with:
- TEXT frames for DAP (opcode 0x01)
- BINARY frames for WebREPL CB (opcode 0x02)
- Single connection multiplexing
- Browser-native debugging

**Status:** Advocacy document + specification

---

### Implementation Guides

#### VS Code Integration (VSCODE_INTEGRATION.md)
Complete guide for using WebDAP with VS Code:

**Covers:**
- VS Code Desktop setup (via proxy)
- vscode.dev setup (native WebSocket)
- Launch vs Attach modes
- Extension development
- Configuration examples

**Target Audience:** VS Code users, extension developers

#### Architecture Comparison (ARCHITECTURE_COMPARISON.md)
Deep comparison of debugging approaches:

**Compares:**
- ESP-IDF debug adapter (C/C++ via JTAG/GDB)
- MicroPython WebDAP (Python via WebSocket)
- Transport layers (TCP vs WebSocket)
- Use cases and trade-offs

**Target Audience:** Developers choosing debug strategy

#### Implementation Guide (IMPLEMENTATION_GUIDE.md)
Technical architecture and implementation:

**Covers:**
- Opcode multiplexing mechanism
- Server-side C implementation
- Client-side JavaScript/Python usage
- Testing procedures
- Troubleshooting

**Target Audience:** Implementers, contributors

---

## Use Case Guide

### "I want to debug Python scripts on ESP32 from my browser"

**Solution:** Use WebDAP (DAP over WebSocket)

1. Read: [dap_websocket_advocacy.md](../dap_websocket_advocacy.md)
2. Implement: [IMPLEMENTATION_GUIDE.md](../IMPLEMENTATION_GUIDE.md)
3. Test: [../examples/browser_debug_example.html](../examples/browser_debug_example.html)

**Why:** Browsers can't access TCP, WebSocket is the only option.

### "I want to debug Python scripts from VS Code Desktop"

**Solution:** Use WebDAP with proxy

1. Setup: [VSCODE_INTEGRATION.md](VSCODE_INTEGRATION.md) → "VS Code Desktop" section
2. Run: `python tools/dap-proxy.py --device 192.168.4.1:8266`
3. Configure: `.vscode/launch.json` → Connect to localhost:5678

**Why:** VS Code expects TCP, proxy bridges to WebSocket.

### "I want to debug the MicroPython interpreter itself (C code)"

**Solution:** Use ESP-IDF debug adapter (not WebDAP)

1. Read: [ARCHITECTURE_COMPARISON.md](ARCHITECTURE_COMPARISON.md) → "ESP-IDF Adapter" section
2. Install: [vscode-esp-idf-extension](https://github.com/espressif/vscode-esp-idf-extension)
3. Setup: JTAG hardware, OpenOCD

**Why:** WebDAP debugs Python, ESP-IDF debugs C at hardware level.

### "I want both Python and C debugging"

**Solution:** Use both! They work at different layers.

1. Read: [ARCHITECTURE_COMPARISON.md](ARCHITECTURE_COMPARISON.md) → "Can Both Coexist?"
2. Setup: Both debug configurations in VS Code
3. Switch: Choose config based on what you're debugging

**Why:** Python bugs need Python debugger, C bugs need C debugger.

### "I'm building a web-based MicroPython IDE"

**Solution:** Implement WebDAP client

1. Study: [IMPLEMENTATION_GUIDE.md](../IMPLEMENTATION_GUIDE.md) → "Client-Side Usage"
2. Example: [../examples/browser_debug_example.html](../examples/browser_debug_example.html)
3. Reference: [DAP Specification](https://microsoft.github.io/debug-adapter-protocol/)

**Why:** Native WebSocket support, protocol multiplexing, single connection.

### "I want to file REPL and files over one connection"

**Solution:** Use WebREPL Binary Protocol

1. Spec: [../webrepl_cb_rfc.md](../webrepl_cb_rfc.md)
2. Channels: 0=Events, 1=REPL, 2=M2M, 23=Files
3. Encoding: CBOR for compact binary messages

**Why:** Channelized protocol, efficient, extensible.

---

## Document Roadmap

### Current Documents (Complete)
- ✅ Legacy WebREPL RFC (informational)
- ✅ WebREPL CB RFC (draft v1.0)
- ✅ DAP WebSocket Advocacy
- ✅ VS Code Integration Guide
- ✅ Architecture Comparison
- ✅ Implementation Guide
- ✅ Proxy tool + examples

### Future Documents (Planned)
- ⏳ WebREPL CB Implementation Tutorial
- ⏳ sys.settrace() Integration Guide
- ⏳ VS Code Extension Tutorial
- ⏳ Performance Tuning Guide
- ⏳ Security Best Practices
- ⏳ Protocol Testing Guide

---

## Contributing

If you're improving these docs:

1. **Accuracy** - Test all examples before documenting
2. **Clarity** - Write for diverse audiences (beginners to experts)
3. **Completeness** - Include edge cases and troubleshooting
4. **Cross-links** - Reference related docs liberally
5. **Examples** - Show, don't just tell

---

## Document Status Legend

| Symbol | Meaning |
|--------|---------|
| ✅ | Complete and tested |
| 🚧 | In progress |
| ⏳ | Planned |
| 📋 | Draft |
| ℹ️ | Informational only |

---

## Questions?

- **Protocol Questions:** Read [../webrepl_cb_rfc.md](../webrepl_cb_rfc.md)
- **VS Code Setup:** Read [VSCODE_INTEGRATION.md](VSCODE_INTEGRATION.md)
- **Architecture Questions:** Read [ARCHITECTURE_COMPARISON.md](ARCHITECTURE_COMPARISON.md)
- **Other Questions:** Contact jep@alphabetiq.com

---

**Last Updated:** December 2025  
**Project:** [ScriptO Studio](https://github.com/scripto-studio)
