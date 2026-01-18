# Changelog - httpserver/wsserver/webrepl

## [Unreleased]

### Fixed

#### Character-by-Character Output Bug (2025-12-23)
- **Problem**: Drain task in `modwebrepl.c` was sending output character-by-character, resulting in 13+ WebSocket frames for a simple `print({"result": 42})` 
- **Root Cause**: No batching delay in drain task - it sent data immediately as soon as any bytes were available in ring buffer
- **Fix**: Added 20ms batching delay in `wbp_drain_task()` after semaphore wake (line 1111)
- **Impact**: Reduced WebSocket message count by ~10-15x, dramatically improved network efficiency
- **Client Impact**: All clients MUST buffer RES messages until PRO arrives (JavaScript client already did this correctly)

```c
// modwebrepl.c - wbp_drain_task()
while (g_output_ring.active) {
    xSemaphoreTake(g_output_ring.data_available, pdMS_TO_TICKS(20));
    
    // NEW: Wait 20ms for more data to accumulate before sending
    vTaskDelay(pdMS_TO_TICKS(20));
    
    while (g_output_ring.active) {
        size_t bytes_read = wbp_ring_read(chunk, DRAIN_CHUNK_SIZE);
        // ...
```

**Test Results**:
- Before: 13 separate RES messages (one per character)
- After: 1 RES message with complete output
- Latency impact: +20ms (imperceptible to users)

#### WebSocket Connection Cleanup Bug (2025-12-23)
- **Problem**: When WebSocket sends failed (e.g., broken connection, PING failure), the client slot remained allocated and WebREPL became completely unresponsive - required device restart
- **Root Cause**: `wsserver_send_to_client()` C API function did not handle fatal send errors like the Python `wsserver.send()` function did
- **Fix**: Added error handling and client cleanup to `wsserver_send_to_client()` in `modwsserver.c`
- **Impact**: WebSocket connections now properly clean up on fatal send errors, allowing new connections to reuse the client slot

```c
// modwsserver.c - wsserver_send_to_client()
if (ret != ESP_OK) {
    free(data_copy);
    ESP_LOGE(TAG, "Failed to send to client %d: error %d (0x%x)", client_id, ret, ret);
    
    // NEW: Check for fatal errors that indicate dead connection
    if (ret == ESP_ERR_HTTPD_INVALID_REQ || ret == ESP_ERR_HTTPD_RESP_SEND) {
        ESP_LOGW(TAG, "Fatal send error to client %d, cleaning up connection", client_id);
        handle_client_disconnect(client_id);  // Clean up the dead connection
    }
    
    return false;
}
```

**Test**: After this fix, failed sends should automatically clean up the connection, allowing new clients to connect without requiring a device restart.

---

## Testing

### Character-by-Character Fix
```bash
cd /Users/jep/github/pyDirect/httpserver/tests
source venv/bin/activate
python -c "
import asyncio
import websockets
import cbor2
import ssl

async def test():
    ssl_context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ssl_context.check_hostname = False
    ssl_context.verify_mode = ssl.CERT_NONE
    
    ws = await websockets.connect('wss://192.168.1.154/webrepl',
                                  subprotocols=['webrepl.binary.v1'],
                                  ping_interval=20, ssl=ssl_context)
    await ws.send(cbor2.dumps([0, 0, 'password']))
    await ws.recv()  # Auth response
    
    # Send test
    await ws.send(cbor2.dumps([2, 0, 'print({\"result\": 42})', 0, 'test-id']))
    
    # Count RES messages
    count = 0
    while True:
        msg = cbor2.loads(await ws.recv())
        if msg[0] == 2 and msg[1] == 0:  # RES
            count += 1
        elif msg[0] == 2 and msg[1] == 2:  # PRO
            break
    
    print(f'RES messages: {count} (should be 1-3, not 13+)')
    await ws.close()

asyncio.run(test())
"
```

### Connection Cleanup Fix
To test, intentionally break a connection and verify new connections work:
1. Connect with client
2. Break connection (close client abruptly or disable PING/PONG)
3. Wait for "Failed to send" errors in device logs
4. Attempt new connection - should succeed (previously would hang/fail)

---

## Files Modified

- `httpserver/modwebrepl.c`: Added batching delay, updated `wbp_send_cbor()` to return bool
- `httpserver/modwsserver.c`: Added fatal error handling and cleanup to `wsserver_send_to_client()`
- `httpserver/tests/webrepl_client.py`: Added RES message buffering (like JavaScript client)
- `httpserver/BUGS.md`: Documented both bugs with root cause analysis
- `httpserver/CHANGELOG.md`: This file

---

## Breaking Changes

**None** - These are bug fixes that restore intended behavior. However:

- WebREPL clients MUST buffer RES messages until PRO arrives (was already required by protocol spec, but some clients might have been relying on broken behavior)
- Clients should implement proper PING/PONG handling (20s interval recommended)
