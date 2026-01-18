# Known Bugs - httpserver/wsserver/webrepl

This file tracks known bugs discovered during testing and development.

## Critical Bugs

### 1. WebREPL drain task sends output character-by-character (HIGH PRIORITY)

**Status**: 🔴 CRITICAL - Causes massive performance degradation and protocol inefficiency

**Discovered**: 2025-12-23 during HIL test development

**Symptom**:
- M2M execution results are sent as individual characters in separate WebSocket frames
- Example: `print({"result": 42})` results in 15+ separate WS messages:
  ```
  [2, 0, '{', 'test-id']
  [2, 0, "'", 'test-id']
  [2, 0, 'r', 'test-id']
  [2, 0, 'e', 'test-id']
  ...
  ```
- Clients must buffer ALL RES messages before PRO arrives
- Massive WebSocket overhead (CBOR encoding + WS frame per character!)
- Poor network efficiency

**Root Cause**:
Located in `modwebrepl.c` lines 1095-1131, `wbp_drain_task()` function:

```c
static void wbp_drain_task(void *pvParameters) {
    // ...
    while (g_output_ring.active) {
        xSemaphoreTake(g_output_ring.data_available, pdMS_TO_TICKS(20));
        
        while (g_output_ring.active) {
            size_t bytes_read = wbp_ring_read(chunk, DRAIN_CHUNK_SIZE);  // ← Reads whatever is available
            
            if (bytes_read == 0) {
                break;  // ← Immediately breaks if nothing available
            }
            
            // ← No batching/delay here!
            int client_id = g_wbp_output_client_id;
            if (client_id >= 0) {
                wbp_send_result(client_id, g_wbp_current_channel, chunk, bytes_read, g_wbp_current_id);
                // ← Sends immediately, even if only 1 byte!
            }
        }
        
        taskYIELD();
    }
}
```

**The Problem**:
1. MicroPython's `print()` calls `wbp_stream_write()` (line 1187), which writes to ring buffer
2. MicroPython may write character-by-character or in very small chunks (implementation detail)
3. Each `wbp_ring_write()` signals `g_output_ring.data_available` semaphore
4. Drain task wakes up immediately, reads the small chunk, and sends it
5. No batching or delay to accumulate more data before sending
6. Result: 1 character = 1 WebSocket frame = 1 CBOR encoded array

**Impact**:
- **Network Efficiency**: 15-20x more WebSocket frames than necessary
- **CPU Usage**: CBOR encoding overhead for each character
- **Protocol Complexity**: All clients MUST buffer RES messages (not optional)
- **Latency**: Actually increases due to many small frames vs fewer large frames
- **Bandwidth**: Huge overhead from WebSocket framing and CBOR encoding per-character

**Fix Required**:

Option 1: **Add batching delay in drain task** (RECOMMENDED)
```c
static void wbp_drain_task(void *pvParameters) {
    // ...
    while (g_output_ring.active) {
        xSemaphoreTake(g_output_ring.data_available, pdMS_TO_TICKS(20));
        
        // Wait a short time for more data to accumulate
        vTaskDelay(pdMS_TO_TICKS(10));  // 10ms batch window
        
        while (g_output_ring.active) {
            size_t bytes_read = wbp_ring_read(chunk, DRAIN_CHUNK_SIZE);
            
            if (bytes_read == 0) {
                break;
            }
            
            int client_id = g_wbp_output_client_id;
            if (client_id >= 0) {
                wbp_send_result(client_id, g_wbp_current_channel, chunk, bytes_read, g_wbp_current_id);
            }
        }
        
        taskYIELD();
    }
}
```

Option 2: **Read minimum threshold before sending**
```c
// Only send if we have at least 64 bytes OR ring is > 50% full OR timeout
#define MIN_SEND_THRESHOLD 64
#define BATCH_TIMEOUT_MS 10

while (g_output_ring.active) {
    size_t available = wbp_ring_available();
    
    if (available < MIN_SEND_THRESHOLD) {
        // Wait for more data or timeout
        xSemaphoreTake(g_output_ring.data_available, pdMS_TO_TICKS(BATCH_TIMEOUT_MS));
        available = wbp_ring_available();
    }
    
    if (available > 0) {
        size_t bytes_read = wbp_ring_read(chunk, DRAIN_CHUNK_SIZE);
        if (bytes_read > 0 && client_id >= 0) {
            wbp_send_result(client_id, g_wbp_current_channel, chunk, bytes_read, g_wbp_current_id);
        }
    }
}
```

