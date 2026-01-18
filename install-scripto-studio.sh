#!/bin/bash
#
# Install Scripto Studio device scripts to ESP32
#
# This downloads and installs the full Scripto Studio device-side scripts,
# which provide advanced features like:
#   - Network management (WiFi, Ethernet, WWAN failover)
#   - Background task system with asyncio
#   - Settings API
#   - Client helper functions
#   - WebRTC signaling
#   - Status LED management
#
# Requirements:
#   - mpremote (pip install mpremote)
#   - git
#
# Usage:
#   ./install-scripto-studio.sh [port] [branch]
#
# Examples:
#   ./install-scripto-studio.sh /dev/ttyUSB0
#   ./install-scripto-studio.sh /dev/ttyUSB0 main

set -e

# Colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

print_info() { echo -e "${BLUE}ℹ${NC} $1"; }
print_success() { echo -e "${GREEN}✓${NC} $1"; }
print_warning() { echo -e "${YELLOW}⚠${NC} $1"; }

PORT="${1:-/dev/ttyUSB0}"
BRANCH="${2:-main}"
REPO_URL="https://github.com/jetpax/scripto-studio.git"
TMP_DIR="/tmp/scripto-studio-install-$$"

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Scripto Studio Device Scripts Installer"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# Check mpremote
if ! command -v mpremote &> /dev/null; then
    echo "ERROR: mpremote not found"
    echo "Install with: pip install mpremote"
    exit 1
fi

# Check git
if ! command -v git &> /dev/null; then
    echo "ERROR: git not found"
    exit 1
fi

print_info "Port: $PORT"
print_info "Repository: $REPO_URL"
print_info "Branch: $BRANCH"
echo ""

# Clone Scripto Studio
print_info "Cloning Scripto Studio..."
git clone --depth 1 --branch "$BRANCH" "$REPO_URL" "$TMP_DIR" 2>&1 | grep -v "Cloning into" || true

DEVICE_SCRIPTS_DIR="$TMP_DIR/device scripts"

if [ ! -d "$DEVICE_SCRIPTS_DIR" ]; then
    echo "ERROR: Device scripts not found in repository"
    rm -rf "$TMP_DIR"
    exit 1
fi

# Upload files
print_info "Uploading device scripts..."

# Upload boot.py and main.py
mpremote connect "$PORT" fs cp "$DEVICE_SCRIPTS_DIR/boot.py" ":boot.py"
mpremote connect "$PORT" fs cp "$DEVICE_SCRIPTS_DIR/main.py" ":main.py"

# Upload lib/ directory
if [ -d "$DEVICE_SCRIPTS_DIR/lib" ]; then
    print_info "Uploading lib/ directory..."
    mpremote connect "$PORT" fs mkdir ":lib" 2>/dev/null || true
    for file in "$DEVICE_SCRIPTS_DIR/lib"/*.py; do
        if [ -f "$file" ]; then
            filename=$(basename "$file")
            mpremote connect "$PORT" fs cp "$file" ":lib/$filename"
        fi
    done
fi

# Upload settings/ directory
if [ -d "$DEVICE_SCRIPTS_DIR/settings" ]; then
    print_info "Uploading settings/ directory..."
    mpremote connect "$PORT" fs mkdir ":settings" 2>/dev/null || true
    for file in "$DEVICE_SCRIPTS_DIR/settings"/*; do
        if [ -f "$file" ]; then
            filename=$(basename "$file")
            mpremote connect "$PORT" fs cp "$file" ":settings/$filename"
        fi
    done
fi

# Cleanup
rm -rf "$TMP_DIR"

echo ""
print_success "Scripto Studio device scripts installed!"
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Next Steps"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
print_info "1. Configure settings (optional):"
echo "     Edit /settings/config.json on device"
echo ""
print_info "2. Reboot device:"
echo "     mpremote connect $PORT exec 'import machine; machine.reset()'"
echo ""
print_info "3. Connect via Scripto Studio IDE:"
echo "     https://scripto.studio"
echo ""
print_warning "Note: Device will auto-start network, HTTP server, and WebREPL on boot"
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
