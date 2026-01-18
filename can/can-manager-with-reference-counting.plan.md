<!-- d8bb8c5d-d63a-4301-b9eb-496d4b07d415 c8e3e6eb-da2f-4a21-a333-735ef56f36bb -->
# CAN Manager Implementation Plan - Integrated into mod_can.c

## Overview

Integrate CAN bus manager functionality directly into `mod_can.c`:

- Extend existing singleton `esp32_can_obj` with client tracking
- Add two-stage registration API: `can_register()` → `can_activate()`
- Implement automatic state management based on activated clients
- Provide unified API for all CAN users (GVRET, OVMS, OI, WebREPL)

## Architecture

### Two-Stage Client Lifecycle

1. **Registered**: Client calls `can_register()` - registered but bus stays STOPPED
2. **Activated**: Client calls `can_activate()` - bus activates based on state machine

### State Machine (based on ACTIVATED clients only)

```
activated_transmitting_clients > 0 → NORMAL mode (ACKs enabled)
activated_clients > 0 (no TX)     → LISTEN_ONLY mode (passive, no ACKs)
activated_clients == 0            → STOPPED (driver stopped, zero power)
```

## Implementation Steps

### 1. Extend mod_can.c with Manager Functionality

**Add to `esp32_can_obj_t` structure (in mod_can.h):**

```c
typedef void (*can_rx_callback_t)(const twai_message_t *frame, void *arg);

typedef enum {
    CAN_CLIENT_MODE_RX_ONLY,      // RX only, contributes to LISTEN_ONLY requirement
    CAN_CLIENT_MODE_TX_ENABLED,   // TX capable, contributes to NORMAL requirement
    CAN_CLIENT_MODE_FORCE_LISTEN // TX capable but force LISTEN_ONLY (passive monitoring)
} can_client_mode_t;

typedef struct can_client {
    uint32_t client_id;
    bool is_registered;
    bool is_activated;
    can_client_mode_t mode;  // Single field replaces needs_tx + force_listen_only
    can_rx_callback_t rx_callback;
    void *rx_callback_arg;
    struct can_client *next;
} can_client_t;

// Add to esp32_can_obj_t:
can_client_t *clients;  // Linked list of registered clients
uint32_t registered_clients;
uint32_t activated_clients;
uint32_t activated_transmitting_clients;
QueueHandle_t tx_queue;  // Shared TX queue
TaskHandle_t rx_dispatcher_task;  // RX dispatcher task
```

**Add Manager API Functions (in mod_can.c):**

- `can_handle_t can_register(bool needs_tx, bool force_listen_only)` - Register client (bus stays STOPPED)
- `esp_err_t can_activate(can_handle_t h)` - Activate client
- `esp_err_t can_deactivate(can_handle_t h)` - Deactivate client
- `void can_unregister(can_handle_t h)` - Unregister client
- `void can_set_rx_callback(can_handle_t h, can_rx_callback_t cb, void *arg)` - Subscribe to RX
- `esp_err_t can_add_filter(can_handle_t h, uint32_t id, uint32_t mask)` - Optional filtering
- `esp_err_t can_transmit(can_handle_t h, const twai_message_t *msg)` - Queue TX frame
- `void update_bus_state(void)` - Internal state machine

**State Management:**

- `update_bus_state()` checks **activated** client counts:
  - If `activated_transmitting_clients > 0` AND no clients force LISTEN_ONLY: Stop driver, reconfigure to NORMAL, start driver
  - Else if `activated_clients > 0`: Stop driver, reconfigure to LISTEN_ONLY, start driver
  - Else: Stop and uninstall driver (bus fully STOPPED)

### 2. Update CAN Module Initialization

**Modify `esp32_can_init_helper()`:**

- Instead of directly installing/starting driver, call `can_register(true, false)` + `can_activate()`
- This allows CAN module to participate in manager's state machine

**Modify `can_deinit()`:**

- Call `can_unregister()` to remove CAN module from manager
- Manager handles actual driver stop/uninstall

### 3. Update GVRET to Use Two-Stage Registration

**Changes to `pyDirect/gvret/modgvret.c`:**

**Stage 1 - Registration (in `gvret_start()`):**

- Call `can_register(true, false)` - registers client, bus stays STOPPED
- Call `can_set_rx_callback()` - sets up RX callback
- Store handle in `gvret_cfg.can_handle`

**Stage 2 - Activation (when TCP client connects):**

- When `accept()` succeeds, call `can_activate(gvret_cfg.can_handle)`
- Bus activates to NORMAL mode

**Deactivation (when TCP client disconnects):**

- Call `can_deactivate(gvret_cfg.can_handle)`
- Bus may go to LISTEN_ONLY or STOPPED

**Unregistration (in `gvret_stop()`):**

- Call `can_unregister(gvret_cfg.can_handle)`

### 4. Update OVMS/OpenInverter Extensions

- Remove `gvret.stop()` calls - manager handles coordination
- CAN module (used by OI/OVMS) automatically participates in manager

