#!/bin/bash
# Build script for MicroPython with pyDirect modules
#
# Usage:
#   ./build.sh                    # Build with defaults (HTTPSERVER only)
#   ./build.sh all                # Build with all modules
#   ./build.sh httpserver twai    # Build with specific modules
#   ./build.sh clean              # Clean build directory

set -e

# Configuration - adjust these paths as needed
MPY_DIR="${MPY_DIR:-/Users/jep/github/micropython-1.27}"
PYDIRECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ESP_IDF_PATH="${ESP_IDF_PATH:-/Users/jep/esp/esp-idf-v5.5.1}"
BOARD="${BOARD:-SCRIPTO_S3}"
BOARD_DIR="${PYDIRECT_DIR}/boards/${BOARD}"
BUILD_DIR="${MPY_DIR}/ports/esp32/build-${BOARD}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

print_usage() {
    echo "Usage: $0 [options] [modules...]"
    echo ""
    echo "Options:"
    echo "  clean              Clean build directory"
    echo "  flash              Flash firmware to device after build"
    echo "  merge              Create merged firmware with VFS (for web flasher)"
    echo "  BOARD=<board>      Set board name (default: SCRIPTO_S3)"
    echo "                     Uses pyDirect/boards/<board> for custom boards"
    echo "                     Falls back to MicroPython boards if not found"
    echo ""
    echo "Modules (all enabled by default):"
    echo "  httpserver         HTTP Server modules"
    echo "  twai               TWAI/CAN module"
    echo "  usbmodem           USB Modem module"
    echo "  gvret              GVRET (CAN over TCP for SavvyCAN)"
    echo "  husarnet           Husarnet P2P VPN"
    echo "  webrtc             WebRTC DataChannel for browser integration"
    echo ""
    echo "  Use -<module> to exclude (e.g., -usbmodem)"
    echo ""
    echo "Examples:"
    echo "  $0                           # Build with all modules (default)"
    echo "  $0 flash                     # Build all + flash"
    echo "  $0 -usbmodem                 # Build all except usbmodem"
    echo "  $0 httpserver                # Build with httpserver only"
    echo "  $0 BOARD=SCRIPTO_P4 -usbmodem flash  # P4 without usbmodem + flash"
}

# Check prerequisites
check_prerequisites() {
    if [ ! -d "$MPY_DIR" ]; then
        echo -e "${RED}❌ MicroPython directory not found: $MPY_DIR${NC}"
        echo "Set MPY_DIR environment variable or adjust path in script"
        exit 1
    fi

    if [ ! -d "$ESP_IDF_PATH" ]; then
        echo -e "${RED}❌ ESP-IDF not found: $ESP_IDF_PATH${NC}"
        echo "Set ESP_IDF_PATH environment variable or adjust path in script"
        exit 1
    fi

    if [ ! -f "$PYDIRECT_DIR/micropython.cmake" ]; then
        echo -e "${RED}❌ pyDirect not found: $PYDIRECT_DIR${NC}"
        exit 1
    fi
}

# Source ESP-IDF
source_idf() {
    if [ -f "$ESP_IDF_PATH/export.sh" ]; then
        echo -e "${GREEN}📦 Sourcing ESP-IDF from $ESP_IDF_PATH${NC}"
        source "$ESP_IDF_PATH/export.sh"
    else
        echo -e "${YELLOW}⚠️  ESP-IDF export.sh not found. Make sure ESP-IDF is set up.${NC}"
    fi
}

# Parse arguments
CLEAN=false
FLASH=false
MERGE=false
MODULES=()
EXCLUDE_MODULES=()

# Known modules for validation
KNOWN_MODULES=("httpserver" "twai" "usbmodem" "gvret" "husarnet" "webrtc" "all")

