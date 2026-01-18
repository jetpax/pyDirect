# CAN Manager Python API

## Overview

The CAN Manager Python API exposes the existing C-level CAN manager functions to Python, enabling multiple Python scripts and C modules to share the CAN bus safely without conflicts.

## Motivation

Previously, the CAN manager was only accessible from C code (e.g., GVRET module). Python code had to use the legacy `CAN()` instance API, which internally used the manager but didn't expose direct control. This prevented Python extensions like DBE from registering as explicit CAN clients alongside C modules.

With this update, Python code can now:
- Register as independent CAN clients
- Coexist peacefully with C modules (GVRET, etc.)
- Share the bus with other Python scripts
- Control activation/deactivation independently

## API Functions

### Module-Level Functions

#### `CAN.register(mode) -> handle`

Register a new CAN client with the manager.

**Parameters:**
- `mode` (int): Client mode constant
  - `CAN.TX_ENABLED` - Client can transmit and receive
  - `CAN.RX_ONLY` - Client can only receive (listen-only)

**Returns:**
- `handle` (int): Client handle for use with other functions

**Example:**
```python
import CAN

# Register a TX-capable client
handle = CAN.register(CAN.TX_ENABLED)
```

**Notes:**
- Registration does NOT activate the bus - use `activate()` to start
- Multiple clients can be registered simultaneously
- Each client gets a unique handle

---

#### `CAN.set_loopback(enabled)`

Enable or disable loopback mode for testing/development.

**Parameters:**
- `enabled` (bool): `True` to enable loopback, `False` to disable

**Example:**
```python
# Enable loopback for testing (no external hardware needed)
CAN.set_loopback(True)

# Register and use as normal
handle = CAN.register(CAN.TX_ENABLED)
CAN.activate(handle)
# Bus will use TWAI_MODE_NO_ACK (allows self-RX without external ACK)
```

**Notes:**
- **Global bus setting** - affects ALL clients on the bus
- Call BEFORE activating clients for immediate effect
- Can be called while bus is running (triggers reconfiguration)
- **Testing only** - disable for production/real CAN bus
- Loopback mode uses `TWAI_MODE_NO_ACK` instead of `TWAI_MODE_NORMAL`

---

#### `CAN.activate(handle)`

Activate a registered client, starting the CAN bus if needed.

**Parameters:**
- `handle` (int): Client handle from `register()`

**Example:**
```python
CAN.activate(handle)
```

**Notes:**
- Bus mode is determined by active clients:
  - If any TX-capable clients are active: `NORMAL` mode
  - If only RX-only clients are active: `LISTEN_ONLY` mode
  - If no clients are active: `STOPPED` mode
- Safe to call multiple times (idempotent)

---

#### `CAN.set_rx_callback(handle, callback)`

Set an RX callback function for a client.

**Parameters:**
- `handle` (int): Client handle
- `callback` (function): Callback function or `None` to disable

**Callback signature:**
```python
def callback(frame):
    # frame is a dict with:
    #   'id': int - CAN identifier
    #   'data': bytes - Frame data (up to 8 bytes)
    #   'extended': bool - True if extended ID
    #   'rtr': bool - True if remote transmission request
```

**Example:**
```python
def on_can_rx(frame):
    print(f"RX: ID=0x{frame['id']:03X}, Data={frame['data'].hex()}")

CAN.set_rx_callback(handle, on_can_rx)
```

**Notes:**
- All active clients receive ALL frames (broadcast)
- Callback is called from CAN manager task context
- Keep callbacks fast to avoid blocking the bus

---

#### `CAN.can_transmit(handle, frame)`

Transmit a CAN frame.

**Parameters:**
- `handle` (int): Client handle
- `frame` (dict): Frame dictionary with:
  - `'id'` (int): CAN identifier (11-bit or 29-bit)
  - `'data'` (bytes): Frame data (up to 8 bytes)
  - `'extended'` (bool, optional): True for 29-bit ID (default: False)
  - `'rtr'` (bool, optional): True for RTR frame (default: False)

**Example:**
```python
# Standard frame
frame = {
    'id': 0x123,
    'data': b'\x11\x22\x33\x44',
    'extended': False
}
CAN.can_transmit(handle, frame)

# Extended frame
frame = {
    'id': 0x1FFFFFFF,
    'data': b'\xAA\xBB\xCC',
    'extended': True
}
CAN.can_transmit(handle, frame)
```

**Notes:**
- Client must be TX-capable (`TX_ENABLED`)
- Client must be activated before transmitting
- Raises `RuntimeError` if transmission fails

---

#### `CAN.can_deactivate(handle)`

Deactivate a client, stopping the bus if no other clients are active.

**Parameters:**
- `handle` (int): Client handle

**Example:**
```python
CAN.can_deactivate(handle)
```

**Notes:**
- Bus mode is recalculated based on remaining active clients
- Safe to call multiple times (idempotent)
- Client remains registered (can be reactivated later)

---

#### `CAN.can_unregister(handle)`

Unregister a client completely.

**Parameters:**
- `handle` (int): Client handle

**Example:**
```python
CAN.can_unregister(handle)
```

**Notes:**
- Automatically deactivates if client was active
- Frees client resources
- Handle becomes invalid after unregistering

---

## Constants

### Client Modes

- `CAN.CLIENT_TX_ENABLED` - Client can transmit and receive
- `CAN.CLIENT_RX_ONLY` - Client can only receive (listen-only)

### Existing CAN Constants

All existing CAN module constants remain available:
- `CAN.NORMAL`, `CAN.LOOPBACK`, `CAN.SILENT`, etc.
- These are used by the legacy `CAN()` instance API