### 5. RX Dispatcher Task

**In mod_can.c:**

- Single RX task reads from TWAI driver (only runs when bus is activated)
- Dispatches frames to all **activated** clients via callbacks
- Filters applied per-client if `can_add_filter()` was called

### 6. TX Queue Task

**In mod_can.c:**

- Single TX task processes queue (only runs when bus is activated)
- `can_transmit()` adds to queue
- TX task calls `twai_transmit_v2()`
- Only accepts TX from activated clients with TX capability

## Files to Modify

1. **Modify:** `pyDirect/twai/mod_can.c` - Add manager functionality
2. **Modify:** `pyDirect/twai/mod_can.h` - Add manager API declarations and structures
3. **Modify:** `pyDirect/gvret/modgvret.c` - Use two-stage manager API
4. **Modify:** `scripto-studio-registry/Extensions/OVMS/lib/OVMS_helpers.py` - Remove GVRET stop calls
5. **Modify:** `scripto-studio-registry/Extensions/OpenInverter/lib/OI_helpers.py` - Remove GVRET stop calls

## Key Design Decisions

- **Integrated into mod_can.c** - Single source of truth, simpler architecture
- **Two-stage registration** - Allows clients to register without activating bus
- **State machine based on activated clients** - Registered clients don't affect bus state
- **Clients can request LISTEN_ONLY** - Even if TX capable, can force passive mode
- **Mode switching requires stop/reconfigure/start** - ESP-IDF limitation
- **RX callbacks only fire for activated clients** - Registered but inactive clients don't receive frames
- **Client persistence** - All clients persist across WebREPL disconnect (GVRET, OVMS, OI, WebREPL scripts)
- **Auto-reload on reconnect** - Clients automatically re-register when WebREPL reconnects
- **Client list cleared only on hard reboot** - Client state survives WebREPL disconnects and CAN deinit()

## Client Lifecycle & Persistence

### Persistence Strategy

- **Static client list** - Stored in static memory, survives WebREPL disconnect
- **Client IDs** - Each client gets unique ID (incrementing counter or pointer-based hash)
- **State preservation** - Registration and activation state preserved across disconnects

### Auto-Reload Mechanism

- **GVRET**: On `gvret_start()`, check if handle already exists, restore if found
- **OVMS/OI**: On extension start, check if CAN module initialized, reuse if so
- **WebREPL scripts**: Scripts re-execute on reconnect, will re-register (idempotent)

### Clear Triggers

- **Hard reboot** - Memory cleared, client list reset
- **Explicit clear** - `can_manager.clear_all_clients()` for testing/debugging
- **NOT cleared on**: WebREPL disconnect, CAN deinit(), soft reset
- **Client persistence** - All clients persist across WebREPL disconnect (GVRET, OVMS, OI, WebREPL scripts)
- **Auto-reload on reconnect** - Clients automatically re-register when WebREPL reconnects
- **Client list cleared only on hard reboot** - Client state survives WebREPL disconnects and CAN deinit()

## Client Lifecycle Management

### Persistence Strategy

- **Registered clients persist** - Client list maintained in static memory, survives WebREPL disconnect
- **Auto-reload mechanism** - On WebREPL reconnect, extensions/clients check if already registered and restore state
- **Clear triggers** - Client list only cleared on:
  - Hard reboot/reset (memory cleared)
  - Explicit `can_manager.clear_all_clients()` call (for testing/debugging)

### Implementation Details

**Client Registration Tracking:**

- Each client gets unique ID (incrementing counter or hash)
- Client list stored in static memory (survives WebREPL disconnect)
- On reconnect, clients check `can_is_registered(client_id)` before re-registering

**Auto-Reload Mechanism:**

- GVRET: On `gvret_start()`, check if already registered (by client ID), if yes restore handle
- OVMS/OI: On extension start, check if CAN module already initialized, reuse if so
- WebREPL scripts: Scripts re-execute on reconnect, will re-register (but can check first)

**State Restoration:**

- Registered clients maintain their registration state
- Activated clients maintain activation state (bus stays active)
- RX callbacks restored from client structure
- Filters restored from client structure

### To-dos

- [x] Add client tracking structures and enums to mod_can.h
- [x] Add manager API function declarations to mod_can.h
- [x] Extend esp32_can_obj_t with client tracking fields in mod_can.c
- [x] Implement can_register() - two-stage registration (bus stays STOPPED)
- [x] Implement can_activate() and can_deactivate() functions
- [x] Implement can_set_mode() with conflict checking
- [x] Implement update_bus_state() state machine
- [x] Implement RX dispatcher task for multiple clients
- [x] Implement TX queue task
- [x] Update GVRET to use two-stage registration API
- [x] Handle SETUP_CANBUS listen-only flag in GVRET
- [x] Update CAN module init/deinit to use manager
- [x] Remove GVRET stop calls from OVMS extension
- [x] Remove GVRET stop calls from OpenInverter extension