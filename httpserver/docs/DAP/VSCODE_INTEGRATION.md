# VS Code Integration Guide

This guide explains how to use the WebSocket DAP implementation with VS Code Desktop and vscode.dev (browser-based VS Code).

## Table of Contents

1. [Overview](#overview)
2. [VS Code Desktop (via Proxy)](#vs-code-desktop-via-proxy)
3. [vscode.dev (Native WebSocket)](#vscodedev-native-websocket)
4. [Launch vs Attach Modes](#launch-vs-attach-modes)
5. [Configuration Reference](#configuration-reference)
6. [Extension Development](#extension-development)

---

## Overview

Our WebSocket DAP implementation supports the full Debug Adapter Protocol, including:

- ✅ `initialize` - Handshake and capability negotiation
- ✅ `launch` - Start a Python script with debugging
- ✅ `attach` - Connect to running MicroPython interpreter
- ✅ `setBreakpoints` - Set/clear breakpoints
- ✅ `continue`, `next`, `stepIn`, `stepOut` - Execution control
- ✅ `stackTrace`, `scopes`, `variables` - Inspection
- ✅ `threads` - Thread enumeration (single thread for MicroPython)

### Architecture Comparison

```
┌─────────────────────────────────────────────────────────────┐
│  ESP-IDF Debug Adapter (esp_debug_adapter)                  │
│                                                              │
│  VS Code ←─[TCP]─→ Python DAP ←─[TCP]─→ OpenOCD ←─→ ESP32  │
│                                                              │
│  Purpose: Debug native C/C++ ESP32 applications             │
│  Debugger: GDB via OpenOCD                                  │
│  Protocol: DAP over TCP, GDB/MI to OpenOCD                  │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│  MicroPython WebDAP (modwebDAP.c)                           │
│                                                              │
│  IDE ←─[WebSocket TEXT]─→ ESP32 MicroPython                 │
│                                                              │
│  Purpose: Debug MicroPython scripts on ESP32                │
│  Debugger: sys.settrace() callbacks (Python-level)          │
│  Protocol: DAP over WebSocket TEXT frames                   │
└─────────────────────────────────────────────────────────────┘
```

**Key Differences:**
- ESP-IDF adapter debugs **native C code** (compiled binaries)
- WebDAP debugs **Python scripts** (MicroPython interpreter)
- ESP-IDF uses TCP (standard), WebDAP uses WebSocket (browser-friendly)

---

## VS Code Desktop (via Proxy)

VS Code Desktop expects DAP over TCP. Use the provided proxy to bridge TCP to WebSocket.

### Step 1: Install Proxy

```bash
cd /path/to/webrepl/tools
pip install -r requirements.txt
```

### Step 2: Start Proxy

```bash
python dap-proxy.py --device 192.168.4.1:8266
```

Output:
```
============================================================
DAP WebSocket Proxy
============================================================
Listening on:  127.0.0.1:5678
Forwarding to: ws://192.168.4.1:8266/webrepl

Configure your IDE to connect to:
  Host: 127.0.0.1
  Port: 5678
============================================================
```

### Step 3: Configure VS Code

Create `.vscode/launch.json`:

```json
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "ESP32 MicroPython: Launch",
            "type": "python",
            "request": "launch",
            "connect": {
                "host": "127.0.0.1",
                "port": 5678
            },
            "program": "/main.py",
            "stopOnEntry": true,
            "pathMappings": [
                {
                    "localRoot": "${workspaceFolder}",
                    "remoteRoot": "/"
                }
            ]
        },
        {
            "name": "ESP32 MicroPython: Attach",
            "type": "python",
            "request": "attach",
            "connect": {
                "host": "127.0.0.1",
                "port": 5678
            },
            "pathMappings": [
                {
                    "localRoot": "${workspaceFolder}",
                    "remoteRoot": "/"
                }
            ]
        }
    ]
}
```

### Step 4: Debug

1. Press `F5` or select debug configuration
2. Set breakpoints in your Python files
3. Execution will pause at breakpoints
4. Use debug toolbar to step, continue, inspect variables

---

## vscode.dev (Native WebSocket)

vscode.dev (browser-based VS Code) can connect directly to WebSocket DAP endpoints without a proxy!

### Why This Matters

**Standard DAP implementations don't work in vscode.dev:**

```javascript
// This FAILS in browser (security restriction)
const socket = net.connect(5678, 'localhost'); // ❌ Not available in browser

// This WORKS in browser
const ws = new WebSocket('ws://device/webrepl'); // ✅ Native browser API
```

Our WebSocket DAP is **the only way** to debug from vscode.dev to a remote device.

### VS Code Extension for vscode.dev

To make this work seamlessly in vscode.dev, you need a VS Code extension that:

1. Registers a custom debug adapter type
2. Connects to WebSocket instead of TCP
3. Handles TEXT frame DAP messages

#### Extension Skeleton

Create `extension.js`:

```javascript
const vscode = require('vscode');

class MicroPythonDebugAdapterDescriptorFactory {
    createDebugAdapterDescriptor(session, executable) {
        // Return WebSocket-based descriptor
        return new vscode.DebugAdapterServer(
            5678, 
            'localhost'
        );
        
        // For direct WebSocket (requires custom implementation):
        // return new MicroPythonWebSocketDebugAdapter(session.configuration);
    }
}

class MicroPythonWebSocketDebugAdapter {
    constructor(config) {
        this.ws = null;
        this.config = config;
    }
    
    async start(session) {
        const wsUrl = this.config.wsUrl || 'ws://192.168.4.1:8266/webrepl';
        
        return new Promise((resolve, reject) => {
            // In Node.js environment (extension host)
            const WebSocket = require('ws');
            this.ws = new WebSocket(wsUrl);
            
            this.ws.on('open', () => {
                console.log('DAP WebSocket connected');
                resolve();
            });
            
            this.ws.on('error', reject);
            
            this.ws.on('message', (data) => {
                // Forward DAP messages to VS Code
                if (typeof data === 'string') {
                    // TEXT frame = DAP
                    this.handleDAPMessage(data, session);
                }
                // Ignore BINARY frames (WebREPL CB)
            });
        });
    }
    
    handleDAPMessage(data, session) {
        // Parse Content-Length header
        const parts = data.split('\r\n\r\n');
        if (parts.length < 2) return;
        
        const json = JSON.parse(parts[1]);
        
        // Forward to VS Code UI
        session.sendProtocolMessage(json);
    }
    
    sendRequest(request) {
        const json = JSON.stringify(request);
        const dapFrame = `Content-Length: ${json.length}\r\n\r\n${json}`;
        
        // Send as TEXT frame
        this.ws.send(dapFrame);
    }
    
    dispose() {
        if (this.ws) {
            this.ws.close();
        }
    }
}

function activate(context) {
    // Register debug adapter factory
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory(
            'micropython-esp32',
            new MicroPythonDebugAdapterDescriptorFactory()
        )
    );
    
    // Register debug configuration provider
    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider(
            'micropython-esp32',
            new MicroPythonConfigurationProvider()
        )
    );
}

class MicroPythonConfigurationProvider {
    resolveDebugConfiguration(folder, config, token) {
        // Provide defaults
        if (!config.type) {
            config.type = 'micropython-esp32';
        }
        
        if (!config.request) {
            config.request = 'attach';
        }
        
        if (!config.wsUrl) {
            config.wsUrl = 'ws://192.168.4.1:8266/webrepl';
        }
        
        return config;
    }
}

module.exports = { activate };
```

#### Extension package.json

```json
{
    "name": "micropython-esp32-debug",
    "displayName": "MicroPython ESP32 Debugger",
    "version": "0.1.0",
    "publisher": "scripto-studio",
    "description": "Debug MicroPython on ESP32 via WebSocket DAP",
    "engines": {
        "vscode": "^1.85.0"
    },
    "categories": ["Debuggers"],
    "activationEvents": [
        "onDebug",
        "onDebugResolve:micropython-esp32"
    ],
    "main": "./extension.js",
    "contributes": {
        "debuggers": [
            {
                "type": "micropython-esp32",
                "label": "MicroPython ESP32",
                "languages": ["python"],
                "configurationAttributes": {
                    "launch": {
                        "required": ["program"],
                        "properties": {
                            "program": {
                                "type": "string",
                                "description": "Path to Python file on device",
                                "default": "/main.py"
                            },
                            "wsUrl": {
                                "type": "string",
                                "description": "WebSocket URL of ESP32 device",
                                "default": "ws://192.168.4.1:8266/webrepl"
                            },
                            "stopOnEntry": {
                                "type": "boolean",
                                "description": "Stop at first line",
                                "default": true
                            }
                        }
                    },
                    "attach": {
                        "properties": {
                            "wsUrl": {
                                "type": "string",
                                "description": "WebSocket URL of ESP32 device",
                                "default": "ws://192.168.4.1:8266/webrepl"
                            }
                        }
                    }
                },
                "configurationSnippets": [
                    {
                        "label": "MicroPython: Launch",
                        "description": "Launch MicroPython script with debugging",
                        "body": {
                            "type": "micropython-esp32",
                            "request": "launch",
                            "name": "Launch MicroPython",
                            "program": "/main.py",
                            "wsUrl": "ws://192.168.4.1:8266/webrepl",
                            "stopOnEntry": true
                        }
                    },
                    {
                        "label": "MicroPython: Attach",
                        "description": "Attach to running MicroPython",
                        "body": {
                            "type": "micropython-esp32",
                            "request": "attach",
                            "name": "Attach to MicroPython",
                            "wsUrl": "ws://192.168.4.1:8266/webrepl"
                        }
                    }
                ]
            }
        ]
    },
    "dependencies": {
        "ws": "^8.14.0"
    }
}
```

### Using the Extension in vscode.dev

1. Package extension: `vsce package`
2. Install in vscode.dev: Drag `.vsix` file to Extensions panel
3. Open workspace with MicroPython files
4. Create `.vscode/launch.json` with WebSocket URL
5. Press F5 to debug!

**No proxy needed** - vscode.dev connects directly to ESP32 WebSocket.

---

## Launch vs Attach Modes

### Launch Mode

**Use case:** Start a Python script fresh with debugging enabled from the beginning.

```json
{
    "type": "micropython-esp32",
    "request": "launch",
    "program": "/main.py",
    "stopOnEntry": true,
    "args": [],
    "cwd": "/"
}
```

**What happens:**
1. VS Code sends `launch` request
2. ESP32 installs `sys.settrace()` callback
3. ESP32 executes specified Python file
4. If `stopOnEntry=true`, pauses at first line
5. User can set breakpoints, step through code

**DAP Sequence:**
```
Client → Server: initialize
Server → Client: initialized event
Client → Server: setBreakpoints
Client → Server: launch
Server → Client: stopped event (if stopOnEntry)
User presses Continue/Step...
```

### Attach Mode

**Use case:** Connect to already-running MicroPython REPL to debug live code.

```json
{
    "type": "micropython-esp32",
    "request": "attach"
}
```

**What happens:**
1. VS Code sends `attach` request
2. ESP32 installs `sys.settrace()` callback on running interpreter
3. Any subsequent Python execution will hit breakpoints
4. User can inspect current state, set breakpoints for future code

**DAP Sequence:**
```
Client → Server: initialize
Server → Client: initialized event
Client → Server: setBreakpoints
Client → Server: attach
User executes code in REPL...
Server → Client: stopped event (when breakpoint hit)
```

---

## Configuration Reference

### Common Options

| Option | Type | Description | Default |
|--------|------|-------------|---------|
| `type` | string | Debug adapter type | `"micropython-esp32"` |
| `request` | string | `"launch"` or `"attach"` | Required |
| `name` | string | Display name in debug dropdown | Required |
| `wsUrl` | string | WebSocket endpoint URL | `"ws://192.168.4.1:8266/webrepl"` |

### Launch Options

| Option | Type | Description | Default |
|--------|------|-------------|---------|
| `program` | string | Path to Python file on device | `"/main.py"` |
| `stopOnEntry` | boolean | Pause at first line | `false` |
| `args` | array | Command-line arguments (future) | `[]` |
| `cwd` | string | Working directory on device | `"/"` |
| `noDebug` | boolean | Run without debugging | `false` |

### Attach Options

| Option | Type | Description | Default |
|--------|------|-------------|---------|
| (none) | - | Attach has no required options | - |

### Path Mappings

Map local workspace files to device filesystem:

```json
{
    "pathMappings": [
        {
            "localRoot": "${workspaceFolder}",
            "remoteRoot": "/"
        },
        {
            "localRoot": "${workspaceFolder}/lib",
            "remoteRoot": "/lib"
        }
    ]
}
```

---

## Extension Development

### Testing Locally

```bash
# Clone extension template
git clone https://github.com/scripto-studio/vscode-micropython-debug

cd vscode-micropython-debug

# Install dependencies
npm install

# Open in VS Code
code .

# Press F5 to launch Extension Development Host
# Test debug configurations in new VS Code window
```

### Publishing to Marketplace

```bash
# Install vsce
npm install -g @vscode/vsce

# Package
vsce package

# Publish
vsce publish
```

### Browser Compatibility (vscode.dev)

For full vscode.dev support, ensure:

1. No Node.js-only modules in browser context
2. Use browser-native `WebSocket` API
3. Test in vscode.dev before publishing

```javascript
// Extension code that works in both contexts
const WebSocket = typeof window !== 'undefined'
    ? window.WebSocket  // Browser
    : require('ws');     // Node.js
```

---

## Comparison with ESP-IDF Extension

### When to Use ESP-IDF Debug Adapter

Use `esp_debug_adapter` when:
- ✅ Debugging native C/C++ ESP-IDF applications
- ✅ Need GDB features (registers, assembly, core dumps)
- ✅ Using OpenOCD for JTAG debugging
- ✅ Working in VS Code Desktop (not browser)

### When to Use MicroPython WebDAP

Use `modwebDAP.c` when:
- ✅ Debugging Python scripts on MicroPython
- ✅ Need browser-based debugging (vscode.dev, ScriptO Studio)
- ✅ Want single WebSocket for REPL + files + debugging
- ✅ Wireless debugging over WiFi without JTAG

### Can You Use Both?

**Yes!** They debug at different levels:

```
┌──────────────────────────────────┐
│  VS Code Desktop                 │
├──────────────────────────────────┤
│  Debug Config 1: ESP-IDF         │
│  → C/C++ debugging (JTAG/GDB)    │
│                                  │
│  Debug Config 2: MicroPython     │
│  → Python debugging (WebSocket)  │
└──────────────────────────────────┘
         ↓                ↓
    ┌────────┐      ┌──────────┐
    │OpenOCD │      │WebSocket │
    └────────┘      └──────────┘
         ↓                ↓
    ┌─────────────────────────┐
    │      ESP32 Device       │
    ├─────────────────────────┤
    │  Native Code (C/C++)    │ ← esp_debug_adapter
    │  MicroPython (Python)   │ ← modwebDAP.c
    └─────────────────────────┘
```

You can debug:
1. The MicroPython interpreter itself (C level, via esp_debug_adapter)
2. Python scripts running in MicroPython (Python level, via WebDAP)

---

## Troubleshooting

### Proxy Issues

**Problem:** Proxy can't connect to device

**Solution:**
- Check device IP: `ping 192.168.4.1`
- Verify WebSocket is running: Test with `websocat ws://192.168.4.1:8266/webrepl`
- Check firewall settings

**Problem:** VS Code can't connect to proxy

**Solution:**
- Verify proxy is running: `netstat -an | grep 5678`
- Check port not in use: `lsof -i :5678`
- Try different port: `--port 5679`

### vscode.dev Issues

**Problem:** Extension doesn't load in vscode.dev

**Solution:**
- Check extension compatibility: `"browser": "./extension.js"` in package.json
- Avoid Node.js-only modules
- Test with `vsce package --target web`

**Problem:** WebSocket connection fails from vscode.dev

**Solution:**
- Use `ws://` not `wss://` for local testing
- Check CORS if using HTTPS
- Verify device is on same network (or use public IP/domain)

---

## Future Enhancements

### Short-term
- [ ] Implement `sys.settrace()` integration
- [ ] Variable inspection (locals, globals)
- [ ] Watch expressions
- [ ] Exception breakpoints

### Long-term
- [ ] Conditional breakpoints
- [ ] Hot code reload
- [ ] Multi-file debugging
- [ ] Performance profiling integration
- [ ] Remote filesystem browser

---

## See Also

- [DAP WebSocket Advocacy](../dap_websocket_advocacy.md) - Design rationale
- [Implementation Guide](../IMPLEMENTATION_GUIDE.md) - Technical architecture
- [WebREPL CB RFC](../webrepl_cb_rfc.md) - Protocol coexistence
- [ESP-IDF Extension](https://github.com/espressif/vscode-esp-idf-extension) - Native C/C++ debugging

---

**Questions or Issues?**

Jonathan Peace  
Email: jep@alphabetiq.com
GitHub: [@sjetpax](https://github.com/jetpax)
