# pyDirect: High-Performance Web Accelerators for MicroPython on ESP32

**pyDirect** is a suite of C-based accelerator modules that provide high-performance web functionality for ESP32 MicroPython applications. By implementing web protocols directly in C with minimal Python overhead, pyDirect achieves near-native performance for web servers, static file serving, WebSocket communication, and WebREPL.

## Architecture

pyDirect follows a clean, modular design where each module has a specific responsibility:

```
                      ┌───────────────────────────────────────────────────────────┐
                      │        Application Layer ( MicroPython Code)              │
                      │         - start/stop/configure pyDirect components        │
                      │         - handle incoming WebREPL commands                │
                      └───────────────────────────────────────────────────────────┘
                        ↓                        ↓                           ↓ 
┌──────────────────────────────┐  ┌──────────────────────────────────┐  ┌─────────────────────────────────┐
│  WebREPL (Protocol Layer)    │  │  webDAP (Protocol Layer)         │  │  webfiles (Static File Server)  │
│  - Password authentication   │  │  - Debug Adapter Protocol (DAP)  │  │  - Direct file serving          │
│  - RAW REPL state machine    │  │  - Breakpoint management         │  │  - Gzip compression support     │
│  - Binary file transfer      │  │  - Stack trace inspection        │  │  - MIME type detection          │
│  - REPL output redirection   │  │  - Variable inspection           │  │  - Cache control                │
└──────────────────────────────┘  └──────────────────────────────────┘  └─────────────────────────────────┘
                          ↓                           ↓                     ↓ 
                    ┌──────────────────────────────────────┐                │
                    │  wsserver (WebSocket Transport)      │                │
                    │  - WebSocket connection management   │                │
                    │  - Ping/pong keep-alive              │                │
                    │  - Activity tracking                 │                │
                    │  - Binary/text frame handling        │                │
                    └──────────────────────────────────────┘                │
                                              ↓                             │
                         ┌──────────────────────────────────────────────────────┐   
                         │      httpserver (HTTP Core)                          │  
                         │      - ESP-IDF HTTP server wrapper                   │
                         │      - Dynamic request routing                       │
                         │      - Message queue for Python handlers             │
                         │      - Shared server handle                          │
                         └──────────────────────────────────────────────────────┘
```

## Key Features

- **Automatic Keep-Alive**: WebSocket ping/pong detects dead connections
- **Modular Design**: Each module can be used independently
- **Thread-Safe**: Proper GIL management for multi-threaded ESP-IDF
- **Native Performance**: Near-native transfer speeds for files and data
- **C API**: Modules can integrate with each other at the C level for maximum efficiency
- **Pre-Queue Filtering**: Urgent messages (like Ctrl+C) bypass the queue for immediate handling
- **Python Independent**: websockets keep running even if MicroPython blocked/halted


### How It Works

```
┌─────────────────────────────────────────────────────┐
│  HTTP Worker Thread (ESP-IDF)                       │
│                                                     │
│  1. WebSocket frame received from client            │
│  2. ✓ Call pre-queue filter callback (if registered)│
│     ├─ Returns false → Message handled, SKIP QUEUE  │
│     └─ Returns true  → Continue with normal queuing │
│  3. httpserver_queue_message() [if not filtered]    │
│                                                     │
└─────────────────────────────────────────────────────┘
                      ↓ (queued)
┌─────────────────────────────────────────────────────┐
│  Main MicroPython Task                              │
│                                                     │
│  1. httpserver.process_queue()                      │
│  2. Dequeue message                                 │
│  3. Call protocol callback (e.g., webrepl handler)  │
│                                                     │
└─────────────────────────────────────────────────────┘
```

### Example: WebREPL Ctrl+C Handler

