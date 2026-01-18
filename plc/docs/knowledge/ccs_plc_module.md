# Knowledge Item: CCS/NACS PLC Module Implementation

**Created:** 2026-01-16  
**Category:** ESP32 Modules, CCS Charging, HomePlug  
**Related Files:** `/Users/jep/github/pyDirect/plc/`

---

## Summary

This knowledge item documents the implementation of a CCS/NACS DC fast charging communication module for ESP32-P4 with MicroPython. The module enables the device to communicate with electric vehicles using the HomePlug Green PHY powerline protocol, supporting EVSE (charger) mode with planned PEV (vehicle) and listen modes.

---

## Key Concepts

### What This Module Does

1. **SLAC (Signal Level Attenuation Characterization)**: Network pairing protocol between EV and charger over the Control Pilot (CP) line
2. **HomePlug Green PHY**: Powerline communication carrying data at ~10 Mbps over the CP wire
3. **V2G (Vehicle-to-Grid)**: Application layer protocol for charging negotiation (DIN 70121)
4. **EXI Encoding**: Efficient XML Interchange - compact binary format for V2G messages

### Hardware Requirements

- **ESP32-P4** with Ethernet (built-in EMAC + IP101GRI PHY)
- **TP-Link TL-PA4010P** HomePlug modem (Qualcomm QCA7420 chipset)
- **CCS/NACS connector** with CP, PP, PE connections
- **CP coupling circuit** (1kHz PWM + transformer for PLC injection)

---

## Architecture

```
Browser (Scripto Studio)
    │
    ▼ WebREPL
ESP32-P4 MicroPython
    │
    ├── lib/CCS/CCS_helpers.py (V2G state machine, TCP server)
    │
    └── plc C Module
        ├── modplc.c (SLAC state machine, L2TAP, MicroPython API)
        └── exi_din.c (DIN 70121 EXI codec)
            │
            ▼ Ethernet (EtherType 0x88E1)
        TP-Link HomePlug Modem
            │
            ▼ PLC over CP line
        Electric Vehicle
```

---

## Module API

### SLAC Functions

```python
import plc

plc.start_evse()           # Start SLAC responder (EVSE mode)
plc.stop()                 # Stop module
plc.set_key(nid, nmk)      # Set 7-byte NID + 16-byte NMK
plc.set_callback(fn)       # fn(car_mac) called on SLAC completion
plc.get_status()           # → {'state': 'MATCHED', 'car_mac': '...', ...}
plc.get_modem_info()       # Query modem firmware version
```

### EXI Functions

```python
# Decode incoming request (bytes → dict)
msg = plc.exi_decode(tcp_data)
# → {'type': 'PreChargeReq', 'EVTargetVoltage': 4000, ...}

# Encode response (string + params → bytes with V2GTP header)
response = plc.exi_encode('PreChargeRes', {'EVSEPresentVoltage': 3950})
```

### Supported EXI Messages

| Decode (Requests) | Encode (Responses) |
|-------------------|-------------------|
| SupportedAppProtocolReq | SupportedAppProtocolRes |
| SessionSetupReq | SessionSetupRes |
| ServiceDiscoveryReq | ServiceDiscoveryRes |
| ServicePaymentSelectionReq | ServicePaymentSelectionRes |
| ContractAuthenticationReq | ContractAuthenticationRes |
| ChargeParameterDiscoveryReq | ChargeParameterDiscoveryRes |
| CableCheckReq | CableCheckRes |
| PreChargeReq | PreChargeRes |
| PowerDeliveryReq | PowerDeliveryRes |
| CurrentDemandReq | CurrentDemandRes |
| WeldingDetectionReq | WeldingDetectionRes |
| SessionStopReq | SessionStopRes |

---

## SLAC State Machine (EVSE Mode)

```
IDLE
  ↓ plc.start_evse()
WAIT_PARAM_REQ ← waiting for car's SLAC_PARAM.REQ
  ↓ → send SLAC_PARAM.CNF
WAIT_ATTEN_CHAR ← receive MNBC_SOUND.IND × 10
  ↓ → send ATTEN_CHAR.IND
WAIT_MATCH_REQ ← receive SLAC_MATCH.REQ
  ↓ → send SLAC_MATCH.CNF (with NID/NMK)
  ↓ → send SET_KEY to modem
MATCHED → callback(car_mac)
  ↓
(IP network active, start TCP server on port 15118)
```

---

## V2G Flow (EVSE Mode)

After SLAC, the car connects via TCP and exchanges V2G messages:

1. **Handshake**: SupportedAppProtocol → select DIN 70121
2. **Session Setup**: Establish session ID
3. **Service Discovery**: Offer DC charging service
4. **Payment Selection**: External payment (no contract)
5. **Authentication**: No authentication needed
6. **Charge Parameters**: Exchange max voltage/current/power
7. **Cable Check**: Isolation test (simulated OK)
8. **PreCharge**: EVSE ramps voltage toward EV target
9. **Power Delivery (Start)**: **🎉 CONTACTORS CLOSE**
10. **Current Demand**: Active charging loop
11. **Power Delivery (Stop)**: End charging
12. **Welding Detection**: Contactor weld check
13. **Session Stop**: Clean up

