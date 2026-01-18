# Makefile for pyDirect - MicroPython C Modules for ESP32
# Builds ESP32 MicroPython firmware with pyDirect modules
# Usage: make firmware  (or: make flash)

# Configuration - adjust these paths for your setup
MPY_DIR := $(HOME)/github/micropython
ESP_IDF_PATH := /Users/jep/esp/esp-idf-v5.4.2
PYDIRECT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

# ESP32 specific settings
BOARD ?= ESP32_GENERIC_S3
# Dual port JTAG adapter ports:
#   - /dev/cu.usbserial-20100: JTAG port (used by OpenOCD)
#   - /dev/cu.usbserial-20101: Serial/UART port (used for erase/monitor)
PORT ?= /dev/cu.wchusbserial5ABA0883981
ESPBAUD ?= 460800

# JTAG/OpenOCD settings (for dual port JTAG adapter)
# Auto-detect OpenOCD scripts directory
# ESP-IDF will automatically select board/esp32s3-ftdi.cfg for ESP32-S3
OPENOCD_SCRIPTS ?= $(shell if [ -d "/usr/local/share/openocd/scripts" ]; then echo "/usr/local/share/openocd/scripts"; elif [ -d "$$HOME/esp/openocd-esp32/share/openocd/scripts" ]; then echo "$$HOME/esp/openocd-esp32/share/openocd/scripts"; else echo ""; fi)

# Module selection (can be overridden)
# Set MODULE_ALL=ON to enable all modules at once
MODULE_ALL ?= ON

ifeq ($(MODULE_ALL),ON)
MODULE_HTTPSERVER := ON
MODULE_TWAI := ON
MODULE_USBMODEM := ON
MODULE_GVRET := ON
MODULE_HUSARNET := ON
MODULE_WEBRTC := ON
else
MODULE_HTTPSERVER ?= ON
MODULE_TWAI ?= OFF
MODULE_USBMODEM ?= OFF
MODULE_GVRET ?= OFF
MODULE_HUSARNET ?= OFF
MODULE_WEBRTC ?= OFF
endif

# Build directory
BUILD_DIR := $(MPY_DIR)/ports/esp32/build

.PHONY: all firmware flash flash-only flash-jtag flash-jtag-only erase-flash-jtag monitor clean help modules

all: firmware

# Show configured modules
modules:
	@echo "pyDirect Module Configuration:"
	@echo "  HTTPSERVER: $(MODULE_HTTPSERVER)"
	@echo "  TWAI:       $(MODULE_TWAI)"
	@echo "  USBMODEM:   $(MODULE_USBMODEM)"
	@echo "  GVRET:      $(MODULE_GVRET)"
	@echo "  HUSARNET:   $(MODULE_HUSARNET)"
	@echo "  WEBRTC:     $(MODULE_WEBRTC)"
	@echo "  Board:      $(BOARD)"
	@echo "  Port:       $(PORT)"