```c
// In modwebrepl.c
static bool webrepl_prequeue_filter(int client_id, const uint8_t *data, 
                                     size_t len, bool is_binary) {
    // Only intercept single-byte text messages that are Ctrl+C
    if (!is_binary && len == 1 && data[0] == '\x03') {
        // mp_sched_keyboard_interrupt() is thread-safe
        mp_sched_keyboard_interrupt();
        mp_hal_wake_main_task();  // Wake main task to process interrupt
        return false;  // Don't queue - we handled it
    }
    
    return true;  // Queue all other messages normally
}

// Register in webrepl_start()
wsserver_register_c_callback(WSSERVER_EVENT_PREQUEUE_FILTER, 
                              (void *)webrepl_prequeue_filter);
```

### Key Points

- **Thread Context**: Pre-queue filters run in the HTTP worker thread, not the main MicroPython task
- **Thread-Safe APIs Only**: Use only thread-safe functions like `mp_sched_keyboard_interrupt()` and `mp_hal_wake_main_task()`
- **Return Value**: `false` = handled/skip queue, `true` = queue normally
- **Generic Design**: `wsserver` has no WebREPL-specific logic; any protocol can register filters
- **Use Cases**: Interrupts, keepalive responses, protocol negotiation, out-of-band signaling

## Modules

### httpserver - Dynamic HTTP Request Handling
- Start/stop HTTP server on specified port
- Register URI handlers for GET and POST requests
- Asynchronous request processing using FreeRTOS queues
- Support for up to 5 concurrent URI handlers
- Thread-safe message passing between ESP-IDF HTTP server task and MicroPython task

### webfiles - Static File Serving
- Serve static files from MicroPython VFS (default) or ESP-IDF `/www` partition
- Automatic MIME type detection
- Gzip compression support (.gz files)
- Cache control headers
- CORS headers for development
- **Note:** Use `webfiles.serve()` for most cases (works with any MicroPython VFS path). Use `webfiles.serve_www()` only when files are on a separate `/www` partition.

### wsserver - WebSocket Server with Keep-Alive
- Generic WebSocket server for real-time communication
- Automatic ping/pong keep-alive (detects dead clients)
- Activity tracking with configurable timeout
- Support for both Python and C callbacks
- Binary and text frame support
- Multiple concurrent clients
- **Pre-queue filter callbacks** for urgent messages (e.g., Ctrl+C interrupts)

### webrepl - WebREPL Protocol
- Remote Python REPL over WebSocket
- Password authentication
- Binary file upload/download
- UART output redirection to WebSocket
- Uses wsserver for connection management and keep-alive

### webdap - Debug Adapter Protocol (Beta/WIP)
- **Status**: Beta/Work in Progress - Not yet fully functional
- Debug Adapter Protocol (DAP) implementation for MicroPython debugging
- Protocol multiplexing: Uses WebSocket TEXT frames (opcode 0x01) while WebREPL CB uses binary frames (opcode 0x02)
- Coexists with WebREPL on the same WebSocket connection
- Implements DAP specification: https://microsoft.github.io/debug-adapter-protocol/
- **Current capabilities**:
  - DAP protocol initialization and handshake
  - Breakpoint management (set/clear breakpoints)
  - Basic debugging commands (continue, threads, stackTrace, scopes)
  - Launch and attach requests (skeleton implementation)
- **Limitations** (WIP):
  - Breakpoint verification not yet implemented
  - Stack trace inspection requires `sys.settrace()` integration (TODO)
  - Variable inspection not yet implemented
  - Step debugging (stepIn, stepOut, next) not yet functional
  - Frame inspection requires MicroPython execution context integration

## API

### `httpserver.start(port=None)`

Start the HTTP server. If `port` is not specified, uses ESP-IDF default port.

**Parameters:**
- `port` (int, optional): Port number to listen on

**Returns:**
- `bool`: True if server started successfully, False otherwise

### `httpserver.on(uri, handler, method="GET")`

Register a URI handler function.

**Parameters:**
- `uri` (str): URI pattern to match
- `handler` (callable): Function to call when URI is requested
- `method` (str, optional): HTTP method ("GET" or "POST")

