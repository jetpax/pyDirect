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

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 ci_update_dependencies.py <idf_component.yml> <esp32_common.cmake>")
        sys.exit(1)

    idf_comp_path = sys.argv[1]
    cmake_path = sys.argv[2]
    
    update_idf_component_yml(idf_comp_path)
    update_esp32_common_cmake(cmake_path)
