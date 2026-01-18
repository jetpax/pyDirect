# Husarnet Module

Husarnet P2P VPN integration for ESP32, enabling secure peer-to-peer networking without port forwarding or complex network configuration.

## Overview

The Husarnet module provides seamless integration with [Husarnet](https://husarnet.com/), a global P2P VPN platform. It allows ESP32 devices to communicate securely over the internet using IPv6, bypassing NAT and firewalls.

## Features

- **Zero-Config P2P** - Automatic peer discovery and connection
- **IPv6 Native** - Direct device-to-device communication
- **NAT Traversal** - Works behind firewalls and NAT
- **Low Latency** - Direct P2P connections when possible
- **Secure** - End-to-end encrypted communication
- **Global Reach** - Connect devices anywhere in the world

## Dependencies

- **Network** - Requires WiFi or Ethernet connectivity
- **Husarnet Account** - Free account at [app.husarnet.com](https://app.husarnet.com/)

## Python API

```python
import husarnet

# Join Husarnet network with join code
husarnet.join("fc94:xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:xxxx/xxxxxxxxxxxxxxxxxxxxxxxxx")

# Get device's Husarnet IPv6 address
ipv6 = husarnet.get_ipv6()
print(f"Husarnet IPv6: {ipv6}")

# Get connection status
status = husarnet.status()
print(f"Connected: {status['connected']}")

# Leave Husarnet network
husarnet.leave()
```

## Setup Guide

### 1. Create Husarnet Account

Visit [app.husarnet.com](https://app.husarnet.com/) and create a free account.

### 2. Create Network

1. Click "Create Network"
2. Name your network (e.g., "IoT Devices")
3. Copy the join code

### 3. Join Device to Network

```python
import husarnet

# Paste your join code from Husarnet dashboard
husarnet.join("fc94:xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:xxxx/xxxxxxxxxxxxxxxxxxxxxxxxx")
```

### 4. Verify Connection

Check the Husarnet dashboard - your device should appear in the network list with its IPv6 address.

## Usage Examples

### Basic P2P Communication

```python
import husarnet
import socket

# Join network
husarnet.join("fc94:...")

# Create TCP server on Husarnet IPv6
server = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
server.bind((husarnet.get_ipv6(), 8080))
server.listen(1)

print(f"Server listening on [{husarnet.get_ipv6()}]:8080")
```

### Device Discovery

```python
import husarnet

# Get list of peers in network
peers = husarnet.get_peers()
for peer in peers:
    print(f"Peer: {peer['name']} - {peer['ipv6']}")
```

## Configuration

The module uses the Husarnet managed component from ESP-IDF Component Registry:

```yaml
# webrtc/idf_component.yml
dependencies:
  husarnet/esp_husarnet: "^0.0.15"
```

## Build Configuration

Enable in `build.sh`:
```bash
./build.sh BOARD husarnet
```

Or in CMake:
```cmake
-DMODULE_PYDIRECT_HUSARNET=ON
```

## Network Architecture

```
Device A (ESP32) ←→ Husarnet Cloud ←→ Device B (PC/Phone/ESP32)
     ↓                                        ↓
  IPv6: fc94::1                          IPv6: fc94::2
```

- **Direct P2P** when both devices are on same LAN
- **Relay via Husarnet** when NAT traversal needed
- **Automatic fallback** between direct and relayed modes

## Security

- **End-to-End Encryption** - All traffic encrypted
- **Device Authentication** - Join codes required
- **Network Isolation** - Devices only see peers in same network

## Limitations

- Requires active internet connection for initial setup
- IPv6 only (no IPv4 support)
- Free tier has bandwidth limits (check Husarnet pricing)

## Troubleshooting

**Device not appearing in dashboard:**
- Check WiFi/Ethernet connection
- Verify join code is correct
- Wait 30-60 seconds for registration

**Cannot connect to peer:**
- Ensure both devices are in same Husarnet network
- Check firewall rules on peer device
- Verify IPv6 address is correct

## See Also

- [Husarnet Documentation](https://husarnet.com/docs/)
- [Husarnet Dashboard](https://app.husarnet.com/)
- [ESP-IDF Husarnet Component](https://components.espressif.com/components/husarnet/esp_husarnet)