**Returns:**
- `int`: Handler ID if successful, raises exception otherwise

**Handler Function Signatures:**
- GET: `def handler(uri): return response_string`
- POST: `def handler(uri, post_data): return response_string`

### `httpserver.send(content)`

Send a response from within a handler function. Alternative to returning a string.

**Parameters:**
- `content` (str): Response content to send

**Returns:**
- `bool`: True if sent successfully, False otherwise

### `httpserver.stop()`

Stop the HTTP server.

**Returns:**
- `bool`: True if server stopped successfully, False otherwise

### `httpserver.process_queue()`

Process queued HTTP requests. Should be called regularly in the main loop.

**Returns:**
- `int`: Number of requests processed

## webfiles Module API

### `webfiles.serve(base_path, uri_prefix)`

Serve static files from a directory.

**Parameters:**
- `base_path` (str): Filesystem path to serve files from (e.g., "/files")
- `uri_prefix` (str): URI prefix pattern (e.g., "/*" for all paths)

**Returns:**
- `bool`: True if file server registered successfully, False otherwise

### `webfiles.serve_file(file_path, uri)`

Serve a specific file at a specific URI (not yet implemented).

**Parameters:**
- `file_path` (str): Path to the file to serve
- `uri` (str): URI to serve the file at

**Returns:**
- `bool`: True if successful, False otherwise

### `webfiles.serve_www(uri_prefix)`

Serve files directly from the `/www` partition using ESP-IDF VFS (bypasses MicroPython).

**Parameters:**
- `uri_prefix` (str): URI prefix pattern (e.g., "/" or "/static/*")

**Returns:**
- `bool`: True if file server registered successfully, False otherwise

**Note:** Requires the www partition to be mounted first using `webfiles.mount_www()`.

### Choosing Between `webfiles.serve()` and `webfiles.serve_www()`

**Important:** It is very difficult to access MicroPython VFS from ESP-IDF code unless files are on a separate partition. For this reason, **`webfiles.serve()` should be used in most cases**.

| Method | Handler | When to Use |
|--------|---------|-------------|
| `webfiles.serve()` | `webfiles_handler` | **Default choice** - Works with any MicroPython VFS path, no special partition setup required |
| `webfiles.serve_www()` | `webfiles_direct_handler` | Only when files are pre-populated on a separate `/www` partition for production deployments |

**Use `webfiles.serve()` when:**
- Files are in the standard MicroPython filesystem
- You need flexibility to serve from any directory
- You're developing or prototyping
- Files may be created/modified at runtime

**Use `webfiles.serve_www()` when:**
- Files are pre-populated on a dedicated `/www` partition
- You need maximum performance for production static assets
- Files are read-only and won't change at runtime
- You've already set up the partition table with a `www` partition

### Constants

- `webfiles.MIME_HTML` = "text/html"
- `webfiles.MIME_JS` = "application/javascript"
- `webfiles.MIME_CSS` = "text/css"
- `webfiles.MIME_JSON` = "application/json"
- `webfiles.MIME_TEXT` = "text/plain"
- `webfiles.MIME_BINARY` = "application/octet-stream"

## webdap Module API (Beta/WIP)

### `webdap.start()`

Start the WebDAP server and register DAP protocol handlers with wsserver.

**Returns:**
- `bool`: True if started successfully, False otherwise

**Note:** Requires `wsserver` to be running. DAP messages are handled via WebSocket TEXT frames (opcode 0x01), allowing coexistence with WebREPL CB on binary frames (opcode 0x02).

### `webdap.stop()`

Stop the WebDAP server and clean up breakpoints.

**Returns:**
- `None`

### `webdap.is_connected()`

Check if a DAP client is currently connected and initialized.

**Returns:**
- `bool`: True if DAP client is connected and initialized, False otherwise

**Note:** This is a beta feature. Many DAP capabilities are not yet fully implemented. See module description above for current limitations.

## Usage Example

### HTTP Server with Dynamic Handlers

