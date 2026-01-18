#!/bin/bash
#
# 
# Generates mkcert certificates for ESP32 devices using their MAC-based hostname.
# Uses the same naming scheme as the device firmware (scripto-XXXX.local).
#
# Requirements:
#   - mkcert installed (brew install mkcert)
#   - CA already created (mkcert -install)
#
# Usage:
#   ./generate-device-cert.sh <mac_suffix> [ip_address]
#
# Examples:
#   ./generate-device-cert.sh 2b88 192.168.1.32
#   ./generate-device-cert.sh 3a4f              # IP optional
#   ./generate-device-cert.sh --help

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Print colored output
print_info() { echo -e "${BLUE}ℹ${NC} $1"; }
print_success() { echo -e "${GREEN}✓${NC} $1"; }
print_warning() { echo -e "${YELLOW}⚠${NC} $1"; }
print_error() { echo -e "${RED}✗${NC} $1"; }

# Print usage
usage() {
    cat << EOF
pyDirect Device Certificate Generator
=====================================

Generates trusted certificates for ESP32 devices using mkcert.

USAGE:
    $0 <mac_suffix> [ip_address]

ARGUMENTS:
    mac_suffix    Last 4 characters of MAC address (e.g., 2b88, 3a4f)
                  Device will be named: scripto-<mac_suffix>.local
    
    ip_address    Optional device IP address (e.g., 192.168.1.32)
                  If provided, certificate will include IP as SAN

EXAMPLES:
    $0 2b88 192.168.1.32    # Generate cert for scripto-2b88.local at 192.168.1.32
    $0 3a4f                 # Generate cert for scripto-3a4f.local (no IP)

REQUIREMENTS:
    - mkcert must be installed: brew install mkcert
    - CA must be initialized: mkcert -install

OUTPUT:
    Two files will be created in the current directory:
    - servercert.pem (certificate + chain)
    - prvtkey.pem (private key)
    
    Upload these to the device at: /certs/

NOTES:
    - Certificate valid for 10 years
    - Includes both hostname.local and wildcard *.hostname.local
    - Safe to regenerate (will overwrite existing files)
    - Certificates work on all devices with your CA installed

MORE INFO:
    See PRODUCTION_CERTIFICATES.md for full setup instructions

EOF
}

# Check if help requested
if [[ "$1" == "--help" ]] || [[ "$1" == "-h" ]]; then
    usage
    exit 0
fi

# Check arguments
if [ -z "$1" ]; then
    print_error "Missing MAC suffix argument"
    echo ""
    usage
    exit 1
fi

MAC_SUFFIX="$1"
DEVICE_IP="$2"

# Validate MAC suffix format (4 hex characters)
if ! [[ "$MAC_SUFFIX" =~ ^[0-9a-fA-F]{4}$ ]]; then
    print_error "Invalid MAC suffix: $MAC_SUFFIX"
    print_info "MAC suffix must be 4 hexadecimal characters (e.g., 2b88, 3a4f)"
    exit 1
fi

# Convert to lowercase for consistency
MAC_SUFFIX=$(echo "$MAC_SUFFIX" | tr '[:upper:]' '[:lower:]')

# Build hostname
HOSTNAME="scripto-${MAC_SUFFIX}.local"

# Check if mkcert is installed
if ! command -v mkcert &> /dev/null; then
    print_error "mkcert not found"
    print_info "Install with: brew install mkcert"
    print_info "Then run: mkcert -install"
    exit 1
fi

# Check if CA is initialized
if ! mkcert -CAROOT &> /dev/null; then
    print_error "mkcert CA not initialized"
    print_info "Run: mkcert -install"
    exit 1
fi

# Print header
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "pyDirect Device Certificate Generator"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# Print configuration
print_info "Device Configuration:"
echo "  MAC Suffix:  $MAC_SUFFIX"
echo "  Hostname:    $HOSTNAME"
if [ -n "$DEVICE_IP" ]; then
    echo "  IP Address:  $DEVICE_IP"
else
    echo "  IP Address:  (none - hostname only)"
fi
echo ""

# Build mkcert arguments
MKCERT_ARGS=(
    "$HOSTNAME"
    "*.${HOSTNAME}"  # Wildcard for subdomains
)

if [ -n "$DEVICE_IP" ]; then
    MKCERT_ARGS+=("$DEVICE_IP")
fi

# Generate certificate
print_info "Generating certificate..."
if mkcert "${MKCERT_ARGS[@]}" > /dev/null 2>&1; then
    print_success "Certificate generated"
else
    print_error "Failed to generate certificate"
    exit 1
fi

# Find generated files (mkcert creates them with combined name)
if [ -n "$DEVICE_IP" ]; then
    CERT_FILE="${HOSTNAME}+2.pem"
    KEY_FILE="${HOSTNAME}+2-key.pem"
else
    CERT_FILE="${HOSTNAME}+1.pem"
    KEY_FILE="${HOSTNAME}+1-key.pem"
fi

# Verify files were created
if [ ! -f "$CERT_FILE" ] || [ ! -f "$KEY_FILE" ]; then
    print_error "Certificate files not found"
    print_info "Expected: $CERT_FILE and $KEY_FILE"
    exit 1
fi

# Rename to standard names
print_info "Renaming files..."
mv "$CERT_FILE" servercert.pem
mv "$KEY_FILE" prvtkey.pem
print_success "Files renamed to servercert.pem and prvtkey.pem"

# Get certificate info
CERT_EXPIRY=$(openssl x509 -in servercert.pem -noout -enddate 2>/dev/null | cut -d= -f2)
CERT_SUBJECT=$(openssl x509 -in servercert.pem -noout -subject 2>/dev/null | cut -d= -f2-)
CERT_SANS=$(openssl x509 -in servercert.pem -noout -text 2>/dev/null | grep -A1 "Subject Alternative Name" | tail -1 | sed 's/^[[:space:]]*//')

# Print summary
echo ""
print_success "Certificate Generated Successfully!"
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Certificate Details"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "  Files Created:"
echo "    📄 servercert.pem  ($(wc -c < servercert.pem | xargs) bytes)"
echo "    🔑 prvtkey.pem     ($(wc -c < prvtkey.pem | xargs) bytes)"
echo ""
echo "  Subject:"
echo "    $CERT_SUBJECT"
echo ""
echo "  Valid Until:"
echo "    $CERT_EXPIRY"
echo ""
echo "  Alternative Names:"
echo "    $CERT_SANS"
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Next Steps"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
print_info "1. Upload certificates to device:"
echo "     • Connect to device via pyDirect Studio"
echo "     • Open File Manager"
echo "     • Navigate to /certs/ directory (create if needed)"
echo "     • Upload servercert.pem and prvtkey.pem"
echo ""
print_info "2. Enable HTTPS in main.py:"
echo "     HTTPS_ENABLED = True"
echo "     HTTPS_CERT_FILE = '/certs/servercert.pem'"
echo "     HTTPS_KEY_FILE = '/certs/prvtkey.pem'"
echo ""
print_info "3. Reboot device"
echo ""
print_info "4. Test connection:"
echo "     • https://$HOSTNAME/"
if [ -n "$DEVICE_IP" ]; then
    echo "     • https://$DEVICE_IP/"
fi
echo "     • wss://$HOSTNAME/webrepl"
echo ""
print_warning "Note: Users must install the CA certificate on their devices"
print_info "CA location: $(mkcert -CAROOT)"
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