# Parse arguments
for arg in "$@"; do
    if [[ $arg == BOARD=* ]]; then
        BOARD="${arg#BOARD=}"
    elif [ "$arg" == "clean" ]; then
        CLEAN=true
    elif [ "$arg" == "all" ]; then
        MODULES=("httpserver" "twai" "usbmodem" "gvret" "husarnet" "webrtc")
    elif [ "$arg" == "flash" ] || [ "$arg" == "--flash" ]; then
        FLASH=true
    elif [ "$arg" == "merge" ]; then
        MERGE=true
    elif [ "$arg" == "-h" ] || [ "$arg" == "--help" ]; then
        print_usage
        exit 0
    elif [[ $arg == -* ]] && [ "$arg" != "-h" ]; then
        # Exclusion: -usbmodem, -twai, etc.
        EXCLUDE_MODULES+=("${arg#-}")
    elif [ -d "${PYDIRECT_DIR}/boards/${arg}" ]; then
        # Argument matches a board directory - treat as board name
        BOARD="$arg"
    elif [ "$arg" != "clean" ] && [ "$arg" != "flash" ] && [[ ! $arg == BOARD=* ]]; then
        # Check if it's a known module
        if [[ " ${KNOWN_MODULES[*]} " =~ " ${arg} " ]]; then
            MODULES+=("$arg")
        else
            echo -e "${RED}❌ Unknown module or board: ${arg}${NC}"
            echo "Available modules: ${KNOWN_MODULES[*]}"
            echo "Available boards: $(ls -1 ${PYDIRECT_DIR}/boards/ | tr '\n' ' ')"
            exit 1
        fi
    fi
done

# Update BOARD_DIR and BUILD_DIR based on parsed BOARD
BOARD_DIR="${PYDIRECT_DIR}/boards/${BOARD}"
BUILD_DIR="${MPY_DIR}/ports/esp32/build-${BOARD}"

# Determine if using external board or MicroPython built-in board
USE_EXTERNAL_BOARD=false
if [ -d "$BOARD_DIR" ]; then
    USE_EXTERNAL_BOARD=true
    echo -e "${GREEN}📋 Using external board definition: ${BOARD_DIR}${NC}"
else
    echo -e "${YELLOW}📋 Using MicroPython built-in board: ${BOARD}${NC}"
    BOARD_DIR=""
fi

# Default to all modules if none specified
if [ ${#MODULES[@]} -eq 0 ]; then
    MODULES=("httpserver" "twai" "usbmodem" "gvret" "husarnet" "webrtc")
fi

# Apply exclusions (e.g., -usbmodem removes usbmodem from the list)
for exclude in "${EXCLUDE_MODULES[@]}"; do
    MODULES=("${MODULES[@]/$exclude}")
done
# Remove empty elements
MODULES=(${MODULES[@]})

# Main execution
check_prerequisites
source_idf

cd "$MPY_DIR/ports/esp32"

# Clean if requested
if [ "$CLEAN" = true ]; then
    echo -e "${YELLOW}🧹 Cleaning build directory: build-${BOARD}${NC}"
    rm -rf "build-${BOARD}"
fi

# For external boards, copy partition table to MicroPython tree if it exists
# This enables portable paths (relative to ports/esp32) for GHA builds
if [ "$USE_EXTERNAL_BOARD" = true ]; then
    PARTITION_FILES=$(find "${BOARD_DIR}" -name "partitions*.csv" 2>/dev/null)
    if [ -n "$PARTITION_FILES" ]; then
        # Create the boards subdirectory in MicroPython if needed
        mkdir -p "${MPY_DIR}/ports/esp32/boards/${BOARD}"
        for pfile in $PARTITION_FILES; do
            cp "$pfile" "${MPY_DIR}/ports/esp32/boards/${BOARD}/"
            echo -e "${GREEN}📦 Copied $(basename $pfile) to ports/esp32/boards/${BOARD}/${NC}"
        done
    fi
fi

# Build CMake options
CMAKE_OPTS=(
    "-DMODULE_PYDIRECT_HTTPSERVER=OFF"
    "-DMODULE_PYDIRECT_TWAI=OFF"
    "-DMODULE_PYDIRECT_USBMODEM=OFF"
    "-DMODULE_PYDIRECT_GVRET=OFF"
    "-DMODULE_PYDIRECT_HUSARNET=OFF"
    "-DMODULE_PYDIRECT_WEBRTC=OFF"
)

# Add board configuration
if [ "$USE_EXTERNAL_BOARD" = true ]; then
    CMAKE_OPTS+=("-DMICROPY_BOARD=${BOARD}")
    CMAKE_OPTS+=("-DMICROPY_BOARD_DIR=${BOARD_DIR}")
else
    CMAKE_OPTS+=("-DMICROPY_BOARD=${BOARD}")
fi

# Enable requested modules
HTTPSERVER=false
TWAI=false
USBMODEM=false
GVRET=false
HUSARNET=false
WEBRTC=false

for module in "${MODULES[@]}"; do
    case "$module" in
        httpserver)
            CMAKE_OPTS+=("-DMODULE_PYDIRECT_HTTPSERVER=ON")
            HTTPSERVER=true
            ;;
        twai)
            CMAKE_OPTS+=("-DMODULE_PYDIRECT_TWAI=ON")
            TWAI=true
            ;;
        usbmodem)
            CMAKE_OPTS+=("-DMODULE_PYDIRECT_USBMODEM=ON")
            USBMODEM=true
            ;;
        gvret)
            CMAKE_OPTS+=("-DMODULE_PYDIRECT_GVRET=ON")
            GVRET=true
            ;;
        husarnet)
            CMAKE_OPTS+=("-DMODULE_PYDIRECT_HUSARNET=ON")
            HUSARNET=true
            ;;
        webrtc)
            CMAKE_OPTS+=("-DMODULE_PYDIRECT_WEBRTC=ON")
            WEBRTC=true
            ;;
        *)
            echo -e "${RED}❌ Unknown module: $module${NC}"
            exit 1
            ;;
    esac
