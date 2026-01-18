# WebREPL Module

WebREPL (Web-based REPL) implementation for embedded Python, providing remote Python shell access via WebSocket and WebRTC transports.

## Overview

The WebREPL module enables remote interactive Python shell access to ESP32 devices through web browsers or compatible clients. It supports two transport mechanisms:

1. **WebSocket** - Traditional WebREPL over WebSocket (compatible with standard WebREPL clients)
2. **WebRTC** - Modern WebREPL over WebRTC DataChannel (browser-native, NAT-traversal capable)

## Features

- **Dual Transport** - WebSocket and WebRTC support
- **Binary Protocol** - Efficient CBOR-based message encoding
- **File Transfer** - Upload/download files to device
- **Password Protection** - Secure access control
- **Browser Compatible** - Works in any modern browser
- **NAT Traversal** - WebRTC mode works through firewalls

## Components

This module consolidates two previously separate implementations:

- **`modwebrepl.c`** - WebSocket transport (originally from httpserver)
- **`modwebrepl_rtc.c`** - WebRTC transport (originally from webrtc)

## Dependencies

- **httpserver** - For WebSocket transport
- **webrtc** - For WebRTC transport
- **CBOR** - For binary protocol encoding

## Python API

### WebSocket Transport

```python
import webrepl_binary

# Start WebREPL on WebSocket
webrepl_binary.start()

# Stop WebREPL
webrepl_binary.stop()

# Set password
webrepl_binary.set_password("mypassword")
```

### WebRTC Transport

```python
import webrepl_rtc

# WebRTC transport is automatically available
# when webrtc module is enabled
# Access via browser at: https://device-ip/webrepl
```

## Usage

### WebSocket Mode

1. **Start WebREPL on device:**
   ```python
   import webrepl_binary
   webrepl_binary.start()
   ```

2. **Connect from browser:**
   - Navigate to: `wss://device-ip/webrepl`
   - Or use WebREPL client: [webrepl.html](https://micropython.org/webrepl/)

### WebRTC Mode

1. **WebRTC is auto-enabled** when webrtc module is active

2. **Connect from browser:**
   - Navigate to: `https://device-ip/`
   - WebRTC connection established automatically
   - REPL available in browser console

## Protocol

WebREPL uses a binary protocol based on CBOR encoding:

### Message Format

```
[opcode, data]
```

**Opcodes:**
- `0x01` - Execute Python code
- `0x02` - Get file
- `0x03` - Put file
- `0x04` - List directory
- `0x05` - Interrupt (Ctrl+C)

### Binary Protocol Advantages

- **Compact** - CBOR encoding is more efficient than JSON
- **Type-Safe** - Preserves Python types (bytes, int, float, etc.)
- **Fast** - Binary parsing is faster than text

## Security

### Password Protection

```python
import webrepl_binary

# Set password (stored in flash)
webrepl_binary.set_password("secure_password")

# Disable password (not recommended)
webrepl_binary.set_password(None)
```

### HTTPS/WSS

WebREPL over WebSocket uses secure WebSocket (WSS) when HTTPS is enabled:

```python
# In main.py
HTTPS_ENABLED = True
HTTPS_CERT_FILE = '/certs/servercert.pem'
HTTPS_KEY_FILE = '/certs/prvtkey.pem'
```

## File Transfer

### Upload File

```python
# From WebREPL client
webrepl.put_file("local_file.py", "/remote_file.py")
```

### Download File

```python
# From WebREPL client
webrepl.get_file("/remote_file.py", "local_file.py")
```

## Build Configuration

Enable in `build.sh`:
```bash
./build.sh BOARD httpserver webrtc  # Enables both transports
```

Or in CMake:
```cmake
-DMODULE_PYDIRECT_HTTPSERVER=ON  # WebSocket transport
-DMODULE_PYDIRECT_WEBRTC=ON      # WebRTC transport
```

## Client Integration

### Browser Client

```javascript
// WebSocket
const ws = new WebSocket('wss://device-ip/webrepl');
ws.onmessage = (event) => {
  const data = CBOR.decode(event.data);
  console.log('Received:', data);
};

// WebRTC
const pc = new RTCPeerConnection();
const dc = pc.createDataChannel('webrepl');
dc.onmessage = (event) => {
  const data = CBOR.decode(event.data);
  console.log('Received:', data);
};
```

### Python Client

```python
import websocket
import cbor2

ws = websocket.WebSocket()
ws.connect('wss://device-ip/webrepl')

# Send command
ws.send(cbor2.dumps([0x01, 'print("Hello")']))

# Receive response
response = cbor2.loads(ws.recv())
print(response)
```

## Comparison: WebSocket vs WebRTC

| Feature | WebSocket | WebRTC |
|---------|-----------|--------|
| **Setup** | Simple | Complex (signaling) |
| **NAT Traversal** | Requires port forwarding | Built-in STUN/TURN |
| **Latency** | Low | Very low |
| **Browser Support** | Universal | Modern browsers only |
| **Firewall** | May be blocked | Usually works |

## Troubleshooting

**Cannot connect via WebSocket:**
- Check HTTPS is enabled
- Verify WebSocket endpoint: `wss://device-ip/webrepl`
- Check firewall rules

**Cannot connect via WebRTC:**
- Ensure WebRTC module is enabled
- Check browser console for errors
- Verify STUN/TURN configuration

**Password not working:**
- Password is case-sensitive
- Check password was set: `webrepl_binary.set_password("...")`
- Try resetting: `webrepl_binary.set_password(None)` then set again

## See Also

- [WebREPL Protocol Specification](https://github.com/micropython/webrepl)
- [CBOR Encoding](https://cbor.io/)
- [WebRTC DataChannel API](https://developer.mozilla.org/en-US/docs/Web/API/RTCDataChannel)
- [httpserver Module](../httpserver/README.md)
- [webrtc Module](../webrtc/README.md)
