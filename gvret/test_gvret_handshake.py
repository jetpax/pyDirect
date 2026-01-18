import socket
import time
import sys

# Configuration
DEVICE_IP = '192.168.1.32' # Change this to your device IP
PORT = 23

def test_handshake():
    print(f"Connecting to {DEVICE_IP}:{PORT}...")
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2.0)
        s.connect((DEVICE_IP, PORT))
        print("Connected.")

        # Test 1: GET_DEV_INFO (0xF1 0x07)
        print("Sending GET_DEV_INFO...")
        s.send(bytes([0xF1, 0x07]))
        resp = s.recv(1024)
        print(f"Response: {resp.hex()}")
        if resp.startswith(b'\xf1\x07'):
            print("PASS: GET_DEV_INFO response valid")
        else:
            print("FAIL: Invalid GET_DEV_INFO response")

        # Test 2: KEEPALIVE (0xF1 0x09)
        print("Sending KEEPALIVE...")
        s.send(bytes([0xF1, 0x09]))
        resp = s.recv(1024)
        print(f"Response: {resp.hex()}")
        if resp == b'\xf1\x09\xde\xad':
            print("PASS: KEEPALIVE response valid")
        else:
            print("FAIL: Invalid KEEPALIVE response")

        # Test 3: GET_CANBUS_PARAMS (0xF1 0x06)
        print("Sending GET_CANBUS_PARAMS...")
        s.send(bytes([0xF1, 0x06]))
        resp = s.recv(1024)
        print(f"Response: {resp.hex()}")
        if resp.startswith(b'\xf1\x06'):
            print("PASS: GET_CANBUS_PARAMS response valid")
        else:
            print("FAIL: Invalid GET_CANBUS_PARAMS response")

        s.close()
        print("Done.")

    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    if len(sys.argv) > 1:
        DEVICE_IP = sys.argv[1]
    test_handshake()
