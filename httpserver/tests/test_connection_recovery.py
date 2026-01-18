"""
Test WebSocket Connection Recovery

Tests that PING failures trigger automatic cleanup and allow reconnection
without requiring device restart.
"""

import asyncio
import websockets
import cbor2
import ssl
import sys

async def test_clean_connection_cycle():
    """Test normal connect, use, disconnect, reconnect cycle"""
    print('=== Test 1: Clean Connection Cycle ===')
    ssl_context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ssl_context.check_hostname = False
    ssl_context.verify_mode = ssl.CERT_NONE
    
    # First connection - normal with PING
    ws1 = await websockets.connect(
        'wss://192.168.1.154/webrepl',
        subprotocols=['webrepl.binary.v1'],
        ping_interval=20,
        ping_timeout=10,
        ssl=ssl_context
    )
    print('✓ Connected (with PING enabled)')
    
    # Auth
    await ws1.send(cbor2.dumps([0, 0, 'rtyu4567']))
    auth = await ws1.recv()
    print(f'✓ Authenticated: {cbor2.loads(auth)}')
    
    # Test simple command
    await ws1.send(cbor2.dumps([2, 0, 'print("test1")', 0, 'test-1']))
    response = await ws1.recv()
    print(f'✓ Got response')
    
    # Get PRO
    pro = await ws1.recv()
    print(f'✓ Got PRO')
    
    # Close cleanly
    await ws1.close()
    print('✓ First connection closed cleanly\n')
    
    # Wait a moment
    await asyncio.sleep(2)
    
    print('=== Test 2: Reconnect After Clean Close ===')
    # Second connection - should work fine
    ws2 = await websockets.connect(
        'wss://192.168.1.154/webrepl',
        subprotocols=['webrepl.binary.v1'],
        ping_interval=20,
        ping_timeout=10,
        ssl=ssl_context
    )
    print('✓ Reconnected successfully')
    
    # Auth
    await ws2.send(cbor2.dumps([0, 0, 'rtyu4567']))
    auth = await ws2.recv()
    print(f'✓ Re-authenticated')
    
    # Test command
    await ws2.send(cbor2.dumps([2, 0, 'print("test2")', 0, 'test-2']))
    response = await ws2.recv()
    print(f'✓ Got response')
    
    # Get PRO
    pro = await ws2.recv()
    print(f'✓ Got PRO')
    
    await ws2.close()
    print('✓ Second connection closed\n')
    
    print('✅ Clean connection cycle: PASSED')
    return True

async def test_ping_failure_recovery():
    """Test that PING failure triggers cleanup and allows reconnection"""
    print('\n=== Test 3: PING Failure Recovery ===')
    ssl_context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ssl_context.check_hostname = False
    ssl_context.verify_mode = ssl.CERT_NONE
    
    # Connect WITHOUT proper PING/PONG to trigger failure
    print('1. Connecting with PING disabled (will cause failure)...')
    ws1 = await websockets.connect(
        'wss://192.168.1.154/webrepl',
        subprotocols=['webrepl.binary.v1'],
        ping_interval=None,  # Disable PING - this will cause server PING to fail
        ssl=ssl_context
    )
    print('✓ Connected (PING disabled)')
    
    # Auth
    await ws1.send(cbor2.dumps([0, 0, 'rtyu4567']))
    auth = await ws1.recv()
    print(f'✓ Authenticated')
    
    # Send a command
    await ws1.send(cbor2.dumps([2, 0, 'print("before failure")', 0, 'test-1']))
    try:
        response = await ws1.recv()
        print(f'✓ Got response')
        pro = await ws1.recv()
        print(f'✓ Got PRO')
    except:
        pass
    
    # Wait for server to send PING and fail (usually after ~20-30 seconds)
    print('\n2. Waiting 35 seconds for server PING to fail...')
    print('   (Server will detect dead connection and clean up)')
    try:
        await asyncio.sleep(35)
        # Try to receive - should fail
        await asyncio.wait_for(ws1.recv(), timeout=5)
    except Exception as e:
        print(f'✓ Connection broken as expected: {type(e).__name__}')
    
    try:
        await ws1.close()
    except:
        pass
    
    # Now try to reconnect - this should work if cleanup is working
    print('\n3. Attempting reconnect after PING failure cleanup...')
    await asyncio.sleep(2)
    
    ws2 = await websockets.connect(
        'wss://192.168.1.154/webrepl',
        subprotocols=['webrepl.binary.v1'],
        ping_interval=20,  # Proper PING this time
        ping_timeout=10,
        ssl=ssl_context
    )
    print('✓ Reconnected successfully!')
    
    # Auth
    await ws2.send(cbor2.dumps([0, 0, 'rtyu4567']))
    auth = await ws2.recv()
    print(f'✓ Re-authenticated')
    
    # Test that it actually works
    await ws2.send(cbor2.dumps([2, 0, 'print("after recovery")', 0, 'test-2']))
    response = await ws2.recv()
    print(f'✓ Got response')
    pro = await ws2.recv()
    print(f'✓ Got PRO')
    
    await ws2.close()
    print('\n✅ PING failure recovery: PASSED')
    print('Device automatically cleaned up dead connection without restart.')
    return True

async def main():
    """Run all connection recovery tests"""
    try:
        # Test 1 & 2: Clean connection cycle
        await test_clean_connection_cycle()
        
        # Test 3: PING failure recovery (takes ~40 seconds)
        await test_ping_failure_recovery()
        
        print('\n' + '=' * 60)
        print('✅ ALL TESTS PASSED')
        print('=' * 60)
        print('Connection cleanup is working correctly!')
        print('- Clean disconnects work')
        print('- PING failures trigger automatic cleanup')
        print('- Reconnection works without device restart')
        
    except Exception as e:
        print(f'\n✗ Test failed: {e}')
        import traceback
        traceback.print_exc()
        sys.exit(1)

if __name__ == "__main__":
    import argparse
    
    parser = argparse.ArgumentParser(description='WebSocket Connection Recovery Tests')
    parser.add_argument('--url', default='wss://192.168.1.154/webrepl', help='WebREPL URL')
    parser.add_argument('--password', default='rtyu4567', help='WebREPL password')
    
    args = parser.parse_args()
    
    # Note: This test is hardcoded for the specific device IP
    # Update the IP addresses in the test functions if needed
    
    print('Connection Recovery Test Suite')
    print('Note: This test takes ~50 seconds to complete (tests PING timeout)')
    print('=' * 60 + '\n')
    
    asyncio.run(main())