---

## File Structure

```
pyDirect/plc/
├── modplc.c           # Main C module (SLAC + MicroPython wrappers)
├── exi_din.c          # DIN EXI codec (decode + encode)
├── exi_din.h          # EXI codec header
├── micropython.cmake  # CMake build config
├── micropython.mk     # Makefile build config
├── README.md          # Usage documentation
├── DESIGN.md          # Comprehensive design document
└── test_plc.py        # Python test script
```

---

## Build Configuration

Enable PLC module in firmware build:

```bash
cd micropython/ports/esp32
make BOARD=SCRIPTO_P4 USER_C_MODULES=/path/to/pyDirect/micropython.cmake \
     CMAKE_ARGS="-DMODULE_PYDIRECT_PLC=ON"
```

---

## TP-Link Modem Configuration

### EVSE Mode (Coordinator)
```bash
setpib evse.pib 74 hfid "EVSE"
setpib evse.pib F4 byte 2             # CCo capability
setpib evse.pib 1653 byte 2           # CCo selection
plctool -ieth0 -P evse.pib <modem_mac>
```

### PEV Mode (Station)
```bash
setpib pev.pib 74 hfid "PEV"
setpib pev.pib F4 byte 0              # Not CCo
setpib pev.pib 1653 byte 0            # Station mode
plctool -ieth0 -P pev.pib <modem_mac>
```

---

## Implementation Status

| Feature | EVSE Mode | PEV Mode | Listen Mode |
|---------|-----------|----------|-------------|
| SLAC State Machine | ✅ Complete | ❌ TODO | N/A |
| MME Frame Handling | ✅ Complete | ⚠️ Partial | ⚠️ Partial |
| EXI Decode | ✅ Requests | ❌ Responses | ✅ Both |
| EXI Encode | ✅ Responses | ❌ Requests | N/A |
| V2G TCP Server | ⚠️ Python | N/A | N/A |
| V2G TCP Client | N/A | ❌ Python | N/A |
| Scripto Extension | ✅ Complete | ❌ TODO | ❌ TODO |

---

## PEV Mode (Planned)

Required work to enable vehicle emulation:

1. **SLAC Initiator**: New state machine (SEND_PARAM_REQ → SEND_SOUNDS → SEND_MATCH_REQ → MATCHED)
2. **SDP Client**: UDP multicast to discover charger IP
3. **EXI Request Encoding**: Add templates for all *Req messages
4. **EXI Response Decoding**: Parse all *Res messages
5. **Modem PIB**: Switch modem to station mode

**Estimated effort**: ~2 weeks

---

## Listen Mode (Planned)

Required work for passive monitoring:

1. **Passive Capture**: Receive all 0x88E1 frames without transmitting
2. **NMK Extraction**: Parse SLAC_MATCH frames to get network key
3. **Dynamic Key Update**: Configure modem with captured NMK
4. **Frame Streaming**: Real-time display of decoded messages
5. **PCAP Export**: Save captures for Wireshark analysis

**Estimated effort**: ~1 week

---

## EXI Codec Design Notes

The codec uses a **pattern matching + template** approach rather than full EXI parsing:

### Decode (Pattern Matching)
- DIN messages start with `0x809a`
- Bytes 2-4 encode message type with distinct patterns
- Extract key fields from known offsets

### Encode (Templates)
- Pre-encoded messages from pyPLC test vectors
- Modify specific bytes for dynamic fields (e.g., voltage)
- Always includes V2GTP 8-byte header

This approach is:
- ✅ Fast and memory-efficient
- ✅ Works for known message set
- ❌ Not schema-aware (can't handle unknown messages)
- ❌ May need updates for different EV implementations

---

## Key References

| Resource | Purpose |
|----------|---------|
| [pyPLC](https://github.com/uhi22/pyPLC) | Reference Python implementation |
| [OpenV2G](https://github.com/Martin-P/OpenV2G) | C EXI codec (schema-aware) |
| [open-plc-utils](https://github.com/qca/open-plc-utils) | HomePlug modem tools |
| ISO 15118-3 | HomePlug Green PHY specification |
| DIN 70121 | DC charging protocol |

---

## Related Conversations

- This knowledge item was created during the CCS/NACS EVSE MVP implementation session
- See `/Users/jep/github/pyDirect/plc/DESIGN.md` for full design document

---

## Quick Start

```python
# 1. Start CP PWM
from machine import PWM, Pin
cp = PWM(Pin(4), freq=1000, duty_u16=int(65535 * 0.05))

# 2. Configure and start PLC
import plc
import os

plc.set_key(os.urandom(7), os.urandom(16))
plc.set_callback(lambda mac: print(f"SLAC done: {mac}"))
plc.start_evse()

# 3. Check status
print(plc.get_status())

# 4. Handle V2G (in Python TCP server)
msg = plc.exi_decode(tcp_data)
if msg['type'] == 'PreChargeReq':
    response = plc.exi_encode('PreChargeRes', {'EVSEPresentVoltage': voltage})
    conn.send(response)
```

---

*Last Updated: 2026-01-16*
