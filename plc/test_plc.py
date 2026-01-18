"""
PLC Module Test Script
======================

Test the PLC module for CCS/NACS charging communication.

Hardware Setup:
- ESP32-P4 with Ethernet connected to TP-Link TL-PA4010P
- TP-Link configured in EVSE mode (PIB settings)
- Optionally: CP PWM output to CCS connector

Usage:
>>> import test_plc
>>> test_plc.test_modem()      # Check modem present
>>> test_plc.test_slac()       # Start SLAC and wait for car
"""

import plc
import time

def test_modem():
    """Test modem detection via GET_SW.REQ"""
    print("Testing modem detection...")
    info = plc.get_modem_info()
    if info:
        print(f"✅ Modem found:")
        for key, value in info.items():
            print(f"   {key}: {value}")
        return True
    else:
        print("❌ Modem not found - check Ethernet connection")
        return False

def test_slac():
    """Start SLAC and wait for car connection"""
    import os
    
    print("Testing SLAC...")
    
    # Check modem first
    if not test_modem():
        return False
    
    # Generate random NID/NMK
    nid = os.urandom(7)
    nmk = os.urandom(16)
    print(f"NID: {nid.hex()}")
    print(f"NMK: {nmk.hex()}")
    
    plc.set_key(nid, nmk)
    
    # Set callback
    slac_complete = [False]
    car_mac = [None]
    
    def on_slac_complete(mac):
        print(f"🎉 SLAC COMPLETE! Car MAC: {mac}")
        slac_complete[0] = True
        car_mac[0] = mac
    
    plc.set_callback(on_slac_complete)
    
    # Start EVSE mode
    print("Starting SLAC responder...")
    if not plc.start_evse():
        print("❌ Failed to start EVSE mode")
        return False
    
    print("✅ SLAC responder started")
    print("Waiting for car connection (Ctrl+C to abort)...")
    
    try:
        while not slac_complete[0]:
            status = plc.get_status()
            print(f"  State: {status['state']}, RX: {status['rx_count']}, TX: {status['tx_count']}")
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nAborted by user")
    
    plc.stop()
    
    if slac_complete[0]:
        print(f"✅ SLAC succeeded! Car MAC: {car_mac[0]}")
        return True
    else:
        status = plc.get_status()
        print(f"❌ SLAC did not complete. Final state: {status['state']}")
        return False

def test_status():
    """Print current PLC status"""
    status = plc.get_status()
    print("PLC Status:")
    for key, value in status.items():
        print(f"  {key}: {value}")
    return status

def start_cp_pwm(pin=4):
    """Start CP PWM at 5% - call this before SLAC"""
    from machine import PWM, Pin
    cp = PWM(Pin(pin), freq=1000, duty_u16=int(65535 * 0.05))
    print(f"✅ CP PWM started on GPIO{pin} at 5% (1kHz)")
    return cp

if __name__ == "__main__":
    test_modem()
