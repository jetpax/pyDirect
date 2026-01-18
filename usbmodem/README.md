# USB Modem Module

USB Host mode modem support for ESP32, enabling cellular connectivity via USB modems (LTE/4G/5G).

## Overview

The USB Modem module provides USB Host functionality for cellular modems, allowing ESP32 devices to connect to the internet via LTE/4G/5G networks. It supports standard USB CDC-ACM modems and implements PPP protocol for IP connectivity.

## Features

- **USB Host Mode** - ESP32 acts as USB host for modem
- **PPP Protocol** - Standard Point-to-Point Protocol implementation
- **AT Commands** - Full AT command interface for modem control
- **Dual Interface** - Separate AT and PPP channels (SIM7600 compatible)
- **Network Integration** - Seamless integration with MicroPython network stack
- **IPv4/IPv6** - Dual-stack support

## Supported Modems

Tested and verified:
- **SIM7600** series (SIM7600G, SIM7600E, SIM7600A)
- **SIM7500** series
- **Quectel EC25/EC21**
- **Generic CDC-ACM modems** with PPP support

## Hardware Requirements

- **ESP32-S3** or **ESP32-P4** (USB Host capable)
- **USB Modem** (LTE/4G/5G with CDC-ACM interface)
- **SIM Card** with active data plan
- **USB Cable** (device to modem)

## Python API

```python
import usbmodem

# Initialize modem
modem = usbmodem.Modem()

# Configure APN (required for cellular data)
modem.set_apn("internet")  # Replace with your carrier's APN

# Connect to network
modem.connect()

# Check connection status
if modem.is_connected():
    print(f"Connected! IP: {modem.get_ip()}")

# Send AT command
response = modem.at("AT+CSQ")  # Check signal quality
print(f"Signal: {response}")

# Disconnect
modem.disconnect()
```

## Usage Example

```python
import usbmodem
import time

# Initialize modem
modem = usbmodem.Modem()

# Configure for your carrier
modem.set_apn("internet")  # e.g., "internet", "hologram", "iot.1nce.net"

# Optional: Set PIN if SIM requires it
# modem.set_pin("1234")

# Connect to cellular network
print("Connecting to cellular network...")
modem.connect()

# Wait for connection
while not modem.is_connected():
    print("Waiting for connection...")
    time.sleep(1)

print(f"Connected! IP: {modem.get_ip()}")

# Now use network as normal
import urequests
response = urequests.get("http://httpbin.org/ip")
print(response.text)
```

## Configuration

### Board Configuration

USB modem requires specific board configuration in `sdkconfig.board`:

```cmake
# USB Host configuration
CONFIG_USB_OTG_SUPPORTED=y
CONFIG_IN_TRANSFER_BUFFER_SIZE=1024
CONFIG_OUT_TRANSFER_BUFFER_SIZE=1024

# Modem dual interface (SIM7600)
CONFIG_MODEM_SUPPORT_SECONDARY_AT_PORT=y
CONFIG_MODEM_USB_ITF=0x03      # Primary (PPP data)
CONFIG_MODEM_USB_ITF2=0x02     # Secondary (AT commands)
```

### APN Settings

Common carrier APNs:
- **AT&T**: `broadband`
- **T-Mobile**: `fast.t-mobile.com`
- **Verizon**: `vzwinternet`
- **Hologram**: `hologram`
- **1NCE**: `iot.1nce.net`

Check with your carrier for the correct APN.

## Build Configuration

Enable in `build.sh`:
```bash
./build.sh BOARD usbmodem
```

Or in CMake:
```cmake
-DMODULE_PYDIRECT_USBMODEM=ON
```

## AT Command Interface

The module provides direct AT command access:

```python
# Check signal quality
signal = modem.at("AT+CSQ")
print(f"Signal: {signal}")

# Get IMEI
imei = modem.at("AT+CGSN")
print(f"IMEI: {imei}")

# Get SIM status
sim = modem.at("AT+CPIN?")
print(f"SIM: {sim}")

# Get network registration
reg = modem.at("AT+CREG?")
print(f"Registration: {reg}")
```

## Network Integration

Once connected, the modem appears as a standard network interface:

```python
import network

# Get modem interface
modem_if = network.PPP(network.STA_IF)

# Check status
print(modem_if.ifconfig())  # (ip, netmask, gateway, dns)
```

## Troubleshooting

**Modem not detected:**
- Check USB cable connection
- Verify ESP32 has USB Host support (S3/P4 only)
- Check `dmesg` output for USB enumeration

**Cannot connect:**
- Verify APN is correct for your carrier
- Check SIM card is inserted and activated
- Ensure data plan is active
- Check signal strength with `AT+CSQ`

**Slow connection:**
- Check signal quality (RSSI)
- Verify modem supports your carrier's bands
- Check for network congestion

**Connection drops:**
- Check power supply (modems draw significant current)
- Verify antenna connection
- Check carrier coverage in your area

## Power Considerations

USB modems can draw 500mA+ during transmission. Ensure:
- Adequate power supply (2A+ recommended)
- Proper USB power delivery
- Decoupling capacitors on power rails

## See Also

- [SIM7600 AT Command Manual](https://www.simcom.com/product/SIM7600X.html)
- [PPP Protocol RFC 1661](https://tools.ietf.org/html/rfc1661)
- [ESP32 USB Host Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/usb_host.html)