# Build MicroPython firmware with pyDirect modules
firmware: check-prerequisites
	@echo "🚀 Building MicroPython firmware with pyDirect modules..."
	@echo "  Board: $(BOARD)"
	@echo "  Modules:"
	@[ "$(MODULE_HTTPSERVER)" = "ON" ] && echo "    ✓ httpserver (httpserver, webfiles, wsserver, webrepl)" || true
	@[ "$(MODULE_TWAI)" = "ON" ] && echo "    ✓ twai (TWAI/CAN bus)" || true
	@[ "$(MODULE_USBMODEM)" = "ON" ] && echo "    ✓ usbmodem (USB modem support)" || true
	@[ "$(MODULE_GVRET)" = "ON" ] && echo "    ✓ gvret (GVRET protocol for SavvyCAN)" || true
	@[ "$(MODULE_HUSARNET)" = "ON" ] && echo "    ✓ husarnet (Husarnet P2P VPN)" || true
	@[ "$(MODULE_WEBRTC)" = "ON" ] && echo "    ✓ webrtc (WebRTC DataChannel for browser integration)" || true
	@echo ""
	@cd $(MPY_DIR)/ports/esp32 && \
		source $(ESP_IDF_PATH)/export.sh && \
		[ -f "$(PYDIRECT_DIR)/sdkconfig.override" ] && cp "$(PYDIRECT_DIR)/sdkconfig.override" sdkconfig.override || true && \
		cmake \
			-G Ninja \
			-DMODULE_PYDIRECT_HTTPSERVER=$(MODULE_HTTPSERVER) \
			-DMODULE_PYDIRECT_TWAI=$(MODULE_TWAI) \
			-DMODULE_PYDIRECT_USBMODEM=$(MODULE_USBMODEM) \
			-DMODULE_PYDIRECT_GVRET=$(MODULE_GVRET) \
			-DMODULE_PYDIRECT_HUSARNET=$(MODULE_HUSARNET) \
			-DMODULE_PYDIRECT_WEBRTC=$(MODULE_WEBRTC) \
			-DMICROPY_BOARD=$(BOARD) \
			-DUSER_C_MODULES=$(PYDIRECT_DIR)/micropython.cmake \
			-S . -B build && \
		ninja -C build -j$$(sysctl -n hw.ncpu || nproc)

# Check prerequisites before building
check-prerequisites:
	@if [ ! -d "$(MPY_DIR)" ]; then \
		echo "❌ MicroPython directory not found at $(MPY_DIR)"; \
		echo "   Please clone MicroPython or set MPY_DIR in Makefile"; \
		exit 1; \
	fi
	@if [ ! -d "$(ESP_IDF_PATH)" ]; then \
		echo "❌ ESP-IDF not found at $(ESP_IDF_PATH)"; \
		echo "   Please install ESP-IDF or set ESP_IDF_PATH in Makefile"; \
		exit 1; \
	fi
	@if [ ! -f "$(PYDIRECT_DIR)/micropython.cmake" ]; then \
		echo "❌ pyDirect not found at $(PYDIRECT_DIR)"; \
		exit 1; \
	fi

# Flash firmware to ESP32 (builds first)
flash: firmware
	@echo "📤 Flashing firmware to ESP32..."
	@echo "   Board: $(BOARD)"
	@echo "   Port:  $(PORT)"
	@echo ""
	@cd $(MPY_DIR)/ports/esp32 && \
		source $(ESP_IDF_PATH)/export.sh && \
		ESPPORT=$(PORT) ESPBAUD=$(ESPBAUD) \
		make -C build flash

# Flash only (without rebuilding) - faster for quick reflash
flash-only:
	@echo "📤 Flashing existing firmware to ESP32..."
	@echo "   Port: $(PORT)"
	@cd $(MPY_DIR)/ports/esp32 && \
		source $(ESP_IDF_PATH)/export.sh && \
		ESPPORT=$(PORT) ESPBAUD=$(ESPBAUD) \
		make -C build flash

# Erase flash completely (use after partition table changes)
erase-flash:
	@echo "⚠️  Erasing entire flash memory..."
	@echo "   This will delete all data on the device!"
	@echo "   Waiting 3 seconds... (Ctrl+C to cancel)"
	@sleep 3
	@cd $(MPY_DIR)/ports/esp32 && \
		source $(ESP_IDF_PATH)/export.sh && \
		ESPPORT=$(PORT) ESPBAUD=$(ESPBAUD) \
		esptool.py --chip $(shell echo $(BOARD) | grep -q "S3" && echo "esp32s3" || echo "esp32") \
			--port $(PORT) --baud $(ESPBAUD) erase_flash

# Erase flash and reflash firmware (recommended after partition changes)
flash-clean: erase-flash flash
	@echo "✅ Flash erased and firmware flashed"

