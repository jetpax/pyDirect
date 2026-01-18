#!/bin/bash
#
# Upload pyDirect device scripts to ESP32 (board-specific)
# with optional WiFi pre-configuration
#
# Requirements:
#   - mpremote (pip install mpremote)
#
# Usage:
#   ./upload-device-scripts.sh [port] [board]
#
# Examples:
#   ./upload-device-scripts.sh /dev/ttyUSB0 SCRIPTO_P4
#   ./upload-device-scripts.sh /dev/ttyUSB0  # Uses default minimal scripts

set -e

# Colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

print_info() { echo -e "${BLUE}ℹ${NC} $1"; }
print_success() { echo -e "${GREEN}✓${NC} $1"; }
print_warning() { echo -e "${YELLOW}⚠${NC} $1"; }
print_error() { echo -e "${RED}✗${NC} $1"; }

PORT="${1:-/dev/ttyUSB0}"
BOARD="${2:-}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "pyDirect Device Scripts Upload"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# Check mpremote
if ! command -v mpremote &> /dev/null; then
    echo "ERROR: mpremote not found"
    echo "Install with: pip install mpremote"
    exit 1
fi

# Determine device scripts directory
if [ -n "$BOARD" ]; then
    DEVICE_SCRIPTS_DIR="$SCRIPT_DIR/boards/$BOARD/device-scripts"
    if [ ! -d "$DEVICE_SCRIPTS_DIR" ]; then
        echo "ERROR: Board-specific device scripts not found: $DEVICE_SCRIPTS_DIR"
        echo "Available boards:"
        ls -1 "$SCRIPT_DIR/boards/" | grep -v README
        exit 1
    fi
    print_info "Using board-specific scripts: $BOARD"
    HAS_WIFI_ONBOARDING=true
else
    DEVICE_SCRIPTS_DIR="$SCRIPT_DIR/device-scripts"
    print_warning "No board specified, using minimal scripts"
    HAS_WIFI_ONBOARDING=false
fi

print_info "Port: $PORT"
print_info "Scripts: $DEVICE_SCRIPTS_DIR"
echo ""

# Upload boot.py
if [ -f "$DEVICE_SCRIPTS_DIR/boot.py" ]; then
    print_info "Uploading boot.py..."
    mpremote connect "$PORT" fs cp "$DEVICE_SCRIPTS_DIR/boot.py" ":boot.py"
fi

# Upload main.py
if [ -f "$DEVICE_SCRIPTS_DIR/main.py" ]; then
    print_info "Uploading main.py..."
    mpremote connect "$PORT" fs cp "$DEVICE_SCRIPTS_DIR/main.py" ":main.py"
fi

# Upload lib/ directory if it exists
if [ -d "$DEVICE_SCRIPTS_DIR/lib" ]; then
    print_info "Uploading lib/ directory..."
    mpremote connect "$PORT" fs mkdir ":lib" 2>/dev/null || true
    for file in "$DEVICE_SCRIPTS_DIR/lib"/*.py; do
        if [ -f "$file" ]; then
            filename=$(basename "$file")
            print_info "  Uploading lib/$filename..."
            mpremote connect "$PORT" fs cp "$file" ":lib/$filename"
        fi
    done
fi

echo ""
print_success "Device scripts uploaded!"
echo ""

# WiFi pre-configuration (only for board-specific scripts with onboarding)
if [ "$HAS_WIFI_ONBOARDING" = true ]; then
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "WiFi Configuration"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo ""
    
    read -p "Configure WiFi now? (y/n): " configure_wifi
    
    if [ "$configure_wifi" = "y" ] || [ "$configure_wifi" = "Y" ]; then
        echo ""
        print_info "Scanning for WiFi networks..."
        
        # Scan networks and save to temp file
        NETWORKS_FILE=$(mktemp)
        mpremote connect "$PORT" exec "
import network
wlan = network.WLAN(network.STA_IF)
wlan.active(True)
import time
time.sleep(1)
networks = wlan.scan()
networks.sort(key=lambda x: x[3], reverse=True)
for i, net in enumerate(networks[:15]):
    ssid = net[0].decode('utf-8') if isinstance(net[0], bytes) else net[0]
    rssi = net[3]
    secure = '🔒' if net[4] != 0 else '  '
    print(f'{i+1:2d}. {secure} {ssid:32s} ({rssi:3d} dBm)')
wlan.active(False)
" 2>/dev/null | tee "$NETWORKS_FILE"
        
        echo ""
        read -p "Select network (1-15, or 0 for manual entry): " selection
        
        if [ "$selection" = "0" ]; then
            # Manual entry
            read -p "Enter SSID: " ssid
        else
            # Extract SSID from selected line
            ssid=$(sed -n "${selection}p" "$NETWORKS_FILE" | sed -E 's/^[0-9 ]+\. [🔒 ]+ ([^ ]+).*/\1/' | xargs)
            
            if [ -z "$ssid" ]; then
                print_error "Invalid selection"
                rm -f "$NETWORKS_FILE"
                exit 1
            fi
        fi
        
        rm -f "$NETWORKS_FILE"
        
        # Prompt for password
        read -sp "Password for '$ssid' (leave empty for open network): " password
        echo ""
        
        # Save credentials to device
        print_info "Saving WiFi credentials to device..."
        
        # Escape single quotes in SSID and password for Python string
        ssid_escaped="${ssid//\'/\\\'}"
        password_escaped="${password//\'/\\\'}"
        
        mpremote connect "$PORT" exec "
from lib.wifi_manager import WiFiManager
wifi_mgr = WiFiManager()
if wifi_mgr.save_credentials('$ssid_escaped', '$password_escaped'):
    print('✓ WiFi credentials saved')
else:
    print('✗ Failed to save credentials')
" 2>/dev/null
        
        echo ""
        print_success "WiFi configured!"
        print_info "Device will connect to '$ssid' on next boot"
        print_info "Captive portal will be skipped"
    else
        echo ""
        print_info "WiFi configuration skipped"
        print_info "Device will start in captive portal mode on first boot"
    fi
    
    echo ""
fi

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Next Steps"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

if [ "$HAS_WIFI_ONBOARDING" = true ]; then
    print_info "1. Reboot device:"
    echo "     mpremote connect $PORT exec 'import machine; machine.reset()'"
    echo ""
    
    if [ "$configure_wifi" = "y" ] || [ "$configure_wifi" = "Y" ]; then
        print_info "2. Device will connect to WiFi automatically"
        echo "     • Check serial console for IP address"
        echo "     • Or visit: http://pyDirect-XXXX.local"
        echo ""
        print_info "3. To reset WiFi credentials:"
        echo "     mpremote connect $PORT exec 'from lib.wifi_manager import WiFiManager; WiFiManager().clear_credentials()'"
    else
        print_info "2. WiFi Onboarding (Captive Portal):"
        echo "     • Device will start AP: pyDirect-XXXX"
        echo "     • Connect to AP and visit: http://192.168.4.1/setup"
        echo "     • Configure WiFi credentials"
        echo "     • Device will be available at: http://pyDirect-XXXX.local"
    fi
else
    print_info "1. Edit main.py to configure WiFi:"
    echo "     WIFI_SSID = 'YOUR_SSID'"
    echo "     WIFI_PASSWORD = 'YOUR_PASSWORD'"
    echo ""
    print_info "2. Reboot device:"
    echo "     mpremote connect $PORT exec 'import machine; machine.reset()'"
    echo ""
    print_info "3. For WiFi onboarding, use board-specific scripts:"
    echo "     ./upload-device-scripts.sh $PORT SCRIPTO_P4"
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
