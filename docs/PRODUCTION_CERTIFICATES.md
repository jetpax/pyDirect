# Production Certificates for pyDirect + Scripto Studio

## Overview

> **Note:** The [web flasher](https://jetpax.github.io/pyDirect/) automatically generates self-signed certificates during device provisioning. This works well for development and testing.
>
> This guide is for **production deployments** where you need trusted certificates that won't show browser warnings.

For production use (especially iOS/iPad), self-signed certificates are not viable. This guide covers two approaches:
1. **Private CA** (recommended for local networks)
2. **Let's Encrypt** (for internet-accessible devices)

---

## Option 1: Private CA (Recommended for Local Networks)

### Why This Works for iOS/iPad

- Install **one CA certificate** on all user devices (iPad, iPhone, Mac)
- Generate **unlimited device certificates** signed by your CA
- All devices trust your certificates automatically
- No internet required, works on local network
- Meets iOS PWA security requirements

### Step 1: Install mkcert (Local CA Tool)

```bash
# macOS
brew install mkcert nss  # nss for Firefox support

# Linux
apt install mkcert

# Windows
choco install mkcert
```

### Step 2: Create and Install Your CA

```bash
# Create a local CA (one-time setup)
mkcert -install

# This creates:
# - Root CA certificate
# - Installs it in your system trust store
# - Stores CA files in: $(mkcert -CAROOT)
```

**Important:** Save the CA certificate for distribution to other devices!

```bash
# Find your CA files
mkcert -CAROOT

# Example output: /Users/jep/Library/Application Support/mkcert
# Copy these files somewhere safe for iOS installation
```

### Step 3: Generate Device Certificates

```bash
# Generate cert for specific device (using mDNS hostname)
mkcert "scripto-2b88.local" "192.168.1.32"

# Or generate wildcard for all devices
mkcert "scripto-*.local" "*.local"

# This creates:
# - scripto-2b88.local.pem (certificate)
# - scripto-2b88.local-key.pem (private key)
```

### Step 4: Upload Certificates to ESP32

```bash
# Rename for consistency with your setup
mv scripto-2b88.local.pem servercert.pem
mv scripto-2b88.local-key.pem prvtkey.pem

# Upload via ScriptO Studio file manager to /certs/
# Or use WebREPL file transfer
```

### Step 5: Install CA Certificate on iOS/iPad

**Method 1: AirDrop (Easiest)**

```bash
# Locate your CA certificate
CA_ROOT=$(mkcert -CAROOT)
open "$CA_ROOT"

# AirDrop the rootCA.pem file to your iPad
```

**Method 2: Email or Web Server**

```bash
# Email the rootCA.pem file to yourself
# Open on iPad, follow prompts to install profile
```

**Method 3: Via Configuration Profile**

Create a `.mobileconfig` profile (for MDM or manual installation):

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>PayloadContent</key>
    <array>
        <dict>
            <key>PayloadCertificateFileName</key>
            <string>ScriptO-CA.pem</string>
            <key>PayloadContent</key>
            <data>
            <!-- Base64 encoded rootCA.pem goes here -->
            </data>
            <key>PayloadDisplayName</key>
            <string>ScriptO Studio Root CA</string>
            <key>PayloadIdentifier</key>
            <string>com.scripto.ca.root</string>
            <key>PayloadType</key>
            <string>com.apple.security.root</string>
            <key>PayloadUUID</key>
            <string>GENERATE-UUID-HERE</string>
            <key>PayloadVersion</key>
            <integer>1</integer>
        </dict>
    </array>
    <key>PayloadDisplayName</key>
    <string>ScriptO Studio CA Certificate</string>
    <key>PayloadIdentifier</key>
    <string>com.scripto.ca</string>
    <key>PayloadRemovalDisallowed</key>
    <false/>
    <key>PayloadType</key>
    <string>Configuration</string>
    <key>PayloadUUID</key>
    <string>GENERATE-UUID-HERE</string>
    <key>PayloadVersion</key>
    <integer>1</integer>
</dict>
</plist>
```

**iOS Installation Steps:**

1. Transfer CA certificate to iPad (AirDrop/email/web)
2. iPad will prompt: "Profile Downloaded"
3. Go to **Settings → General → VPN & Device Management**
4. Tap on the profile → **Install**
5. Enter passcode
6. Tap **Install** again (warning about root certificate)
7. Go to **Settings → General → About → Certificate Trust Settings**
8. **Enable Full Trust** for your CA

### Step 6: Test on iPad

```bash
# On iPad Safari, visit:
https://scripto-2b88.local/

# Should show secure padlock, no warnings!
```

---

## Option 2: Let's Encrypt (Public Domain Required)

### Requirements

- **Public domain name** (e.g., scripto-device-123.yourdomain.com)
- **Port 80/443 accessible** from internet for ACME challenge
- **Dynamic DNS** (if device IP changes)

### Not Recommended Because

❌ ESP32 devices are typically on **local networks**  
❌ Requires **port forwarding** (security risk)  
❌ Requires **public domain** for each device  
❌ **Certificates expire every 90 days** (auto-renewal on ESP32 is complex)  
❌ Doesn't work **offline/air-gapped** networks  

### If You Still Want Let's Encrypt

Use **certbot** on a separate server/computer:

```bash
# Install certbot
brew install certbot  # macOS
# or: apt install certbot  # Linux

# Get certificate (HTTP challenge - requires port 80)
sudo certbot certonly --standalone \
  -d scripto-device-123.yourdomain.com

# Or use DNS challenge (no port forwarding needed)
sudo certbot certonly --manual --preferred-challenges dns \
  -d scripto-device-123.yourdomain.com

# Certificates stored in:
# /etc/letsencrypt/live/scripto-device-123.yourdomain.com/

# Copy to ESP32:
# fullchain.pem → servercert.pem
# privkey.pem → prvtkey.pem
```

---

## Option 3: Commercial CA Certificate

Purchase from DigiCert, Sectigo, etc.

**Pros:**
- Trusted by all devices out-of-the-box
- No CA installation needed

**Cons:**
- **Expensive** ($50-$200+ per year per certificate)
- Requires **public domain name**
- **Not practical** for multiple local devices

---

## Recommended Deployment Strategy

### For Development (Current)
✅ Self-signed certificates  
✅ Manual acceptance per device/browser  
✅ Fast iteration  

### For Beta Testing
✅ **Private CA (mkcert)**  
✅ Distribute CA cert to testers once  
✅ Generate device certs as needed  

### For Production (Local Network)
✅ **Private CA with MDM**  
✅ Deploy CA cert via Mobile Device Management  
✅ Or: Include CA cert in onboarding docs  
✅ Generate device certs automatically during provisioning  

### For Production (Internet-Accessible)
✅ **Let's Encrypt** with auto-renewal  
✅ Central server manages certificates  
✅ Sync to devices periodically  

---

## iOS/iPad Specific Considerations

### PWA Requirements

For ScriptO Studio to work as a PWA on iOS:
1. ✅ **Must be served over HTTPS** (http:// will not install)
2. ✅ **Certificate must be trusted** (self-signed won't work after installation)
3. ✅ **Valid certificate chain** (no intermediate cert issues)
4. ✅ **Hostname must match certificate** (IP addresses have issues on iOS)

### Recommended for iOS Production

```bash
# Generate cert with BOTH hostname and IP as SANs
mkcert \
  "scripto-2b88.local" \
  "*.scripto-2b88.local" \
  "192.168.1.32" \
  "localhost"

# iOS will work with either:
# - https://scripto-2b88.local/
# - https://192.168.1.32/
```

---

## Certificate Management Script

Automate certificate generation for multiple devices:

```bash
#!/bin/bash
# generate-device-cert.sh

DEVICE_MAC="$1"  # e.g., 2b88
DEVICE_IP="$2"   # e.g., 192.168.1.32

if [ -z "$DEVICE_MAC" ] || [ -z "$DEVICE_IP" ]; then
    echo "Usage: $0 <device_mac> <device_ip>"
    echo "Example: $0 2b88 192.168.1.32"
    exit 1
fi

HOSTNAME="scripto-${DEVICE_MAC}.local"

echo "Generating certificate for $HOSTNAME ($DEVICE_IP)..."

# Generate certificate
mkcert "$HOSTNAME" "$DEVICE_IP"

# Rename for ESP32
mv "${HOSTNAME}.pem" servercert.pem
mv "${HOSTNAME}-key.pem" prvtkey.pem

echo "✅ Certificates generated:"
echo "   servercert.pem"
echo "   prvtkey.pem"
echo ""
echo "📤 Upload to device /certs/ directory"
echo "   Device: $HOSTNAME ($DEVICE_IP)"
```

Usage:
```bash
chmod +x generate-device-cert.sh
./generate-device-cert.sh 2b88 192.168.1.32
```

---

## Troubleshooting

### iOS Says "Not Trusted"

Check:
1. CA certificate installed in **Settings → General → VPN & Device Management**
2. CA certificate **trusted** in **Settings → About → Certificate Trust Settings**
3. Device hostname matches certificate CN/SAN
4. Using **Safari** (not Chrome - Chrome on iOS uses Safari engine)

### Certificate Expires

- mkcert certificates expire after **10 years** (basically never)
- Let's Encrypt expires after **90 days** (requires renewal)

### mDNS Not Working on iOS

- Ensure device is on **same WiFi network**
- iOS supports `.local` hostnames natively
- Try **forgetting and rejoining** WiFi network
- Check **Private Wi-Fi Address** is disabled for your network

---

## Summary

**For your use case (iPad target, local network):**

1. ✅ Use **mkcert** to create private CA
2. ✅ Install CA cert on all user devices (iPad, iPhone, Mac) - **one time**
3. ✅ Generate device certs as needed (scripto-XXXX.local)
4. ✅ No internet required, works offline
5. ✅ Meets iOS PWA requirements
6. ✅ No browser warnings or manual acceptance

**Time investment:**
- Setup CA: 5 minutes
- Generate device cert: 30 seconds
- Install CA on iPad: 2 minutes (per device, one time)

**Result:** Professional, production-ready HTTPS with zero friction! 🚀






