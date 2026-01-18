# PLC Module - CCS/NACS Charging Communication

MicroPython C module for CCS DC fast charging communication.

## Overview

This module enables an ESP32 to act as a **CCS/NACS charger (EVSE)** for DC fast charging. It communicates with electric vehicles using the HomePlug Green Phy powerline protocol over the Control Pilot (CP) line.

## Hardware Requirements

- **ESP32** with Ethernet (e.g., Waveshare ESP32-P4-Module)
- **TP-Link TL-PA4010P** HomePlug modem (configured for EVSE mode)
- **CCS/NACS connector** with CP, PP, PE connections
- **CP coupling circuit** (PWM + PLC transformer)
- **PP resistor** (e.g., 1.5kΩ for 13A rating)

## Features

### SLAC Phase (Complete)
- [x] L2TAP raw Ethernet for HomePlug EtherType 0x88E1
- [x] SLAC responder state machine (EVSE mode)
- [x] CM_SLAC_PARAM.CNF response
- [x] CM_ATTEN_CHAR.IND generation
- [x] CM_SLAC_MATCH.CNF with NID/NMK
- [x] CM_SET_KEY to configure modem
- [x] Python callback on SLAC completion

### V2G Phase (Complete)
- [x] DIN 70121 EXI codec (pattern-matching decode, template encode)
- [x] V2GTP header handling
- [x] Message type detection
- [x] EVSE response encoding for all DIN messages
- [ ] V2G TCP server integration (Python-side)
- [ ] PreCharge voltage simulation (Python-side)

## API

```python
import plc

# Check modem present
info = plc.get_modem_info()
print(f"Modem: {info}")

# Set NID/NMK (generate random or use fixed for testing)
import os
nid = os.urandom(7)
nmk = os.urandom(16)
plc.set_key(nid, nmk)

# Set callback for SLAC completion
def on_slac_complete(car_mac):
    print(f"SLAC complete! Car MAC: {car_mac}")
    # Start V2G server here

plc.set_callback(on_slac_complete)

# Start EVSE mode
plc.start_evse()

# Check status
status = plc.get_status()
print(f"State: {status['state']}")

# Stop
plc.stop()
```

### EXI Codec API

```python
import plc

# Decode incoming V2G message (with or without V2GTP header)
raw_data = b'\x01\xfe\x80\x01...'  # From TCP socket
msg = plc.exi_decode(raw_data)
print(f"Message type: {msg['type']}")

# Decode returns dict with message-specific fields:
# - PreChargeReq: EVTargetVoltage, EVTargetCurrent
# - CurrentDemandReq: EVTargetVoltage, EVTargetCurrent, ChargingComplete
# - PowerDeliveryReq: ChargeProgress ('Start' or 'Stop')

# Encode EVSE response (returns bytes with V2GTP header)
response = plc.exi_encode('SupportedAppProtocolRes', None)
socket.send(response)

# Encode with parameters
response = plc.exi_encode('PreChargeRes', {
    'EVSEPresentVoltage': 3950,  # 395.0V in 0.1V units
    'EVSEPresentCurrent': 0,
})

response = plc.exi_encode('CableCheckRes', {
    'Finished': True,  # or False for 'processing'
})

# Supported message types:
# - SupportedAppProtocolRes
# - SessionSetupRes
# - ServiceDiscoveryRes
# - ServicePaymentSelectionRes
# - ContractAuthenticationRes
# - ChargeParameterDiscoveryRes
# - CableCheckRes
# - PreChargeRes
# - PowerDeliveryRes
# - CurrentDemandRes
# - WeldingDetectionRes
# - SessionStopRes
```

## SLAC State Machine

```
IDLE
  ↓ (receive SLAC_PARAM.REQ)
WAIT_PARAM_REQ → send SLAC_PARAM.CNF
  ↓
WAIT_ATTEN_CHAR ← receive MNBC_SOUND.IND (10x)
  ↓ (after sounds or timeout)
  → send ATTEN_CHAR.IND
  ↓
WAIT_MATCH_REQ ← receive SLAC_MATCH.REQ
  ↓
  → send SLAC_MATCH.CNF (with NID/NMK)
  → send SET_KEY to modem
  ↓
MATCHED → callback to Python
```

## Configuration

### TP-Link EVSE Mode PIB

The TP-Link modem must be configured as EVSE (coordinator):

```bash
# On Linux with open-plc-utils
cp original.pib evse.pib
setpib evse.pib 74 hfid "EVSE"
setpib evse.pib F4 byte 2
setpib evse.pib 1653 byte 2
setpib evse.pib 1C98 long 10240 long 102400
plctool -ieth0 -P evse.pib <modem_mac>
```

### CP PWM

Before starting SLAC, the Control Pilot must be set to 5% PWM (1kHz) to signal "digital communication required":

```python
from machine import PWM, Pin
CP_PIN = 4  # Your CP GPIO
cp = PWM(Pin(CP_PIN), freq=1000, duty_u16=int(65535 * 0.05))
```

## References

- [pyPLC](https://github.com/uhi22/pyPLC) - Original Python implementation
- [ISO 15118-3](https://www.iso.org/standard/59675.html) - HomePlug Green Phy for V2G
- [DIN 70121](https://www.beuth.de/en/standard/din-70121/152567898) - DC charging protocol
- [open-plc-utils](https://github.com/qca/open-plc-utils) - HomePlug modem configuration
