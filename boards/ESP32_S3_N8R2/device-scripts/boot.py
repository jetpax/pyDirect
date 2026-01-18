# boot.py - Boot configuration for pyDirect with WiFi onboarding
# This file runs on every boot (including wake-boot from deepsleep)

import esp
esp.osdebug(None)  # Disable vendor O/S debugging messages

# Disable WiFi on boot - main.py will handle WiFi setup
import network
network.WLAN(network.STA_IF).active(False)
network.WLAN(network.AP_IF).active(False)

# Optional: Set CPU frequency for better performance
# import machine
# machine.freq(240000000)  # 240MHz

print("pyDirect boot complete - WiFi onboarding enabled")