done

# Display configuration
echo -e "${GREEN}🚀 Building MicroPython firmware${NC}"
echo "  Board: $BOARD"
if [ "$USE_EXTERNAL_BOARD" = true ]; then
    echo "  Board dir: $BOARD_DIR (external)"
else
    echo "  Board dir: (MicroPython built-in)"
fi
echo "  Modules:"
[ "$HTTPSERVER" = true ] && echo "    ✓ httpserver (httpserver, webfiles, wsserver, webrepl)"
[ "$TWAI" = true ] && echo "    ✓ twai (TWAI/CAN bus)"
[ "$USBMODEM" = true ] && echo "    ✓ usbmodem (USB modem support)"
[ "$GVRET" = true ] && echo "    ✓ gvret (CAN over TCP for SavvyCAN)"
[ "$HUSARNET" = true ] && echo "    ✓ husarnet (P2P VPN)"
[ "$WEBRTC" = true ] && echo "    ✓ webrtc (WebRTC DataChannel for browser)"
echo "  Build directory: $BUILD_DIR"
echo ""

# Configure and build
echo -e "${GREEN}⚙️  Configuring CMake...${NC}"
cmake "${CMAKE_OPTS[@]}" \
    -DUSER_C_MODULES="$PYDIRECT_DIR/micropython.cmake" \
    -DEXTRA_COMPONENT_DIRS="$PYDIRECT_DIR/components/pydirect_deps" \
    -S . -B "build-${BOARD}"

echo -e "${GREEN}🔨 Building firmware...${NC}"
make -C "build-${BOARD}" -j$(nproc)

echo -e "${GREEN}✅ Build complete!${NC}"
echo ""
echo "Firmware location: ${BUILD_DIR}/micropython.bin"
echo ""

if [ "$FLASH" = true ]; then
    echo -e "${GREEN}📤 Flashing firmware at 921600 baud (UART)...${NC}"
    make -C "build-${BOARD}" flash BAUD=921600
    echo -e "${GREEN}✅ Flashing complete!${NC}"
else
    echo "To flash the firmware, run:"
    echo "  make -C ${BUILD_DIR} flash"
    echo ""
    echo "Or use this script with 'flash' argument:"
    echo "  $0 ${MODULES[*]} flash"
fi

# ============================================================================
# Merged Firmware Creation (for web flasher)
# ============================================================================

create_vfs_partition() {
    local VFS_SIZE=$1
    local VFS_IMAGE="${BUILD_DIR}/vfs.bin"
    
    echo -e "${GREEN}📦 Creating LittleFS VFS partition...${NC}"
    
    # Check for mklittlefs
    if ! command -v mklittlefs &> /dev/null; then
        echo -e "${RED}❌ mklittlefs not found!${NC}"
        echo "Install with: brew install mklittlefs"
        echo "Or build from: https://github.com/earlephilhower/mklittlefs"
        exit 1
    fi
    
    # Create LittleFS image from device-scripts
    mklittlefs \
        -c "${PYDIRECT_DIR}/device-scripts" \
        -b 4096 \
        -p 256 \
        -s "$VFS_SIZE" \
        "$VFS_IMAGE"
    
    echo -e "${GREEN}✅ VFS partition created: $VFS_IMAGE${NC}"
}

