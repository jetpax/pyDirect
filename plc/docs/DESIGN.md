# CCS/NACS PLC Module Design Document

**Version:** 1.0  
**Date:** 2026-01-16  
**Author:** JetPax / Gemini  
**Status:** EVSE Mode Implemented, PEV/Listen Modes Planned

---

## Table of Contents

1. [Project Overview](#project-overview)
2. [Architecture](#architecture)
3. [Hardware Setup](#hardware-setup)
4. [Protocol Stack](#protocol-stack)
5. [EVSE Mode (Implemented)](#evse-mode-implemented)
6. [PEV Mode (Planned)](#pev-mode-planned)
7. [Listen/Sniff Mode (Planned)](#listensniff-mode-planned)
8. [EXI Codec Design](#exi-codec-design)
9. [V2G Message Flow](#v2g-message-flow)
10. [Implementation Status](#implementation-status)
11. [Future Enhancements](#future-enhancements)
12. [References](#references)

---

## Project Overview

### Goal

Build a minimal viable product (MVP) for CCS/NACS DC fast charging communication using:
- **ESP32-P4** with built-in Ethernet
- **TP-Link TL-PA4010P** HomePlug modem
- **MicroPython** for high-level logic
- **C module** for performance-critical SLAC and EXI handling

### Primary Use Case: EVSE Mode

The initial target is to emulate a CCS charger (EVSE) to make a Tesla Model Y (or any CCS/NACS compatible EV) close its DC fast charging contactors. This involves:
1. Generating the correct Control Pilot (CP) PWM signal
2. Completing the SLAC handshake to form a HomePlug network
3. Running the V2G protocol (DIN 70121) to negotiate charging
4. Simulating PreCharge voltage to trigger contactor closure

### Secondary Use Cases

- **PEV Mode**: Act as a vehicle to communicate with real chargers
- **Listen Mode**: Passively monitor CCS communication between car and charger

---

## Architecture

### System Diagram

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    Scripto Studio (Browser)                             │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐                     │
│  │   Config    │  │   Status    │  │   V2G       │                     │
│  │   Panel     │  │   Panel     │  │   Panel     │                     │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘                     │
│         └────────────────┼────────────────┘                             │
│                          │ WebREPL / device.exec()                      │
└──────────────────────────┼──────────────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    ESP32-P4 MicroPython                                 │
│                                                                         │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │                lib/CCS/CCS_helpers.py                             │  │
│  │  • startCCS() / stopCCS()                                         │  │
│  │  • SLAC callback handler                                          │  │
│  │  • V2G TCP server (port 15118)                                    │  │
│  │  • PreCharge voltage simulation                                   │  │
│  └───────────────────────────────────────────────────────────────────┘  │
│                          │                                              │
│                          ▼                                              │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │                plc C Module (modplc.c)                            │  │
│  │                                                                   │  │
│  │  SLAC Layer:                    EXI Layer:                        │  │
│  │  • L2TAP (EtherType 0x88E1)     • exi_decode() → dict             │  │
│  │  • SLAC state machine           • exi_encode() → bytes            │  │
│  │  • MME frame construction       • V2GTP header handling           │  │
│  │  • FreeRTOS task               • DIN 70121 templates              │  │
│  │  • Python callback             • Pattern-based detection          │  │
│  └───────────────────────────────────────────────────────────────────┘  │
│                          │                                              │
│                          ▼                                              │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │                ESP-IDF / Hardware Layer                           │  │
│  │  • esp_eth (RMII → IP101GRI PHY)                                  │  │
│  │  • esp_vfs_l2tap (raw Ethernet frames)                            │  │
│  │  • LEDC PWM (CP signal @ 1kHz)                                    │  │
│  │  • GPIO (PP resistor sensing)                                     │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
                           │
                           ▼ Ethernet (RMII)
┌─────────────────────────────────────────────────────────────────────────┐
│              TP-Link TL-PA4010P HomePlug Modem                          │
│              (Configured as EVSE or PEV via PIB)                        │
└─────────────────────────────────────────────────────────────────────────┘
                           │
                           ▼ PLC over CP line
┌─────────────────────────────────────────────────────────────────────────┐
│              CCS/NACS Connector → Electric Vehicle                      │
└─────────────────────────────────────────────────────────────────────────┘
```

### Design Decisions

| Decision | Rationale |
|----------|-----------|
| **C module for SLAC** | Timing-critical MME handling, FreeRTOS task for reliable response |
| **C module for EXI** | EXI parsing is complex; pattern matching is faster in C |
| **Python for V2G state machine** | Easier iteration, extensibility; TCP server is not timing-critical |
| **L2TAP VFS interface** | Socket-like API coexists with standard IP stack |
| **Template-based EXI encoding** | Full EXI encoding would require significant code; templates work for known responses |
| **DIN 70121 only (MVP)** | Tesla and most EVs support DIN; ISO 15118-2 can be added later |

---

## Hardware Setup

### Components

| Component | Purpose | Notes |
|-----------|---------|-------|
| **ESP32-P4** | Main controller | Built-in Ethernet MAC, runs MicroPython |
| **IP101GRI** | Ethernet PHY | RMII interface, on Waveshare module |
| **TP-Link TL-PA4010P** | HomePlug modem | Qualcomm QCA7420 chipset |
| **PWM circuit** | CP signal generation | 1kHz, 5% duty = DC charging request |
| **Coupling transformer** | PLC injection | Couples HomePlug signal onto CP line |
| **PP resistor** | Cable rating | 1.5kΩ = 13A, 680Ω = 20A, 220Ω = 32A |

### Pin Assignments (SCRIPTO_P4 Board)

| Signal | GPIO | Notes |
|--------|------|-------|
| CP PWM | GPIO4 | 1kHz PWM output |
| PP ADC | GPIO5 | Analog read for cable detection |
| ETH_MDC | GPIO31 | RMII management clock |
| ETH_MDIO | GPIO27 | RMII management data |
| ETH_REF_CLK | GPIO50 | 50MHz from PHY |
| ETH_TXD0/1 | GPIO33/34 | RMII TX data |
| ETH_RXD0/1 | GPIO28/29 | RMII RX data |
| ETH_TX_EN | GPIO32 | RMII TX enable |
| ETH_CRS_DV | GPIO30 | RMII RX data valid |

### TP-Link Modem PIB Configuration

The modem must be configured for the correct role:

**EVSE Mode (Coordinator):**
```bash
# Using open-plc-utils on Linux
cp original.pib evse.pib
setpib evse.pib 74 hfid "EVSE"        # Human-readable ID
setpib evse.pib F4 byte 2             # CCo capability
setpib evse.pib 1653 byte 2           # CCo selection
setpib evse.pib 1C98 long 10240 long 102400  # Timing
plctool -ieth0 -P evse.pib <modem_mac>
```

**PEV Mode (Station):**
```bash
cp original.pib pev.pib
setpib pev.pib 74 hfid "PEV"
setpib pev.pib F4 byte 0              # Not CCo
setpib pev.pib 1653 byte 0            # Station mode
plctool -ieth0 -P pev.pib <modem_mac>
```

---

## Protocol Stack

### Layer Diagram

```
┌────────────────────────────────────────────────────────┐
│              Application Layer                          │
│   V2G Messages (SessionSetup, ChargeParameter, etc.)   │
├────────────────────────────────────────────────────────┤
│              Presentation Layer                         │
│   EXI Encoding (DIN 70121 / ISO 15118-2 schema)        │
├────────────────────────────────────────────────────────┤
│              Session Layer                              │
│   V2GTP (Vehicle-to-Grid Transport Protocol)           │
│   Version 0x01, PayloadType 0x8001                     │
├────────────────────────────────────────────────────────┤
│              Transport Layer                            │
│   TCP (port 15118)                                     │
├────────────────────────────────────────────────────────┤
│              Network Layer                              │
│   IPv6 (link-local, SDP for discovery)                 │
├────────────────────────────────────────────────────────┤
│              Data Link Layer                            │
│   HomePlug Green PHY (SLAC for pairing)                │
│   EtherType 0x88E1 (MME messages)                      │
├────────────────────────────────────────────────────────┤
│              Physical Layer                             │
│   OFDM over Control Pilot (CP) line                    │
│   1-30 MHz, ~10 Mbps effective                         │
└────────────────────────────────────────────────────────┘
```

### Key Protocols

| Protocol | Standard | Purpose |
|----------|----------|---------|
| **SLAC** | ISO 15118-3 | Signal Level Attenuation Characterization - network pairing |
| **HomePlug GP** | HomePlug Green PHY | Powerline communication physical layer |
| **SDP** | ISO 15118-2 | SECC Discovery Protocol - find charger IP |
| **V2GTP** | ISO 15118-2 | Transport framing for EXI messages |
| **DIN 70121** | DIN SPEC 70121 | DC charging protocol (simpler than ISO 15118-2) |
| **ISO 15118-2** | ISO 15118-2 | Full V2G with Plug&Charge, TLS, etc. |

---

## EVSE Mode (Implemented)

### SLAC State Machine (EVSE as Responder)

```
┌─────────────────────────────────────────────────────────────────────────┐
│                     EVSE SLAC State Machine                              │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│   ┌─────────┐                                                            │
│   │  IDLE   │ ← plc.start_evse()                                        │
│   └────┬────┘                                                            │
│        │ CP PWM starts (5% = DC charging)                                │
│        ▼                                                                 │
│   ┌─────────────────┐                                                    │
│   │ WAIT_PARAM_REQ  │ ←─────────────────────────────────────┐           │
│   └────────┬────────┘                                       │           │
│            │ ← CM_SLAC_PARAM.REQ from car                   │ timeout   │
│            │ → CM_SLAC_PARAM.CNF to car                     │ (retry)   │
│            ▼                                                 │           │
│   ┌─────────────────┐                                        │           │
│   │ WAIT_ATTEN_CHAR │                                        │           │
│   └────────┬────────┘                                        │           │
│            │ ← CM_MNBC_SOUND.IND × 10 (attenuation sounds)   │           │
│            │   or timeout after 2s with partial sounds       │           │
│            │ → CM_ATTEN_CHAR.IND to car                      │           │
│            ▼                                                 │           │
│   ┌─────────────────┐                                        │           │
│   │ WAIT_MATCH_REQ  │────────────────────────────────────────┘           │
│   └────────┬────────┘                                                    │
│            │ ← CM_SLAC_MATCH.REQ from car                                │
│            │ → CM_SLAC_MATCH.CNF to car (with NID/NMK)                   │
│            │ → CM_SET_KEY.REQ to modem (configure network)               │
│            ▼                                                             │
│   ┌─────────┐                                                            │
│   │ MATCHED │ → Python callback(car_mac)                                │
│   └─────────┘   IP network now active, ready for TCP                    │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### MME Messages Implemented

| Message | Direction | Purpose |
|---------|-----------|---------|
| `CM_SLAC_PARAM.REQ` | Car → EVSE | Initiate SLAC, contains RunID |
| `CM_SLAC_PARAM.CNF` | EVSE → Car | Accept SLAC, specify sound count |
| `CM_MNBC_SOUND.IND` | Car → EVSE | Attenuation measurement sounds |
| `CM_ATTEN_CHAR.IND` | EVSE → Car | Report attenuation profile |
| `CM_ATTEN_CHAR.RSP` | Car → EVSE | Acknowledge attenuation |
| `CM_SLAC_MATCH.REQ` | Car → EVSE | Request network join |
| `CM_SLAC_MATCH.CNF` | EVSE → Car | Provide NID/NMK |
| `CM_SET_KEY.REQ` | EVSE → Modem | Configure modem with NID/NMK |
| `CM_GET_SW.REQ/CNF` | EVSE ↔ Modem | Query modem firmware version |

### V2G State Machine (EVSE Responses)

```
┌─────────────────────────────────────────────────────────────────────────┐
│                     V2G Protocol Flow (EVSE Mode)                        │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│   After SLAC MATCHED, car connects to EVSE on TCP 15118                 │
│                                                                          │
│   1. SupportedAppProtocolReq → Res (select DIN 70121)                   │
│   2. SessionSetupReq → Res (establish session ID)                        │
│   3. ServiceDiscoveryReq → Res (offer DC charging service)               │
│   4. ServicePaymentSelectionReq → Res (external payment)                 │
│   5. ContractAuthenticationReq → Res (no auth needed)                    │
│   6. ChargeParameterDiscoveryReq → Res (EVSE max V/I/P)                  │
│                                                                          │
│   7. CableCheckReq → Res (isolation test - fake OK)                      │
│      └── May loop until Finished=true                                    │
│                                                                          │
│   8. PreChargeReq → Res (voltage ramp)                                   │
│      └── Loop: Simulate rising EVSEPresentVoltage toward                 │
│          EVTargetVoltage (e.g., 20V increments)                          │
│      └── When voltage matches (±20V), car proceeds                       │
│                                                                          │
│   9. PowerDeliveryReq(Start) → Res                                       │
│      ╔════════════════════════════════════════════════════════╗          │
│      ║  🎉 CONTACTORS CLOSE HERE!                              ║          │
│      ║  Battery HV now present on CCS pins                     ║          │
│      ╚════════════════════════════════════════════════════════╝          │
│                                                                          │
│  10. CurrentDemandReq → Res (active charging - loop)                     │
│      └── EVSE reports present current matching target                    │
│                                                                          │
│  11. PowerDeliveryReq(Stop) → Res (end charging)                         │
│  12. WeldingDetectionReq → Res (contactor weld check)                    │
│  13. SessionStopReq → Res                                                │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### Python API (EVSE Mode)

```python
import plc
import os

# Configure NID/NMK
nid = os.urandom(7)
nmk = os.urandom(16)
plc.set_key(nid, nmk)

# Set SLAC completion callback
def on_slac_done(car_mac):
    print(f"SLAC complete: {car_mac}")
    # Start V2G TCP server here
    
plc.set_callback(on_slac_done)

# Start CP PWM (via machine.PWM)
from machine import PWM, Pin
cp = PWM(Pin(4), freq=1000, duty_u16=int(65535 * 0.05))

# Start SLAC responder
plc.start_evse()

# Check status
print(plc.get_status())
# {'enabled': True, 'state': 'WAIT_PARAM_REQ', 'car_mac': None, ...}

# EXI encode/decode
msg = plc.exi_decode(raw_tcp_data)
response = plc.exi_encode('PreChargeRes', {'EVSEPresentVoltage': 3950})
```

---

## PEV Mode (Planned)

### Overview

PEV (Plug-in Electric Vehicle) mode allows the ESP32-P4 to act as a car, initiating SLAC and connecting to real chargers. This is useful for:
- Testing EVSE implementations
- V2G energy export (vehicle-to-grid)
- Charger compatibility research

### SLAC State Machine (PEV as Initiator)

```
┌─────────────────────────────────────────────────────────────────────────┐
│                     PEV SLAC State Machine                               │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│   ┌─────────┐                                                            │
│   │  IDLE   │ ← plc.start_pev()                                         │
│   └────┬────┘                                                            │
│        │ Wait for CP PWM detection (charger starts 5% PWM)               │
│        ▼                                                                 │
│   ┌─────────────────┐                                                    │
│   │ SEND_PARAM_REQ  │                                                    │
│   └────────┬────────┘                                                    │
│            │ → CM_SLAC_PARAM.REQ to broadcast                            │
│            │ ← CM_SLAC_PARAM.CNF from charger                            │
│            ▼                                                             │
│   ┌─────────────────┐                                                    │
│   │ SEND_SOUNDS     │                                                    │
│   └────────┬────────┘                                                    │
│            │ → CM_MNBC_SOUND.IND × 10 (broadcast)                        │
│            │ ← CM_ATTEN_CHAR.IND from charger                            │
│            │ → CM_ATTEN_CHAR.RSP to charger                              │
│            ▼                                                             │
│   ┌─────────────────┐                                                    │
│   │ SEND_MATCH_REQ  │                                                    │
│   └────────┬────────┘                                                    │
│            │ → CM_SLAC_MATCH.REQ to charger                              │
│            │ ← CM_SLAC_MATCH.CNF (contains NID/NMK)                      │
│            │ → CM_SET_KEY.REQ to own modem                               │
│            ▼                                                             │
│   ┌─────────┐                                                            │
│   │ MATCHED │ → callback(charger_mac)                                   │
│   └─────────┘   → Use SDP to find charger IP                            │
│                 → Connect TCP to charger:15118                           │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### Implementation Requirements (PEV Mode)

| Component | Status | Work Required |
|-----------|--------|---------------|
| SLAC Initiator state machine | ❌ Not started | New state machine in modplc.c |
| CM_SLAC_PARAM.REQ composition | ❌ Not started | Generate RunID, send to broadcast |
| CM_MNBC_SOUND.IND generation | ❌ Not started | Generate 10 sounds at correct timing |
| CM_ATTEN_CHAR.RSP handling | ❌ Not started | Acknowledge charger's attenuation report |
| CM_SLAC_MATCH.REQ composition | ❌ Not started | Request network join |
| CM_SLAC_MATCH.CNF parsing | ❌ Not started | Extract NID/NMK from charger |
| SDP Client | ❌ Not started | UDP multicast to find charger IP |
| V2G Request encoding | ⚠️ Partial | Need to add request message templates |
| V2G Response decoding | ⚠️ Partial | Need to parse response messages |
| Modem PIB switching | ❌ Not started | Switch modem to PEV mode |

### Proposed Python API (PEV Mode)

```python
import plc

# Set callback for SLAC completion
def on_charger_found(charger_mac, charger_ip):
    print(f"Charger: {charger_mac} at {charger_ip}")
    # Connect TCP and start V2G as PEV

plc.set_callback(on_charger_found)

# Start PEV mode (initiator)
plc.start_pev()

# Status will show PEV states:
# SEND_PARAM_REQ, WAIT_PARAM_CNF, SEND_SOUNDS, WAIT_ATTEN, 
# SEND_MATCH, WAIT_MATCH_CNF, MATCHED

# EXI encode requests
req = plc.exi_encode('SessionSetupReq', {'EVCCID': my_mac})
req = plc.exi_encode('ChargeParameterDiscoveryReq', {
    'EVMaxVoltage': 4000,
    'EVMaxCurrent': 2000,
})
```

### EXI Messages Required for PEV Mode

| Message | Direction | Current Status |
|---------|-----------|----------------|
| `SupportedAppProtocolReq` | PEV → EVSE | ❌ Need encoder |
| `SessionSetupReq` | PEV → EVSE | ❌ Need encoder |
| `ServiceDiscoveryReq` | PEV → EVSE | ❌ Need encoder |
| `ServicePaymentSelectionReq` | PEV → EVSE | ❌ Need encoder |
| `ContractAuthenticationReq` | PEV → EVSE | ❌ Need encoder |
| `ChargeParameterDiscoveryReq` | PEV → EVSE | ❌ Need encoder |
| `CableCheckReq` | PEV → EVSE | ❌ Need encoder |
| `PreChargeReq` | PEV → EVSE | ❌ Need encoder |
| `PowerDeliveryReq` | PEV → EVSE | ❌ Need encoder |
| `CurrentDemandReq` | PEV → EVSE | ❌ Need encoder |
| All *Res messages | EVSE → PEV | ❌ Need decoder |

---

## Listen/Sniff Mode (Planned)

### Overview

Listen mode passively monitors CCS communication between a real car and charger without participating. This requires:
- Coupling to the CP line without interfering
- Capturing both directions of communication
- Decoding all messages for analysis

### Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                  Listen Mode Architecture                                │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│   Real EVSE ◄────────────────────────────────────► Real EV              │
│        │                CP Line                      │                   │
│        │                  │                          │                   │
│        │                  │ (passive coupling)       │                   │
│        │                  ▼                          │                   │
│        │        ┌─────────────────┐                  │                   │
│        │        │ Coupling Xfmr   │                  │                   │
│        │        │ (high-Z tap)    │                  │                   │
│        │        └────────┬────────┘                  │                   │
│        │                 │                           │                   │
│        │                 ▼                           │                   │
│        │        ┌─────────────────┐                  │                   │
│        │        │ TP-Link Modem   │                  │                   │
│        │        │ (Sniffer Mode)  │                  │                   │
│        │        └────────┬────────┘                  │                   │
│        │                 │ Ethernet                  │                   │
│        │                 ▼                           │                   │
│        │        ┌─────────────────┐                  │                   │
│        │        │    ESP32-P4     │                  │                   │
│        │        │ • Capture frames│                  │                   │
│        │        │ • Decode MME    │                  │                   │
│        │        │ • Decode EXI    │                  │                   │
│        │        │ • Log/stream    │                  │                   │
│        │        └─────────────────┘                  │                   │
│        │                                             │                   │
└────────┴─────────────────────────────────────────────┴───────────────────┘
```

### Challenges

1. **NID/NMK Recovery**: The modem needs the network key to decode encrypted traffic
   - Must capture SLAC_MATCH.CNF which contains the NMK in plaintext
   - Or use a known test NMK for controlled environments

2. **Timing**: Must process frames fast enough to not miss any

3. **Hardware**: May need a second modem for truly passive sniffing

### Implementation Requirements (Listen Mode)

| Component | Status | Work Required |
|-----------|--------|---------------|
| Passive frame capture | ❌ Not started | Receive all 0x88E1 frames |
| SLAC message decoding | ⚠️ Partial | Extend decode for all MME types |
| NMK extraction | ❌ Not started | Parse SLAC_MATCH frames |
| Dynamic key update | ❌ Not started | Configure modem with captured NMK |
| V2G frame capture | ❌ Not started | Capture TCP/IPv6 traffic |
| Real-time streaming | ❌ Not started | Stream decoded messages to client |
| PCAP export | ❌ Not started | Save captures for Wireshark |

### Proposed Python API (Listen Mode)

```python
import plc

# Frame callback
def on_frame(frame_type, direction, data, decoded):
    print(f"[{direction}] {frame_type}: {decoded}")
    # direction = 'PEV→EVSE' or 'EVSE→PEV'

plc.set_frame_callback(on_frame)

# Start listen mode
plc.start_listen()

# Status
status = plc.get_status()
# {'mode': 'LISTEN', 'frames_captured': 1234, 'nmk_known': True, ...}

# Export capture
plc.export_pcap('/sd/capture.pcap')

# Stop
plc.stop()
```

---

## EXI Codec Design

### Approach: Pattern Matching + Templates

Full EXI implementation would require:
- Schema-aware bit-level parsing
- Complex grammar rules
- Significant code size

Our approach instead:
1. **Decode**: Pattern match on known byte sequences
2. **Encode**: Use pre-built templates from pyPLC test vectors

### Message Detection Algorithm

```c
din_msg_type_t exi_detect_message_type(const uint8_t *exi, int len) {
    // Handshake messages start with 0x80 0x00 or 0x80 0x40
    if (exi[0] == 0x80 && exi[1] == 0x00) {
        if (exi[2] == 0xdb || exi[2] == 0xeb) {
            return DIN_MSG_SUPPORTED_APP_PROTOCOL_REQ;
        }
    }
    
    // DIN messages start with 0x80 0x9a
    if (exi[0] == 0x80 && exi[1] == 0x9a) {
        // Pattern match on bytes 2-4 for message type
        // Each message has a distinct pattern
    }
}
```

### Template-Based Encoding

Each response is a complete EXI-encoded message from pyPLC:

```c
// PreChargeRes template from pyPLC
static const uint8_t TPL_PRE_CHARGE_RES[] = {
    0x80, 0x9a, 0x00, 0x11, 0x60, 0x02, 0x00, 0x00, 
    0x00, 0x32, 0x00, 0x00
};

// Voltage is at a known offset - can be modified
```

### V2GTP Header

All V2G messages are wrapped with an 8-byte header:

```
Byte 0: 0x01 (version)
Byte 1: 0xFE (version inverted)
Byte 2-3: 0x80 0x01 (payload type = EXI)
Byte 4-7: payload length (big-endian)
Byte 8+: EXI data
```

---

## V2G Message Flow

### Complete EVSE Session

```
┌─────────┐                                           ┌─────────┐
│   PEV   │                                           │  EVSE   │
└────┬────┘                                           └────┬────┘
     │                                                     │
     │ ──────────── SLAC (HomePlug MME) ─────────────────► │
     │ ◄───────────────── SLAC ────────────────────────── │
     │                                                     │
     │ ══════════ Network Formed (IPv6 Link Local) ═══════ │
     │                                                     │
     │ ─── TCP Connect to [fe80::...]:15118 ─────────────► │
     │                                                     │
     │ ─── SupportedAppProtocolReq ──────────────────────► │
     │ ◄── SupportedAppProtocolRes ─────────────────────── │
     │                                                     │
     │ ─── SessionSetupReq ──────────────────────────────► │
     │ ◄── SessionSetupRes ───────────────────────────── │
     │                                                     │
     │ ─── ServiceDiscoveryReq ──────────────────────────► │
     │ ◄── ServiceDiscoveryRes ─────────────────────────── │
     │                                                     │
     │ ─── ServicePaymentSelectionReq ───────────────────► │
     │ ◄── ServicePaymentSelectionRes ──────────────────── │
     │                                                     │
     │ ─── ContractAuthenticationReq ────────────────────► │
     │ ◄── ContractAuthenticationRes ───────────────────── │
     │                                                     │
     │ ─── ChargeParameterDiscoveryReq ──────────────────► │
     │ ◄── ChargeParameterDiscoveryRes ─────────────────── │
     │                                                     │
     │ ─── CableCheckReq ────────────────────────────────► │
     │ ◄── CableCheckRes (Ongoing) ┐                       │
     │ ─── CableCheckReq ──────────┤ (loop until finished) │
     │ ◄── CableCheckRes (Finished)┘                       │
     │                                                     │
     │ ─── PreChargeReq (EVTargetVoltage=400V) ──────────► │
     │ ◄── PreChargeRes (EVSEPresentVoltage=50V) ──────── │
     │ ─── PreChargeReq ─────────────────────────────────► │
     │ ◄── PreChargeRes (EVSEPresentVoltage=200V) ─────── │
     │ ─── PreChargeReq ─────────────────────────────────► │
     │ ◄── PreChargeRes (EVSEPresentVoltage=395V) ─────── │
     │                                                     │
     │ ─── PowerDeliveryReq (ChargeProgress=Start) ──────► │
     │ ◄── PowerDeliveryRes ───────────────────────────── │
     │    ╔═══════════════════════════════════════════╗    │
     │    ║  🔌 CONTACTORS CLOSE - HV NOW ACTIVE      ║    │
     │    ╚═══════════════════════════════════════════╝    │
     │                                                     │
     │ ─── CurrentDemandReq ┐                              │
     │ ◄── CurrentDemandRes ├ (loop during charging)       │
     │ ─── CurrentDemandReq ┘                              │
     │                                                     │
     │ ─── PowerDeliveryReq (ChargeProgress=Stop) ───────► │
     │ ◄── PowerDeliveryRes ───────────────────────────── │
     │    (Contactors open)                                │
     │                                                     │
     │ ─── WeldingDetectionReq ──────────────────────────► │
     │ ◄── WeldingDetectionRes ─────────────────────────── │
     │                                                     │
     │ ─── SessionStopReq ───────────────────────────────► │
     │ ◄── SessionStopRes ──────────────────────────────── │
     │                                                     │
     │ ──── TCP Close ───────────────────────────────────► │
└────┴────┘                                           └────┴────┘
```

---

## Implementation Status

### Summary Table

| Component | EVSE Mode | PEV Mode | Listen Mode |
|-----------|-----------|----------|-------------|
| **SLAC Layer** | | | |
| L2TAP Ethernet | ✅ Done | ✅ Done | ✅ Done |
| MME Frame handling | ✅ Done | ⚠️ Partial | ⚠️ Partial |
| SLAC State Machine | ✅ Done | ❌ TODO | N/A |
| Modem SET_KEY | ✅ Done | ⚠️ Reuse | ⚠️ Reuse |
| Python Callback | ✅ Done | ⚠️ Reuse | ❌ TODO |
| **EXI Layer** | | | |
| V2GTP Header | ✅ Done | ✅ Done | ✅ Done |
| Message Detection | ✅ Done | ✅ Done | ✅ Done |
| Request Decoding | ✅ Done (EVSE) | ❌ TODO (Res) | ✅ Reuse |
| Response Encoding | ✅ Done (EVSE) | ❌ TODO (Req) | N/A |
| **V2G Layer** | | | |
| TCP Server | ⚠️ Python | N/A | ⚠️ Capture |
| TCP Client | N/A | ❌ Python | N/A |
| SDP Server | ❌ Not needed | N/A | N/A |
| SDP Client | N/A | ❌ TODO | N/A |
| **Application** | | | |
| Scripto Extension | ✅ Done | ❌ TODO | ❌ TODO |
| Device Helpers | ✅ Done | ❌ TODO | ❌ TODO |

### Estimated Effort

| Mode | Effort | Priority |
|------|--------|----------|
| EVSE Mode (current) | ✅ Complete | HIGH |
| PEV Mode | ~2 weeks | MEDIUM |
| Listen Mode | ~1 week | LOW |

---

## Future Enhancements

### Short-term

1. **Complete V2G TCP server** in Python
2. **Test with real Tesla Model Y**
3. **Add ISO 15118-2 support** (schema B)
4. **TLS support** for ISO 15118-2

### Medium-term

1. **PEV mode** implementation
2. **Listen mode** with Wireshark export
3. **CAN bus integration** for vehicle data
4. **Web dashboard** for monitoring

### Long-term

1. **Plug & Charge** (ISO 15118-2 with certificates)
2. **Bidirectional charging** (V2G/V2H)
3. **Smart grid integration**
4. **Multi-vehicle support**

---

## References

### Standards

- **ISO 15118-1**: General information and use-case definition
- **ISO 15118-2**: Network and application protocol requirements
- **ISO 15118-3**: Physical and data link layer requirements (HomePlug GP)
- **DIN SPEC 70121**: DC charging requirements (subset of ISO 15118)
- **HomePlug Green PHY**: Powerline communication specification

### Open Source Projects

- **[pyPLC](https://github.com/uhi22/pyPLC)**: Python reference implementation (our main reference)
- **[OpenV2G](https://github.com/Martin-P/OpenV2G)**: C EXI codec
- **[open-plc-utils](https://github.com/qca/open-plc-utils)**: HomePlug modem tools
- **[EVerest](https://github.com/EVerest)**: Full charging stack

### Hardware

- **ESP32-P4**: [Espressif Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/)
- **TP-Link TL-PA4010P**: Qualcomm QCA7420 chipset, widely available

---

## Appendix: Test Vectors

### SLAC Messages (from pyPLC)

```
# CM_SLAC_PARAM.REQ (Ioniq)
01fe 8001 0000001e  # V2GTP header (if present)
# Actual MME frame starts here

# CM_SLAC_PARAM.CNF
# ... (see pyPLC for examples)
```

### EXI Messages (from pyPLC)

```
# SupportedAppProtocolReq (Ioniq)
8000dbab9371d3234b71d1b981899189d191818991d26b9b3a232b30020000040040

# SupportedAppProtocolRes
80400040

# SessionSetupReq
809a0011d00000

# SessionSetupRes
809a02004080c1014181c211e0000080

# PreChargeReq
809a001150400000c80006400000

# PreChargeRes
809a00116002000000320000
```

---

*Document generated during implementation session. See git history for updates.*
