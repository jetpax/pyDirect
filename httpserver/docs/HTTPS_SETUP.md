# HTTPS/WSS Setup Guide

This guide explains how to enable HTTPS and WSS (WebSocket Secure) support in your MicroPython application.

## Overview

The httpserver module now supports dual HTTP/HTTPS operation:
- **HTTP** runs on port 80 (ws:// for WebSocket)
- **HTTPS** runs on port 443 (wss:// for WebSocket Secure)

Both servers run simultaneously when certificates are provided, allowing clients to connect via either protocol.

## Quick Start

### 1. Generate Self-Signed Certificates

On your development machine, generate a self-signed certificate and private key:

```bash
openssl req -newkey rsa:2048 -nodes -keyout prvtkey.pem -x509 -days 3650 \
  -out servercert.pem -subj "/CN=ESP32 Device"
```

This creates two files:
- `servercert.pem` - Server certificate (public)
- `prvtkey.pem` - Private key (keep secure)

**Note:** Self-signed certificates will show a security warning in browsers. For production, use certificates from a trusted Certificate Authority.

### 2. Upload Certificates to Device

Create a `/certs` directory on your device and upload the certificate files:

```python
import os

# Create directory if it doesn't exist
try:
    os.mkdir('/certs')
except:
    pass  # Directory already exists

# Upload servercert.pem and prvtkey.pem to /certs/
# Use your preferred file upload method (WebREPL, mpremote, etc.)
```

### 3. Enable HTTPS in main.py

Edit your `main.py` to enable HTTPS:

```python
# HTTPS Configuration
HTTPS_ENABLED = True  # Enable HTTPS
HTTPS_CERT_FILE = "/certs/servercert.pem"
HTTPS_KEY_FILE = "/certs/prvtkey.pem"
```

### 4. Start the Server

The server will automatically start both HTTP and HTTPS when you run `main.py`:

```python
# This will start both HTTP (port 80) and HTTPS (port 443)
httpserver.start(port=80, cert_file=HTTPS_CERT_FILE, key_file=HTTPS_KEY_FILE)
```

## Connection URLs

After enabling HTTPS, you can connect using either protocol:

### HTTP (Insecure)
- Web: `http://192.168.1.32/`
- WebSocket: `ws://192.168.1.32/webrepl`

### HTTPS (Secure)
- Web: `https://192.168.1.32/`
- WebSocket Secure: `wss://192.168.1.32/webrepl`

## Accepting Self-Signed Certificates

### In Web Browsers

1. Navigate to `https://192.168.1.32/`
2. Browser will show a security warning
3. Click "Advanced" or "Details"
4. Choose "Proceed" or "Accept Risk"
5. Certificate is now trusted for this session

### In Python Scripts

```python
import ssl
import websocket

# Disable certificate verification (development only)
ws = websocket.WebSocket(sslopt={"cert_reqs": ssl.CERT_NONE})
ws.connect("wss://192.168.1.32/webrepl")
```

### In Node.js/JavaScript

```javascript
const WebSocket = require('ws');

// Disable certificate verification (development only)
const ws = new WebSocket('wss://192.168.1.32/webrepl', {
  rejectUnauthorized: false
});
```

## Production Certificates

For production deployment, use certificates from a trusted Certificate Authority (CA):

### Option 1: Let's Encrypt (Free)

1. Get a domain name (e.g., `mydevice.example.com`)
2. Point DNS to your device's public IP
3. Use Certbot to obtain certificates
4. Upload to device

### Option 2: Commercial CA

1. Purchase certificate from CA (e.g., DigiCert, Sectigo)
2. Generate CSR on your computer
3. Submit CSR to CA
4. Receive signed certificate
5. Upload to device

### Option 3: Internal CA

For internal networks, set up your own Certificate Authority and distribute the CA certificate to all clients.

## Memory Considerations

HTTPS/TLS requires more memory than HTTP:

- **Certificate Storage**: ~1-2 KB per certificate
- **TLS Session**: ~40 KB per active connection
- **Stack Size**: HTTPS uses larger stack (10 KB vs 8 KB)

The default configuration limits HTTPS to 4 concurrent connections (vs 10 for HTTP) to manage memory usage.

## Troubleshooting

### "Failed to start HTTPS server"

**Possible causes:**
- Certificate files not found or unreadable
- Insufficient memory
- Port 443 already in use

**Solutions:**
- Verify certificate paths are correct
- Check files exist: `import os; os.stat('/certs/servercert.pem')`
- Free up memory: `import gc; gc.collect()`
- Restart device

### "Certificate not trusted" in browser

This is normal for self-signed certificates. You must manually accept the certificate in your browser's security warning dialog.

### Connection timeout on port 443

- Check firewall rules allow port 443
- Verify device is connected to network
- Try HTTP (port 80) first to confirm network connectivity

### Memory allocation failed

HTTPS uses significantly more memory than HTTP. To reduce memory usage:

```python
# Start HTTP only (no HTTPS)
HTTPS_ENABLED = False
httpserver.start(port=80)
```

Or increase available heap by disabling other features.

## API Reference

### httpserver.start()

```python
httpserver.start(port=80, cert_file=None, key_file=None)
```

**Parameters:**
- `port` (int): HTTP server port (default: 80)
- `cert_file` (str): Path to PEM certificate file (optional)
- `key_file` (str): Path to PEM private key file (optional)

**Returns:**
- `True` if server started successfully
- `False` on error

**Behavior:**
- If `cert_file` and `key_file` provided: Starts both HTTP (on `port`) and HTTPS (on 443)
- If certificates omitted: Starts HTTP only (on `port`)
- Both servers share the same URI handlers

### WebSocket Endpoints

WebSocket endpoints registered via `webrepl.start()` or `wsserver.start()` are automatically available on both HTTP and HTTPS servers:

```python
# WebSocket available on both protocols
webrepl.start(password="mypassword", path="/webrepl")

# Clients can connect via:
# ws://device-ip/webrepl   (HTTP)
# wss://device-ip/webrepl  (HTTPS)
```

## Security Best Practices

1. **Never commit private keys to version control**
   - Add `*.pem` to `.gitignore`
   - Store keys securely

2. **Use strong passwords**
   - For WebREPL and other authenticated endpoints
   - Change default passwords

3. **Rotate certificates regularly**
   - Set expiry dates (e.g., 1-2 years)
   - Monitor expiration

4. **Limit HTTPS to trusted networks**
   - Or use proper CA-signed certificates
   - Self-signed certs are vulnerable to MITM attacks

5. **Keep firmware updated**
   - Update ESP-IDF for security patches
   - Rebuild firmware regularly

## Example: Complete Setup

```python
# main.py - Production configuration

# WiFi Configuration
WIFI_SSID = "MyNetwork"
WIFI_PASSWORD = "SecurePassword123"

# HTTPS Configuration
HTTPS_ENABLED = True
HTTPS_CERT_FILE = "/certs/servercert.pem"
HTTPS_KEY_FILE = "/certs/prvtkey.pem"

# WebREPL Configuration
WEBREPL_PASSWORD = "SecureWebREPLPass456"

def main():
    # Connect to WiFi
    import network
    sta = network.WLAN(network.STA_IF)
    sta.active(True)
    sta.connect(WIFI_SSID, WIFI_PASSWORD)
    
    while not sta.isconnected():
        pass
    
    ip = sta.ifconfig()[0]
    print(f"Connected: {ip}")
    
    # Start HTTP/HTTPS servers
    from esp32 import httpserver, webrepl
    
    httpserver.start(
        port=80,
        cert_file=HTTPS_CERT_FILE if HTTPS_ENABLED else None,
        key_file=HTTPS_KEY_FILE if HTTPS_ENABLED else None
    )
    
    # Start WebREPL (available on both WS and WSS)
    webrepl.start(password=WEBREPL_PASSWORD, path="/webrepl")
    
    print(f"HTTP:  http://{ip}/")
    print(f"WS:    ws://{ip}/webrepl")
    
    if HTTPS_ENABLED:
        print(f"HTTPS: https://{ip}/")
        print(f"WSS:   wss://{ip}/webrepl")
    
    # Run queue processor
    while True:
        httpserver.process_queue()
        webrepl.process_queue()
        time.sleep_ms(10)

if __name__ == "__main__":
    main()
```

## Further Reading

- [ESP-IDF HTTPS Server Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/esp_https_server.html)
- [OpenSSL Certificate Generation](https://www.openssl.org/docs/man1.1.1/man1/openssl-req.html)
- [WebSocket Secure (WSS) Protocol](https://datatracker.ietf.org/doc/html/rfc6455)
- [TLS/SSL Best Practices](https://wiki.mozilla.org/Security/Server_Side_TLS)