Option 3: **Signal semaphore less frequently** (more complex)
- Only signal `data_available` once per batch, not on every write
- Track if semaphore was already signaled
- Requires changes to `wbp_ring_write()` and drain task

**Recommended Fix**: Option 1 (simplest, effective, low latency impact)
- Add 10ms delay after semaphore wake to allow batching
- Still responsive (10ms is imperceptible to users)
- Dramatically reduces message count (single batch instead of N characters)

**Workaround for Clients**:
All WebREPL clients MUST buffer RES messages until PRO arrives:
```python
# Python client example
response_buffers = {}

def handle_res(msg_id, data):
    if msg_id not in response_buffers:
        response_buffers[msg_id] = []
    response_buffers[msg_id].append(data)

def handle_pro(msg_id, status):
    full_response = ''.join(response_buffers.get(msg_id, []))
    # Process full_response
    del response_buffers[msg_id]
```

JavaScript client (webrepl-wcb.js) already implements this correctly (line 240).

---

### 2. WebREPL becomes unresponsive after failed PING (HIGH PRIORITY)

**Status**: 🔴 CRITICAL - Requires device restart to recover

**Discovered**: 2025-12-23 during HIL test development

**Symptom**: 
- WebSocket server sends PING to client
- Client fails to respond or connection fails during PING
- Error logged: `E (53611) WSSERVER: Failed to send PING to client 0: 258`
- WebREPL becomes completely unresponsive to subsequent connection attempts
- No recovery possible without hard device restart

**Impact**:
- Breaks WebREPL functionality completely
- Requires physical device restart
- Cannot recover programmatically
- Affects production deployments if clients disconnect improperly

**Root Cause (Hypothesis)**:
- WebSocket connection cleanup not properly releasing resources when PING fails
- Client slot (client_id 0) likely remains in use/locked state
- Connection state machine doesn't properly transition to closed state on PING failure
- Resources (memory, file descriptors, task handles) not freed

**Reproduction Steps**:
1. Connect WebREPL client without proper PING/PONG handling (e.g., `ping_interval=None` in websockets library)
2. Wait 20-30 seconds for server to attempt PING
3. Observe error: `E (53611) WSSERVER: Failed to send PING to client 0: 258`
4. Client connection drops
5. Attempt to reconnect - connection refused or hangs
6. WebREPL completely unresponsive

**Workaround**:
- Ensure all WebSocket clients implement proper PING/PONG handling
- Set reasonable `ping_interval` and `ping_timeout` in client library
- For `websockets` Python library: `ping_interval=20, ping_timeout=10`

**Fix Required**:
1. Investigate `wsserver` connection cleanup in `modwebrepl.c` and `modwsserver.c`
2. Check PING failure handling
3. Ensure failed PING properly:
   - Closes WebSocket connection
   - Releases client slot (client_id)
   - Frees all associated resources
   - Resets connection state machine
   - Allows new connections to same client slot
4. Add defensive cleanup on connection errors
5. Add connection timeout/watchdog if client becomes unresponsive
6. Add diagnostic logging for connection state transitions

**Files to Investigate**:
- `/Users/jep/github/pyDirect/httpserver/modwebrepl.c` - WebREPL protocol handler
- `/Users/jep/github/pyDirect/httpserver/modwsserver.c` - WebSocket server core
- Connection state management
- Client slot allocation/deallocation
- PING/PONG handling code

---

## Bug Reporting

When reporting bugs, please include:
1. Symptom description
2. Reproduction steps
3. Expected vs actual behavior
4. Device logs (if available)
5. Device model and firmware version
6. Network conditions (WiFi, Ethernet, etc.)

---

## Fix Priority Legend

- 🔴 **CRITICAL**: Causes system failure or major performance degradation
- 🟠 **HIGH**: Major functionality broken, workaround exists
- 🟡 **MEDIUM**: Feature partially broken, minor impact
- 🟢 **LOW**: Cosmetic or minor inconvenience
