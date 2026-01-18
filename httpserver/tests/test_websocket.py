"""
WebSocket server functionality tests using WebREPL WBP.

These tests verify WebSocket server functionality:
- Server start/stop
- Connection handling
- Message processing
- Keep-alive functionality

Usage:
    python test_websocket.py
    # Or with custom URL/password:
    python test_websocket.py --url ws://192.168.4.1/WebREPL --password secret
"""

import asyncio
import argparse
import sys
from webrepl_client import WebREPLTestClient
import test_helpers
from test_helpers import assert_websocket_server


async def test_websocket_start_stop(client: WebREPLTestClient):
    """Test WebSocket server can start and stop."""
    print("\n" + "=" * 60)
    print("Test: WebSocket Server Start/Stop")
    print("=" * 60)
    
    await assert_websocket_server(client)


async def test_websocket_with_http(client: WebREPLTestClient):
    """Test WebSocket server works with HTTP server."""
    print("\n" + "=" * 60)
    print("Test: WebSocket + HTTP Server")
    print("=" * 60)
    
    code = """
import httpserver
import wsserver
import time

# Start HTTP server first
assert httpserver.start(8080) == True
time.sleep(0.5)

# Start WebSocket server
assert wsserver.start() == True
time.sleep(0.5)

# Both should be running
assert wsserver.is_running() == True

# Stop WebSocket
wsserver.stop()
time.sleep(0.5)

# Stop HTTP
httpserver.stop()
"""
    
    async with test_helpers.TestAssertion(client, "WebSocket + HTTP integration"):
        result = await client.run_test(code)
        if result.status != 'pass':
            raise AssertionError(result.error or result.message)


async def test_websocket_configuration(client: WebREPLTestClient):
    """Test WebSocket server configuration options."""
    print("\n" + "=" * 60)
    print("Test: WebSocket Configuration")
    print("=" * 60)
    
    code = """
import httpserver
import wsserver
import time

httpserver.start(8080)

# Test with custom path
assert wsserver.start("/custom/ws") == True
time.sleep(0.5)
assert wsserver.is_running() == True
wsserver.stop()

# Test with ping interval
assert wsserver.start("/ws", ping_interval=30) == True
time.sleep(0.5)
wsserver.stop()

httpserver.stop()
"""
    
    async with test_helpers.TestAssertion(client, "WebSocket configuration"):
        result = await client.run_test(code)
        if result.status != 'pass':
            raise AssertionError(result.error or result.message)


async def test_websocket_multiple_starts(client: WebREPLTestClient):
    """Test that multiple starts are handled gracefully."""
    print("\n" + "=" * 60)
    print("Test: Multiple WebSocket Starts")
    print("=" * 60)
    
    code = """
import httpserver
import wsserver
import time

httpserver.start(8080)

# Start first time
assert wsserver.start() == True
time.sleep(0.5)

# Try to start again (should handle gracefully)
try:
    wsserver.start()
except:
    pass  # Exception is acceptable

# Should still be running
assert wsserver.is_running() == True

wsserver.stop()
httpserver.stop()
"""
    
    async with test_helpers.TestAssertion(client, "Multiple starts handling"):
        result = await client.run_test(code)
        if result.status != 'pass':
            raise AssertionError(result.error or result.message)


async def run_all_tests(url: str, password: str, ssl_verify: bool = True):
    """Run all WebSocket tests."""
    client = WebREPLTestClient(url, password, ssl_verify=ssl_verify)
    
    try:
        print("Connecting to WebREPL...")
        await client.connect()
        print("✓ Connected and authenticated")
        
        await test_websocket_start_stop(client)
        await test_websocket_with_http(client)
        await test_websocket_configuration(client)
        await test_websocket_multiple_starts(client)
        
        print("\n" + "=" * 60)
        print("All WebSocket tests passed!")
        print("=" * 60)
        
    except Exception as e:
        print(f"\n✗ Test failed: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
    finally:
        await client.close()


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(description='WebSocket server tests')
    parser.add_argument(
        '--url',
        default='ws://192.168.4.1/WebREPL',
        help='WebREPL WebSocket URL (ws:// or wss://)'
    )
    parser.add_argument(
        '--password',
        default='secret',
        help='WebREPL password'
    )
    parser.add_argument(
        '--no-ssl-verify',
        action='store_true',
        help='Disable SSL certificate verification (for self-signed certs)'
    )
    
    args = parser.parse_args()
    
    asyncio.run(run_all_tests(args.url, args.password, ssl_verify=not args.no_ssl_verify))


if __name__ == "__main__":
    main()