# Flash firmware via serial port on dual port adapter (builds first)
# NOTE: idf.py flash always uses esptool.py (serial), not OpenOCD/JTAG
# This uses the serial/UART port of your dual port adapter
flash: firmware
	@echo "📤 Flashing firmware to ESP32-S3 via serial port..."
	@echo "   Board: $(BOARD)"
	@echo "   Serial Port: $(PORT) (dual port adapter UART)"
	@echo "   Note: idf.py flash uses esptool.py (serial), not JTAG"
	@echo "   For true JTAG flashing, use OpenOCD directly"
	@echo ""
	@cd $(MPY_DIR)/ports/esp32 && \
		source $(ESP_IDF_PATH)/export.sh && \
		ESPPORT="$(PORT)" ESPBAUD="$(ESPBAUD)" \
		idf.py flash

# Flash only via serial port (without rebuilding) - faster for quick reflash
flash-only:
	@echo "📤 Flashing existing firmware to ESP32-S3 via serial port..."
	@echo "   Serial Port: $(PORT) (dual port adapter UART)"
	@cd $(MPY_DIR)/ports/esp32 && \
		source $(ESP_IDF_PATH)/export.sh && \
		ESPPORT="$(PORT)" ESPBAUD="$(ESPBAUD)" \
		idf.py flash

# Erase flash (always uses serial - simpler and more reliable)
# Note: Erase is via serial port, flash can be via JTAG
erase-flash: erase-flash
	@echo "✅ Flash erased via serial port"

# Open serial monitor
monitor:
	@echo "📺 Opening serial monitor on $(PORT)..."
	@echo "   Press Ctrl+] to exit"
	@cd $(MPY_DIR)/ports/esp32 && \
		source $(ESP_IDF_PATH)/export.sh && \
		ESPPORT=$(PORT) idf.py monitor

# Clean build artifacts
clean:
	@echo "🧹 Cleaning build artifacts..."
	@if [ -d "$(BUILD_DIR)" ]; then \
		echo "   Removing $(BUILD_DIR)..."; \
		rm -rf $(BUILD_DIR); \
	fi
	@echo "✅ Clean complete"
	@echo ""
	@echo "Note: To clean and rebuild, run: make clean firmware"

# Clean build directory and reconfigure
clean-all: clean
	@echo "🧹 Removing CMake cache..."
	@cd $(MPY_DIR)/ports/esp32 && \
		rm -rf build/CMakeCache.txt build/CMakeFiles

