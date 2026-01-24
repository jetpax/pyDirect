#!/usr/bin/env python3
import os
import sys

def update_idf_component_yml(path):
    print(f"🔧 Updating {path}...")
    try:
        with open(path, 'r') as f:
            lines = f.readlines()
    except FileNotFoundError:
        print(f"❌ File not found: {path}")
        sys.exit(1)

    # Dependencies to insert
    deps = [
        "  # pyDirect dependencies (injected by CI)\n",
        '  joltwallet/littlefs: "^1.0.0"\n',
        '  husarnet/esp_husarnet: "^0.0.15"\n',
        '  espressif/cbor: "^0.6.0"\n',
        '  espressif/esp_peer: "^1.2.2"\n',
        '  espressif/iot_usbh_modem:\n',
        '    version: "^2.0.0"\n',
        '    rules:\n',
        '      - if: "target in [esp32s2, esp32s3, esp32p4]"\n'
    ]

    new_lines = []
    inserted = False
    
    # Simple insertion strategy: Insert before the 'idf:' line
    for line in lines:
        if line.strip().startswith('idf:') and not inserted:
            new_lines.extend(deps)
            inserted = True
        new_lines.append(line)

    if not inserted:
        # Fallback: append if 'idf:' not found (unlikely but safe)
        print("⚠️  'idf:' key not found, appending dependencies...")
        new_lines.extend(deps)

    with open(path, 'w') as f:
        f.writelines(new_lines)
    print("✅ Inserted managed dependencies into idf_component.yml")

def update_esp32_common_cmake(path):
    print(f"🔧 Updating {path}...")
    try:
        with open(path, 'r') as f:
            content = f.read()
    except FileNotFoundError:
        print(f"❌ File not found: {path}")
        sys.exit(1)

    # Replace esp_event with esp_event + http servers
    if 'esp_http_server' not in content:
        content = content.replace(
            '    esp_event',
            '    esp_event\n    esp_http_server\n    esp_https_server'
        )
        with open(path, 'w') as f:
            f.write(content)
        print("✅ Added esp_http_server/esp_https_server to IDF_COMPONENTS")
    else:
        print("ℹ️  esp_http_server already present")

def apply_mspi_workaround():
    """Apply ESP-IDF v5.5.1 workaround for mspi_timing_tuning include path"""
    mspi_file = "/opt/esp/idf/components/esp_hw_support/mspi_timing_tuning/port/esp32s3/mspi_timing_config.c"
    
    if not os.path.exists(mspi_file):
        print("ℹ️  mspi_timing_config.c not found (different IDF version?), skipping workaround")
        return
    
    print(f"🔧 Applying ESP-IDF v5.5.1 mspi_timing_tuning workaround...")
    try:
        with open(mspi_file, 'r') as f:
            content = f.read()
        
        old_include = '#include "mspi_timing_tuning_configs.h"'
        new_include = '#include "../mspi_timing_tuning_configs.h"'
        
        if old_include in content:
            content = content.replace(old_include, new_include)
            with open(mspi_file, 'w') as f:
                f.write(content)
            print("✅ Applied mspi_timing_tuning include path workaround")
        elif new_include in content:
            print("ℹ️  mspi workaround already applied")
        else:
            print("⚠️  Could not find mspi include to patch")
    except Exception as e:
        print(f"⚠️  Could not apply mspi workaround: {e}")

def fix_mspi_include_dir(cmake_path):
    """Remove missing mspi include directory from esp32_common.cmake for ESP-IDF v5.5.x"""
    mspi_include_dir = "/opt/esp/idf/components/esp_hw_support/mspi_timing_tuning/port/esp32s3/include"
    
    if os.path.exists(mspi_include_dir):
        print("ℹ️  mspi include directory exists, no cmake fix needed")
        return
    
    print(f"🔧 Fixing missing mspi include directory in esp32_common.cmake...")
    try:
        with open(cmake_path, 'r') as f:
            content = f.read()
        
        # The problematic line adds this include path - we need to remove or guard it
        old_line = '${IDF_PATH}/components/esp_hw_support/mspi_timing_tuning/port/${IDF_TARGET}/include'
        
        if old_line in content:
            # Comment out the line instead of removing it
            content = content.replace(
                old_line,
                '# ${IDF_PATH}/components/esp_hw_support/mspi_timing_tuning/port/${IDF_TARGET}/include  # Removed for IDF v5.5.x'
            )
            with open(cmake_path, 'w') as f:
                f.write(content)
            print("✅ Commented out missing mspi include directory")
        else:
            print("ℹ️  mspi include path not found in cmake (may already be fixed)")
    except Exception as e:
        print(f"⚠️  Could not fix mspi include directory: {e}")

def patch_dupterm_slots(mpconfigport_path):
    """Patch mpconfigport.h to enable 3 dupterm slots.
    
    pyDirect WebREPL binary protocol requires 3 slots:
      - Slot 0: UART console
      - Slot 1: WebRTC (webrepl_rtc)
      - Slot 2: WebSocket (webrepl_binary)
    
    Default ESP32 MicroPython has only 2 slots.
    """
    print(f"🔧 Patching {mpconfigport_path} for 3 dupterm slots...")
    
    try:
        with open(mpconfigport_path, 'r') as f:
            content = f.read()
        
        # Look for the existing MICROPY_PY_OS_DUPTERM definition
        old_pattern = '#define MICROPY_PY_OS_DUPTERM               (2)'
        new_value = '#define MICROPY_PY_OS_DUPTERM               (3)  // pyDirect: 3 slots for UART + WebRTC + WebSocket'
        
        if old_pattern in content:
            content = content.replace(old_pattern, new_value)
            with open(mpconfigport_path, 'w') as f:
                f.write(content)
            print("✅ Patched MICROPY_PY_OS_DUPTERM from 2 to 3")
        elif '(3)' in content and 'MICROPY_PY_OS_DUPTERM' in content:
            print("ℹ️  MICROPY_PY_OS_DUPTERM already set to 3")
        else:
            print("⚠️  Could not find MICROPY_PY_OS_DUPTERM pattern to patch")
    except FileNotFoundError:
        print(f"❌ File not found: {mpconfigport_path}")
        sys.exit(1)
    except Exception as e:
        print(f"⚠️  Could not patch dupterm slots: {e}")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 ci_update_dependencies.py <idf_component.yml> <esp32_common.cmake> [mpconfigport.h]")
        sys.exit(1)

    idf_comp_path = sys.argv[1]
    cmake_path = sys.argv[2]
    mpconfigport_path = sys.argv[3] if len(sys.argv) > 3 else None
    
    update_idf_component_yml(idf_comp_path)
    update_esp32_common_cmake(cmake_path)
    apply_mspi_workaround()
    fix_mspi_include_dir(cmake_path)
    
    if mpconfigport_path:
        patch_dupterm_slots(mpconfigport_path)