---

## Complete Example

```python
import CAN
import time
from lib import board

# Get board CAN pins
can_bus = board.can("twai")

# Register a CAN client
handle = CAN.register(CAN.TX_ENABLED)

# Set RX callback
rx_count = 0

def on_can_rx(frame):
    global rx_count
    rx_count += 1
    print(f"RX #{rx_count}: ID=0x{frame['id']:03X}, Data={frame['data'].hex()}")

CAN.set_rx_callback(handle, on_can_rx)

# Activate client (bus starts)
CAN.activate(handle)

# Transmit some frames
for i in range(5):
    frame = {
        'id': 0x100 + i,
        'data': bytes([i, i+1, i+2, i+3]),
        'extended': False
    }
    CAN.can_transmit(handle, frame)
    time.sleep_ms(10)

# Wait for any pending RX
time.sleep_ms(100)

print(f"Total received: {rx_count}")

# Cleanup
CAN.can_deactivate(handle)
CAN.can_unregister(handle)
```

---

## Coexistence with Legacy API

The new manager API coexists peacefully with the legacy `CAN()` instance API:

```python
# Legacy API (still works)
can = CAN(0, tx=4, rx=5, mode=CAN.NORMAL, bitrate=500000)
can.send([0x01, 0x02], 0x123)

# New manager API (can run simultaneously)
handle = CAN.register(CAN.TX_ENABLED)
CAN.activate(handle)
CAN.can_transmit(handle, {'id': 0x456, 'data': b'\xAA\xBB'})
```

Both clients share the same CAN bus via the manager. The bus mode is automatically determined by all active clients.

---

## Coexistence with C Modules

Python clients can coexist with C modules like GVRET:

```python
# GVRET already running (C module using CAN manager)

# Python client can register and use the bus
handle = CAN.register(CAN.TX_ENABLED)
CAN.activate(handle)

# Both GVRET and Python client receive ALL frames
# Both can transmit without conflicts
```

---

## Thread Safety

All CAN manager operations are mutex-protected and thread-safe. Multiple clients can register, activate, and transmit from different tasks without conflicts.

---

## Testing

See `test_can_manager_api.py` for a comprehensive test script.

To test:
1. Flash MicroPython firmware with updated TWAI module
2. Upload test script to device
3. Run: `import test_can_manager_api`

---

## Implementation Details

### Architecture Overview

The CAN Manager uses a hybrid approach for thread-safe operation:

**TX (Transmit) Flow:**
- Direct call to `twai_transmit_v2()` from Python context
- No background task needed (TWAI driver has built-in TX queue)
- Mutex-protected client validation before transmission

**RX (Receive) Flow:**
- Background task polls `twai_receive_v2()` 
- Queues raw frame data in FreeRTOS queue (no Python object allocation)
- Calls `mp_sched_schedule()` to trigger processing
- Processor runs in main MicroPython task, creates Python objects, dispatches callbacks

This pattern ensures:
- No Python object allocation from background tasks (prevents crashes)
- Thread-safe callback dispatch via `mp_sched_schedule()`
- Efficient use of TWAI driver's built-in queuing

### Handle Storage

Handles are stored as Python integers (pointer cast). This is safe because:
- Handles are opaque pointers managed by the C manager
- The manager maintains a reference count for each client
- Handles remain valid until `can_unregister()` is called

### Callback GC Safety

Python callbacks are stored in the client's `void *arg` field. The MicroPython GC keeps the callback alive as long as:
- The client is registered
- The callback is referenced in the `arg` field

When a client is unregistered, the callback reference is freed automatically.

### Bus State Management

The CAN manager automatically adjusts bus mode based on active clients:

| Active Clients | Bus Mode |
|----------------|----------|
| 1+ TX-enabled  | NORMAL (or NO_ACK if loopback) |
| RX-only only   | LISTEN_ONLY |
| None           | STOPPED  |

This ensures optimal bus mode for all active clients without manual intervention.

---

## Files Modified

1. **`mod_can.c`**: Added Python wrapper functions and module-level exports
2. **`mod_can.h`**: Added documentation for Python callback handling
3. **`DBE_helpers.py`**: Updated to use new CAN manager API
4. **`test_can_manager_api.py`**: Comprehensive test script

---

## Migration Guide

### For Extensions Using Legacy API

**Before:**
```python
import CAN

can_dev = CAN(0, tx=4, rx=5, mode=CAN.NORMAL, bitrate=500000)
can_dev.send([0x01, 0x02], 0x123)

if can_dev.any():
    id, ext, rtr, data = can_dev.recv()
```

**After (using manager API):**
```python
import CAN

# Register and activate
handle = CAN.register(CAN.TX_ENABLED)

# Set RX callback instead of polling
def on_rx(frame):
    # Process frame
    pass

CAN.can_set_rx_callback(handle, on_rx)
CAN.activate(handle)

# Transmit
frame = {'id': 0x123, 'data': b'\x01\x02'}
CAN.can_transmit(handle, frame)

# Cleanup
CAN.can_deactivate(handle)
CAN.can_unregister(handle)
```

### Benefits of Migration

- Explicit client registration (better visibility)
- Callback-based RX (no polling needed)
- Safe coexistence with other clients
- Dynamic bus mode management

---

## Future Enhancements

Potential future additions:
- `CAN.can_add_filter()` - Add ID filters per client
- `CAN.can_set_mode()` - Change client mode dynamically
- `CAN.can_get_stats()` - Get client statistics
- Support for CAN-FD frames

---

## License

MIT License (same as MicroPython and pyDirect)
