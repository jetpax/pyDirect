#!/bin/bash
# Build VFS partition image from device-scripts
# This can be run independently to update just the filesystem without rebuilding firmware

set -e

# Configuration
PYDIRECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOARD="${BOARD:-SCRIPTO_S3}"
MPY_DIR="${MPY_DIR:-/Users/jep/github/micropython-1.27}"
BUILD_DIR="${MPY_DIR}/ports/esp32/build-${BOARD}"
BOARD_DIR="${PYDIRECT_DIR}/boards/${BOARD}"

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}📦 Building VFS partition image${NC}"
echo "  Board: $BOARD"
echo "  Source: ${PYDIRECT_DIR}/device-scripts"
echo "  Output: ${BUILD_DIR}/vfs.bin"
echo ""

# Check for mklittlefs
if ! command -v mklittlefs &> /dev/null; then
    echo -e "${RED}❌ mklittlefs not found!${NC}"
    echo "Install with: brew install mklittlefs"
    echo "Or build from: https://github.com/earlephilhower/mklittlefs"
    exit 1
fi

# Create build directory if it doesn't exist
if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${YELLOW}📁 Creating build directory: $BUILD_DIR${NC}"
    mkdir -p "$BUILD_DIR"
fi

# Get partition info from partition table
PARTITION_FILE=$(find "${BOARD_DIR}" -name "partitions*.csv" 2>/dev/null | head -1)
if [ -z "$PARTITION_FILE" ]; then
    echo -e "${RED}❌ Partition table not found in ${BOARD_DIR}${NC}"
    exit 1
fi

# Parse VFS partition size from partition table
VFS_LINE=$(grep "^vfs," "$PARTITION_FILE")
if [ -z "$VFS_LINE" ]; then
    echo -e "${RED}❌ VFS partition not found in partition table${NC}"
    exit 1
fi

VFS_SIZE=$(echo "$VFS_LINE" | cut -d',' -f5 | xargs)

# Convert hex size if needed
if [[ "$VFS_SIZE" == 0x* ]]; then
    VFS_SIZE=$((VFS_SIZE))
fi

echo "  Partition table: $PARTITION_FILE"
echo "  VFS size: $VFS_SIZE bytes"
echo ""

# Create LittleFS image from device-scripts
echo -e "${GREEN}🔨 Creating LittleFS image...${NC}"
mklittlefs \
    -c "${PYDIRECT_DIR}/device-scripts" \
    -b 4096 \
    -p 256 \
    -s "$VFS_SIZE" \
    "${BUILD_DIR}/vfs.bin"

echo ""
echo -e "${GREEN}✅ VFS partition created!${NC}"
echo "  Output: ${BUILD_DIR}/vfs.bin"
echo "  Size: $(du -h "${BUILD_DIR}/vfs.bin" | cut -f1)"
echo ""
echo "To flash just the VFS partition:"
echo "  esptool.py write_flash <VFS_OFFSET> ${BUILD_DIR}/vfs.bin"
echo ""
echo "Or rebuild the merged firmware:"
echo "  ./build.sh merge"
