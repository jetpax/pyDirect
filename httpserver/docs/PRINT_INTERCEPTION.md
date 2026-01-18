# WebREPL Output Redirection using dupterm

## Overview

This document explains how the ScriptO Studio WebREPL implementation redirects MicroPython's `print()` output to the WebSocket client using MicroPython's official `dupterm` mechanism with a non-blocking ring buffer architecture.

## Architecture

### The Flow

```
print("Hello")
  ↓
mp_builtin_print()
  ↓
mp_hal_stdout_tx_strn()
  ↓
uart_stdout_tx_strn()     → UART hardware (normal output)
  ↓
mp_os_dupterm_tx_strn()   → Our WebREPL stream
  ↓
webrepl_stream.write()    → Ring buffer (non-blocking)
  ↓
Drain Task                → WebSocket (blocking OK in separate task)
  ↓
WebSocket client receives output
```

### Key Components

1. **WebREPL Stream Type** - A MicroPython native stream type registered with `os.dupterm()`
2. **Ring Buffer** - Non-blocking circular buffer (8KB) for output data
3. **Drain Task** - FreeRTOS task that sends buffered data to WebSocket
4. **dupterm Integration** - Uses MicroPython's official terminal duplication mechanism

## Why dupterm?

Previous implementations used GCC's `--wrap` linker feature to intercept `uart_stdout_tx_strn()`. While this worked, it had drawbacks:

1. **Blocking WebSocket sends** - The MicroPython task could block on network I/O
2. **Non-standard approach** - Linker wrapping is a hack, not the official mechanism
3. **No partial write handling** - If buffer was full, data could be lost

The new dupterm-based approach provides:

1. **Non-blocking print()** - Output goes to ring buffer immediately
2. **Proper MicroPython integration** - Uses the official dupterm mechanism
3. **Graceful handling of slow clients** - Ring buffer absorbs bursts
4. **Clean lifecycle management** - dupterm automatically handles attach/detach

## Implementation Details

### WebREPL Stream Type

```c
// Stream object for dupterm
typedef struct _webrepl_stream_obj_t {
    mp_obj_base_t base;
    int client_id;
} webrepl_stream_obj_t;

// Stream protocol - write goes to ring buffer (non-blocking)
static mp_uint_t webrepl_stream_write(mp_obj_t self_in, const void *buf, 
                                       mp_uint_t size, int *errcode) {
    size_t written = webrepl_ring_write((const uint8_t *)buf, size);
    if (written == 0 && size > 0) {
        *errcode = MP_EAGAIN;  // Buffer full
        return MP_STREAM_ERROR;
    }
    return written;
}
```

### Ring Buffer

The 8KB ring buffer provides:
- **Non-blocking writes** from MicroPython task
- **Thread-safe access** via FreeRTOS mutex
- **Semaphore signaling** to wake drain task when data available

```c
#define WEBREPL_OUTPUT_RING_SIZE 8192

typedef struct {
    uint8_t buffer[WEBREPL_OUTPUT_RING_SIZE];
    volatile size_t head;  // Write position
    volatile size_t tail;  // Read position
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t data_available;
    TaskHandle_t drain_task;
    volatile bool active;
} webrepl_output_ring_t;
```

### Drain Task

The drain task runs in a separate FreeRTOS task at lower priority than MicroPython:

```c
static void webrepl_drain_task(void *pvParameters) {
    uint8_t chunk[1024];
    
    while (g_output_ring.active) {
        // Wait for data
        xSemaphoreTake(g_output_ring.data_available, pdMS_TO_TICKS(100));
        
        // Send all available data
        while (g_output_ring.active) {
            size_t bytes = webrepl_ring_read(chunk, sizeof(chunk));
            if (bytes == 0) break;
            
            // This may block - OK in this task
            wsserver_send_to_client(client_id, chunk, bytes, false);
            taskYIELD();
        }
    }
}
```

## Lifecycle

### Client Authentication

When a WebREPL client successfully authenticates:

```c
if (password_correct) {
    g_webrepl_output_client_id = client_id;
    webrepl_dupterm_attach(client_id);  // Register with os.dupterm
}
```

### Client Disconnect

On disconnect (from HTTP worker thread):

```c
// Stop drain task immediately (safe from any thread)
webrepl_drain_task_stop();
g_webrepl_output_client_id = -1;

// Queue disconnect for MP context to detach dupterm
webrepl_queue_message(WEBREPL_MSG_DISCONNECT, client_id, NULL, 0);
```

Then in MicroPython task context:

```c
case WEBREPL_MSG_DISCONNECT:
    // Safe to call mp_os_dupterm here
    webrepl_dupterm_detach();
    break;
```

## Benefits Over Previous Approach

| Feature | Linker Wrapping | dupterm + Ring Buffer |
|---------|-----------------|------------------------|
| print() blocking | Could block on network | Non-blocking (ring buffer) |
| MicroPython integration | Hack (linker wrap) | Official mechanism |
| Build complexity | Required --wrap flags | Standard build |
| Portability | ESP32-specific | Works on any port |
| Buffer overflow | Data lost | Graceful EAGAIN |
| UART output | Suppressed when active | Both work simultaneously |

## Debugging

Enable debug logs to see the flow:

```
Ring buffer: wrote 42/42 bytes
Drain task: sending 42 bytes to client 0
dupterm attached for client 0
```

## Configuration

- `WEBREPL_OUTPUT_RING_SIZE` - Ring buffer size (default 8KB)
- `DRAIN_CHUNK_SIZE` - Max bytes per WebSocket send (default 1KB)

## Summary

The dupterm-based output redirection:

1. Registers a custom stream with `os.dupterm(stream, 0)`
2. Stream's `write()` pushes data to a non-blocking ring buffer
3. A dedicated drain task sends buffered data to WebSocket
4. MicroPython's print() never blocks on network I/O
5. Clean lifecycle via dupterm attach/detach

This provides a robust, non-blocking, and officially-supported way to redirect MicroPython output to WebREPL clients.