```python
from esp32 import httpserver
import time

def handle_root(uri):
    return "<html><body><h1>Hello World!</h1><p>URI: " + uri + "</p></body></html>"

def handle_post(uri, post_data):
    return "<html><body><h1>POST Received</h1><p>Data: " + post_data + "</p></body></html>"

# Start server
httpserver.start(8080)

# Register handlers
httpserver.on("/", handle_root)
httpserver.on("/api", handle_post, "POST")

# Main loop
while True:
    httpserver.process_queue()
    time.sleep_ms(10)
```

### Static File Serving with webfiles

```python
from esp32 import httpserver, webfiles
import time

# Start HTTP server
httpserver.start(8080)

# Serve static files from /files directory
webfiles.serve("/files", "/*")

# Main loop - still need to process queued requests
while True:
    httpserver.process_queue()
    time.sleep_ms(10)
```

### Combined: Dynamic Handlers + Static Files

```python
from esp32 import httpserver, webfiles
import time

def handle_api(uri, post_data):
    return '{"status": "ok", "data": "' + post_data + '"}'

# Start server
httpserver.start(8080)

# Dynamic API endpoint
httpserver.on("/api", handle_api, "POST")

# Serve static files (HTML, CSS, JS) from /files
webfiles.serve("/files", "/*")

# Main loop
while True:
    httpserver.process_queue()
    time.sleep_ms(10)
```

### Debug Adapter Protocol with webdap (Beta/WIP)

```python
from esp32 import httpserver, wsserver, webdap
import time

# Start HTTP server
httpserver.start(8080)

# Start WebSocket server (required for webdap)
wsserver.start()

# Start WebDAP server for debugging
webdap.start()

# Main loop
while True:
    httpserver.process_queue()
    time.sleep_ms(10)
    
    # Check if DAP client is connected
    if webdap.is_connected():
        # DAP debugging is active
        pass
```

**Note:** This is a beta feature. The DAP protocol will be available on WebSocket TEXT frames, while WebREPL CB continues to work on binary frames. Many debugging features (breakpoints, step debugging, variable inspection) are not yet fully implemented.

## Building

### Make-based build

Add to your MicroPython build command:
```bash
make USER_C_MODULES=/path/to/usermod_httpserver
```

### CMake-based build

Create a top-level `micropython.cmake` that includes this module:
```cmake
include(/path/to/usermod_httpserver/micropython.cmake)
```

Then build with:
```bash
make USER_C_MODULES=/path/to/top-level/micropython.cmake
```

## Implementation Notes

- Built on ESP-IDF HTTP server for native performance
- Uses MicroPython's object model (`mp_obj_t`) and exception handling (`nlr` mechanism)
- Module registration uses `MP_REGISTER_MODULE`
- Thread-safe design with proper GIL management
- Direct C APIs between modules for maximum efficiency

## Features

### httpserver Module
- Asynchronous request handling via FreeRTOS queues
- POST data handling
- Custom response content or return values
- Wildcard URI matching support

### webfiles Module  
- Serve files from MicroPython VFS (default) or ESP-IDF `/www` partition
- Efficient file serving with minimal Python overhead
- Automatic MIME type detection (20+ file types)
- Gzip compression support (serves .gz files automatically)
- Smart caching (1 hour for static assets, no-cache for HTML)
- CORS headers for development
- Query parameter handling

## Limitations

- Maximum 5 concurrent URI handlers (httpserver)
- Queue size limited to 10 messages (httpserver)
- Processes up to 5 messages per `process_queue()` call
- Requires regular calling of `process_queue()` in main loop
- Fixed 4KB chunk size for file transfers

## Thread Safety

The modules are designed to be thread-safe:
- ESP-IDF HTTP server runs in its own task
- Message passing uses FreeRTOS queues and mutexes
- All MicroPython function calls happen in the main MicroPython task context
- File serving happens directly in ESP-IDF task (no Python interaction)
