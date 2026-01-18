#!/bin/bash
#
# Upload certificates to ESP32 device
#
# Requirements:
#   - mpremote (pip install mpremote)
#   OR
#   - ampy (pip install adafruit-ampy)
#
# Usage:
#   ./upload-certs.sh [port] [cert_file] [key_file]
#
# Examples:
#   ./upload-certs.sh /dev/ttyUSB0
#   ./upload-certs.sh /dev/ttyUSB0 servercert.pem prvtkey.pem

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

print_info() { echo -e "${BLUE}ℹ${NC} $1"; }
print_success() { echo -e "${GREEN}✓${NC} $1"; }
print_warning() { echo -e "${YELLOW}⚠${NC} $1"; }
print_error() { echo -e "${RED}✗${NC} $1"; }

# Default values
PORT="${1:-/dev/ttyUSB0}"
CERT_FILE="${2:-servercert.pem}"
KEY_FILE="${3:-prvtkey.pem}"
REMOTE_DIR="/certs"

# Print header
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "pyDirect Certificate Upload"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# Check if files exist
if [ ! -f "$CERT_FILE" ]; then
    print_error "Certificate file not found: $CERT_FILE"
    exit 1
fi

if [ ! -f "$KEY_FILE" ]; then
    print_error "Key file not found: $KEY_FILE"
    exit 1
fi

print_info "Configuration:"
echo "  Port:        $PORT"
echo "  Certificate: $CERT_FILE"
echo "  Private Key: $KEY_FILE"
echo "  Remote Dir:  $REMOTE_DIR"
echo ""

# Check which tool is available
TOOL=""
if command -v mpremote &> /dev/null; then
    TOOL="mpremote"
    print_info "Using mpremote"
elif command -v ampy &> /dev/null; then
    TOOL="ampy"
    print_info "Using ampy"
else
    print_error "Neither mpremote nor ampy found"
    echo ""
    echo "Install one of:"
    echo "  pip install mpremote"
    echo "  pip install adafruit-ampy"
    exit 1
fi

# Function to upload with mpremote
upload_mpremote() {
    print_info "Creating $REMOTE_DIR directory..."
    mpremote connect "$PORT" exec "import os; os.mkdir('$REMOTE_DIR')" 2>/dev/null || true
    
    print_info "Uploading $CERT_FILE..."
    mpremote connect "$PORT" fs cp "$CERT_FILE" ":$REMOTE_DIR/servercert.pem"
    
    print_info "Uploading $KEY_FILE..."
    mpremote connect "$PORT" fs cp "$KEY_FILE" ":$REMOTE_DIR/prvtkey.pem"
    
    print_info "Verifying files..."
    mpremote connect "$PORT" fs ls "$REMOTE_DIR"
}

# Function to upload with ampy
upload_ampy() {
    print_info "Creating $REMOTE_DIR directory..."
    ampy --port "$PORT" run - <<EOF 2>/dev/null || true
import os
try:
    os.mkdir('$REMOTE_DIR')
except:
    pass
EOF
    
    print_info "Uploading $CERT_FILE..."
    ampy --port "$PORT" put "$CERT_FILE" "$REMOTE_DIR/servercert.pem"
    
    print_info "Uploading $KEY_FILE..."
    ampy --port "$PORT" put "$KEY_FILE" "$REMOTE_DIR/prvtkey.pem"
    
    print_info "Verifying files..."
    ampy --port "$PORT" ls "$REMOTE_DIR"
}

# Upload files
echo ""
if [ "$TOOL" = "mpremote" ]; then
    upload_mpremote
else
    upload_ampy
fi

echo ""
print_success "Certificates uploaded successfully!"
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Next Steps"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
print_info "1. Enable HTTPS in main.py:"
echo "     HTTPS_ENABLED = True"
echo "     HTTPS_CERT_FILE = '/certs/servercert.pem'"
echo "     HTTPS_KEY_FILE = '/certs/prvtkey.pem'"
echo ""
print_info "2. Reboot device:"
echo "     mpremote connect $PORT exec 'import machine; machine.reset()'"
echo ""
print_info "3. Test HTTPS connection:"
echo "     https://device-ip/"
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