create_merged_firmware() {
    echo -e "${GREEN}📦 Creating merged firmware for web flasher...${NC}"
    
    local MERGED_BIN="${BUILD_DIR}/pyDirect-${BOARD}-merged.bin"
    
    # Get partition info from partition table
    local PARTITION_FILE=$(find "${BOARD_DIR}" -name "partitions*.csv" | head -1)
    if [ -z "$PARTITION_FILE" ]; then
        echo -e "${RED}❌ Partition table not found in ${BOARD_DIR}${NC}"
        exit 1
    fi
    
    # Parse VFS partition offset and size from partition table
    local VFS_LINE=$(grep "^vfs," "$PARTITION_FILE")
    if [ -z "$VFS_LINE" ]; then
        echo -e "${RED}❌ VFS partition not found in partition table${NC}"
        exit 1
    fi
    
    local VFS_OFFSET=$(echo "$VFS_LINE" | cut -d',' -f4 | xargs)
    local VFS_SIZE=$(echo "$VFS_LINE" | cut -d',' -f5 | xargs)
    
    # Convert hex size if needed
    if [[ "$VFS_SIZE" == 0x* ]]; then
        VFS_SIZE=$((VFS_SIZE))
    fi
    
    echo "  Partition table: $PARTITION_FILE"
    echo "  VFS offset: $VFS_OFFSET"
    echo "  VFS size: $VFS_SIZE bytes"
    
    # Create VFS partition
    create_vfs_partition "$VFS_SIZE"
    
    # Detect chip type from board config
    local CHIP="esp32s3"
    if [[ "$BOARD" == *"P4"* ]]; then
        CHIP="esp32p4"
    elif [[ "$BOARD" == *"C3"* ]]; then
        CHIP="esp32c3"
    elif [[ "$BOARD" == *"C6"* ]]; then
        CHIP="esp32c6"
    elif [[ "$BOARD" == *"S2"* ]]; then
        CHIP="esp32s2"
    fi
    
    # Get flash size from sdkconfig
    local FLASH_SIZE="8MB"
    if [ -f "${BOARD_DIR}/sdkconfig.board" ]; then
        local FLASH_CFG=$(grep "CONFIG_ESPTOOLPY_FLASHSIZE" "${BOARD_DIR}/sdkconfig.board" | grep -v "#" | head -1)
        if [[ "$FLASH_CFG" == *"16MB"* ]]; then
            FLASH_SIZE="16MB"
        elif [[ "$FLASH_CFG" == *"4MB"* ]]; then
            FLASH_SIZE="4MB"
        fi
    fi
    
    echo "  Chip: $CHIP"
    echo "  Flash size: $FLASH_SIZE"
    
    # Merge all parts into single binary
    esptool.py --chip "$CHIP" merge_bin \
        --fill-flash-size "$FLASH_SIZE" \
        --flash_mode dio \
        --flash_freq 80m \
        --flash_size "$FLASH_SIZE" \
        -o "$MERGED_BIN" \
        0x0 "${BUILD_DIR}/bootloader/bootloader.bin" \
        0x8000 "${BUILD_DIR}/partition_table/partition-table.bin" \
        0x10000 "${BUILD_DIR}/micropython.bin" \
        $VFS_OFFSET "${BUILD_DIR}/vfs.bin"
    
    echo ""
    echo -e "${GREEN}✅ Merged firmware created!${NC}"
    echo "  Output: $MERGED_BIN"
    echo "  Size: $(du -h "$MERGED_BIN" | cut -f1)"
    echo ""
    echo "This file can be flashed at offset 0x0:"
    echo "  esptool.py --chip $CHIP write_flash 0x0 $MERGED_BIN"
}

# Handle merge command
if [ "$MERGE" = true ]; then
    if [ ! -f "${BUILD_DIR}/micropython.bin" ]; then
        echo -e "${RED}❌ Firmware not built yet. Run build first.${NC}"
        exit 1
    fi
    create_merged_firmware
fi

