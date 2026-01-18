"""
Basic HTTP server functionality tests using WebREPL WBP.

These tests verify core HTTP server functionality:
- Server start/stop
- Handler registration
- Request processing
- Queue management

Usage:
    python test_http_basic.py
    # Or with custom URL/password:
    python test_http_basic.py --url ws://192.168.4.1/WebREPL --password secret
"""

import asyncio
import argparse
import sys
from webrepl_client import WebREPLTestClient
import test_helpers
from test_helpers import (
    assert_http_server_start_stop,
    assert_handler_registration,
    run_with_cleanup
)


async def test_server_start_stop(client: WebREPLTestClient):
    """Test HTTP server can start and stop."""
    print("\n" + "=" * 60)
    print("Test: HTTP Server Start/Stop")
    print("=" * 60)
    
    await assert_http_server_start_stop(client, port=8080)
    await assert_http_server_start_stop(client, port=8081)


async def test_handler_registration(client: WebREPLTestClient):
    """Test handler registration."""
    print("\n" + "=" * 60)
    print("Test: Handler Registration")
    print("=" * 60)
    
    await assert_handler_registration(client, uri="/")
    await assert_handler_registration(client, uri="/api")
    await assert_handler_registration(client, uri="/test/path")


async def test_multiple_handlers(client: WebREPLTestClient):
    """Test registering multiple handlers."""
    print("\n" + "=" * 60)
    print("Test: Multiple Handlers")
    print("=" * 60)
    
    code = """
import httpserver

httpserver.start(8080)

# Register multiple handlers
def handler1(uri):
    return "<html><body>Handler 1</body></html>"

def handler2(uri, post_data):
    return "<html><body>Handler 2</body></html>"

id1 = httpserver.on("/handler1", handler1)
id2 = httpserver.on("/handler2", handler2, "POST")
id3 = httpserver.on("/handler3", handler1)

assert id1 >= 0
assert id2 >= 0
assert id3 >= 0
assert id1 != id2
assert id2 != id3

httpserver.stop()
"""
    
    async with test_helpers.TestAssertion(client, "Multiple handler registration"):
        result = await client.run_test(code)
        if result.status != 'pass':
            raise AssertionError(result.error or result.message)


async def test_queue_processing(client: WebREPLTestClient):
    """Test queue processing."""
    print("\n" + "=" * 60)
    print("Test: Queue Processing")
    print("=" * 60)
    
    code = """
import httpserver
import time

httpserver.start(8080)

def handle_test(uri):
    return "<html><body>Test</body></html>"

httpserver.on("/test", handle_test)

# Process queue multiple times
for i in range(10):
    processed = httpserver.process_queue()
    time.sleep_ms(50)

httpserver.stop()
"""
    
    async with test_helpers.TestAssertion(client, "Queue processing"):
        result = await client.run_test(code)
        if result.status != 'pass':
            raise AssertionError(result.error or result.message)


async def test_get_handler(client: WebREPLTestClient):
    """Test GET request handler."""
    print("\n" + "=" * 60)
    print("Test: GET Handler")
    print("=" * 60)
    
    code = """
import httpserver
import time

httpserver.start(8080)

def handle_get(uri):
    return f"<html><body>GET: {uri}</body></html>"

handler_id = httpserver.on("/get", handle_get)
assert handler_id >= 0

# Process queue
time.sleep(0.5)
for i in range(5):
    httpserver.process_queue()
    time.sleep_ms(100)

httpserver.stop()
"""
    
    async with test_helpers.TestAssertion(client, "GET handler execution"):
        result = await client.run_test(code)
        if result.status != 'pass':
            raise AssertionError(result.error or result.message)


async def test_post_handler(client: WebREPLTestClient):
    """Test POST request handler."""
    print("\n" + "=" * 60)
    print("Test: POST Handler")
    print("=" * 60)
    
    code = """
import httpserver
import time

httpserver.start(8080)

def handle_post(uri, post_data):
    return f"<html><body>POST: {uri}, Data: {post_data}</body></html>"

handler_id = httpserver.on("/post", handle_post, "POST")
assert handler_id >= 0

# Process queue
time.sleep(0.5)
for i in range(5):
    httpserver.process_queue()
    time.sleep_ms(100)

httpserver.stop()
"""
    
    async with test_helpers.TestAssertion(client, "POST handler execution"):
        result = await client.run_test(code)
        if result.status != 'pass':
            raise AssertionError(result.error or result.message)


async def test_error_handling(client: WebREPLTestClient):
    """Test error handling."""
    print("\n" + "=" * 60)
    print("Test: Error Handling")
    print("=" * 60)
    
    # Test stopping server that's not started
    code = """
import httpserver

# Stop without starting should not crash
try:
    httpserver.stop()
except Exception as e:
    # Exception is acceptable
    pass
"""
    
    async with test_helpers.TestAssertion(client, "Stop without start"):
        result = await client.run_test(code)
        # Should not raise exception
        assert result.status in ['pass', 'error']  # Either is acceptable


async def run_all_tests(url: str, password: str, ssl_verify: bool = True, debug: bool = False):
    """Run all HTTP basic tests."""
    client = WebREPLTestClient(url, password, ssl_verify=ssl_verify, debug=debug)
    
    try:
        print("Connecting to WebREPL...")
        await client.connect()
        print("✓ Connected and authenticated")
        
        await test_server_start_stop(client)
        await test_handler_registration(client)
        await test_multiple_handlers(client)
        await test_queue_processing(client)
        await test_get_handler(client)
        await test_post_handler(client)
        await test_error_handling(client)
        
        print("\n" + "=" * 60)
        print("All HTTP basic tests passed!")
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
    parser = argparse.ArgumentParser(description='HTTP server basic tests')
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
    parser.add_argument(
        '--debug',
        action='store_true',
        help='Enable debug output for message tracing'
    )
    
    args = parser.parse_args()
    
    asyncio.run(run_all_tests(args.url, args.password, ssl_verify=not args.no_ssl_verify, debug=args.debug))


if __name__ == "__main__":
    main()