# Show help
help:
	@echo "pyDirect Build System"
	@echo "====================="
	@echo ""
	@echo "Targets:"
	@echo "  firmware         - Build MicroPython firmware with pyDirect modules"
	@echo "  flash            - Build and flash firmware to ESP32 (serial)"
	@echo "  flash-only       - Flash existing firmware without rebuilding (serial)"
	@echo "  flash-jtag       - Build and flash firmware via JTAG/OpenOCD"
	@echo "  flash-jtag-only  - Flash existing firmware via JTAG (no rebuild)"
	@echo "  flash-clean      - Erase flash and reflash (use after partition changes)"
	@echo "  erase-flash      - Erase entire flash memory via serial (WARNING: deletes all data)"
	@echo "  erase-flash-jtag - Erase entire flash memory via JTAG (WARNING: deletes all data)"
	@echo "  monitor          - Open serial monitor to view output"
	@echo "  modules          - Show current module configuration"
	@echo "  clean            - Clean build artifacts"
	@echo "  clean-all        - Clean build artifacts and CMake cache"
	@echo "  help             - Show this help"
	@echo ""
	@echo "Configuration:"
	@echo "  MPY_DIR       = $(MPY_DIR)"
	@echo "  ESP_IDF_PATH  = $(ESP_IDF_PATH)"
	@echo "  PYDIRECT_DIR = $(PYDIRECT_DIR)"
	@echo "  BOARD              = $(BOARD) (default: ESP32_GENERIC_S3)"
	@echo "  PORT               = $(PORT) (serial/UART port for erase/monitor)"
	@echo "  OPENOCD_SCRIPTS    = $(OPENOCD_SCRIPTS) (auto-detected)"
	@echo ""
	@echo "  Dual port adapter ports:"
	@echo "    - Serial/UART: $(PORT) (used for erase-flash, monitor)"
	@echo "    - JTAG: auto-detected by OpenOCD (used for flash-jtag)"
	@echo ""
	@echo "Module Selection:"
	@echo "  MODULE_HTTPSERVER = $(MODULE_HTTPSERVER) (default: ON)"
	@echo "  MODULE_TWAI       = $(MODULE_TWAI) (default: OFF)"
	@echo "  MODULE_USBMODEM   = $(MODULE_USBMODEM) (default: OFF)"
	@echo "  MODULE_GVRET      = $(MODULE_GVRET) (default: OFF)"
	@echo "  MODULE_HUSARNET   = $(MODULE_HUSARNET) (default: OFF)"
	@echo "  MODULE_ALL        = $(MODULE_ALL) (enable all modules)"
	@echo ""
	@echo "Examples:"
	@echo "  make firmware                      # Build with defaults (HTTPSERVER only)"
	@echo "  make firmware MODULE_ALL=ON        # Build with ALL modules enabled"
	@echo "  make firmware MODULE_TWAI=ON      # Build with HTTPSERVER + TWAI"
	@echo "  make firmware MODULE_GVRET=ON     # Build with HTTPSERVER + GVRET"
	@echo "  make flash BOARD=GENERIC           # Build and flash for ESP32 (generic)"
	@echo "  make flash-only                   # Quick reflash without rebuilding"
	@echo "  make flash-jtag                   # Flash via JTAG adapter"
	@echo "  make flash-jtag OPENOCD_CFG=interface/ftdi/ft4232h.cfg  # Custom JTAG config"
	@echo "  make monitor                      # Open serial monitor"
	@echo ""
	@echo "Enable multiple modules:"
	@echo "  make firmware MODULE_ALL=ON                         # All modules"
	@echo "  make firmware MODULE_HTTPSERVER=ON MODULE_TWAI=ON   # Specific modules"
	@echo "  make firmware MODULE_HTTPSERVER=ON MODULE_GVRET=ON  # HTTPSERVER + GVRET"
	@echo "  make firmware MODULE_HUSARNET=ON                    # HTTPSERVER + Husarnet VPN"
	@echo ""
	@echo "Prerequisites:"
	@echo "  - ESP-IDF v5.0+ installed and configured"
	@echo "  - MicroPython source code"
	@echo "  - ESP32 device connected (for flash/monitor)"
	@echo ""
	@echo "JTAG Flashing (ESP32-S3):"
	@echo "  make flash-jtag                    # Flash via JTAG (uses OpenOCD via idf.py)"
	@echo "  make flash-jtag-only              # Quick reflash via JTAG"
	@echo "  make erase-flash-jtag             # Erase flash (uses serial - simpler)"
	@echo ""
	@echo "  Note: Flash uses JTAG/OpenOCD, erase uses serial (esptool.py)"
	@echo "  idf.py will use OpenOCD automatically when ESPPORT is not set"
	@echo "  OpenOCD scripts: $(OPENOCD_SCRIPTS)"
	@echo ""
	@echo "Troubleshooting:"
	@echo "  If serial flash fails, try: make erase-flash flash"
	@echo "  If JTAG flash fails, try: make erase-flash-jtag flash-jtag"
	@echo "  If you get CMake generator mismatch error:"
	@echo "    make clean  # Then rebuild"
	@echo "  To change port: make flash PORT=/dev/ttyUSB0"
	@echo "  To change board: make flash BOARD=GENERIC_C3"
	@echo "  To use JTAG: make flash-jtag"
