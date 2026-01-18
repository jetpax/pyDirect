# Installing ESP32 Certificate in Browser Trust Store

To eliminate the 2-second TLS handshake delay caused by self-signed certificate rejection, install the ESP32's certificate in your browser's trusted certificate store.

## Why This Helps

When the browser encounters a self-signed certificate, it:
1. Rejects the first connection (fatal alert, `-0x7780` error)
2. Waits ~2 seconds
3. Retries the connection (succeeds if user previously accepted)

By pre-trusting the certificate, the browser skips step 1-2 entirely.

## Instructions by Browser

### Chrome / Edge (macOS)

1. **Export the certificate from the device:**
   ```bash
   # From the device filesystem (via WebREPL file manager):
   # Download /certs/servercert.pem to your computer
   ```

2. **Install in macOS Keychain:**
   ```bash
   # Open Keychain Access
   open /Applications/Utilities/Keychain\ Access.app
   
   # OR use command line:
   sudo security add-trusted-cert -d -r trustRoot \
     -k /Library/Keychains/System.keychain servercert.pem
   ```

3. **In Keychain Access GUI:**
   - Drag `servercert.pem` into "System" keychain
   - Double-click the certificate
   - Expand "Trust" section
   - Set "When using this certificate" to **"Always Trust"**
   - Close (requires password)

4. **Restart browser** for changes to take effect

### Firefox (macOS/Linux/Windows)

1. Go to `about:preferences#privacy`
2. Scroll to "Certificates" → Click **"View Certificates"**
3. Go to **"Authorities"** tab
4. Click **"Import..."**
5. Select your `servercert.pem` file
6. Check **"Trust this CA to identify websites"**
7. Click **OK**
8. Restart browser

### Safari (macOS)

Same as Chrome - uses macOS Keychain Access.

### Chrome (Windows)

1. Open Chrome Settings → Privacy and security → Security
2. Scroll to "Manage certificates"
3. Go to **"Trusted Root Certification Authorities"** tab
4. Click **"Import..."**
5. Follow wizard to import `servercert.pem`
6. Restart browser

### Chrome (Linux)

```bash
# Add to system-wide CA certificates
sudo cp servercert.pem /usr/local/share/ca-certificates/esp32-scripto.crt
sudo update-ca-certificates

# Restart browser
```

## Verifying Installation

After installation, connect to `https://scripto-2b88.local/` or `wss://scripto-2b88.local/webrepl`:
- The padlock icon should be green (or show as secure)
- No security warnings
- **No 2-second delay** on first connection!

## Notes

- This certificate is **device-specific** (tied to MAC address `scripto-2b88`)
- If you have multiple devices, you'll need to install each device's certificate
- Certificates generated with different MAC addresses require separate installation
- **For production**, use a proper CA-signed certificate or mkcert

## Alternative: Using mkcert for Development

For a better development experience with multiple devices:

```bash
# Install mkcert (creates a local CA)
brew install mkcert  # macOS
# or: apt install mkcert  # Linux
# or: choco install mkcert  # Windows

# Create and install local CA
mkcert -install

# Generate wildcard certificate
mkcert "scripto-*.local" "*.scripto-*.local"

# Upload generated .pem files to device /certs/ directory
```

This creates a **local CA** that your browser trusts, avoiding the need to install each device certificate individually.






