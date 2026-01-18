# boot.py - Minimal boot configuration for pyDirect
# This file runs on every boot (including wake-boot from deepsleep)

import esp
esp.osdebug(None)  # Disable vendor O/S debugging messages

# Optional: Disable WiFi on boot (save power if using Ethernet)
# import network
# network.WLAN(network.STA_IF).active(False)
# network.WLAN(network.AP_IF).active(False)

# Optional: Set CPU frequency
# import machine
# machine.freq(240000000)  # 240MHz

print("pyDirect boot complete")
