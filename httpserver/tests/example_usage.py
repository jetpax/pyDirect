"""
Example usage of WebREPL WBP test client.

This demonstrates how to use the test client library to write custom tests.
"""

import asyncio
from webrepl_client import WebREPLTestClient
import test_helpers


async def example_basic_test():
    """Basic example: Connect and run a simple test."""
    client = WebREPLTestClient('ws://192.168.1.154/WebREPL', 'rtyu4567')
    
    try:
        print("Connecting...")
        await client.connect()
        print("✓ Connected")
        
        # Execute simple code
        result = await client.execute_m2m("print('Hello from device!')")
        print(f"Result: {result}")
        
        # Run a test
        test_result = await client.run_test("assert 1 + 1 == 2")
        print(f"Test status: {test_result.status}")
        
    finally:
        await client.close()


async def example_http_test():
    """Example: Test HTTP server using helpers."""
    client = WebREPLTestClient('ws://192.168.4.1/WebREPL', 'secret')
    
    try:
        await client.connect()
        
        # Use test helper
        await test_helpers.assert_http_server_start_stop(client, port=8080)
        
        # Custom test with context manager
        async with test_helpers.TestAssertion(client, "Custom HTTP test"):
            code = """
import httpserver
httpserver.start(8080)
def handler(uri):
    return "<html><body>Test</body></html>"
httpserver.on("/test", handler)
httpserver.stop()
"""
            result = await client.run_test(code)
            assert result.status == 'pass'
        
    finally:
        await client.close()


async def example_terminal_output():
    """Example: Capture terminal output."""
    client = WebREPLTestClient('ws://192.168.4.1/WebREPL', 'secret')
    
    try:
        await client.connect()
        
        # Execute on terminal channel and collect output
        output = await client.execute_terminal("for i in range(3): print(i)")
        print("Terminal output:")
        for line in output:
            print(f"  {line}")
        
    finally:
        await client.close()


async def example_error_handling():
    """Example: Handle test failures gracefully."""
    client = WebREPLTestClient('ws://192.168.4.1/WebREPL', 'secret')
    
    try:
        await client.connect()
        
        # Test that should fail
        result = await client.run_test("assert 1 + 1 == 3")
        
        if result.status == 'fail':
            print(f"Test failed as expected: {result.error}")
        elif result.status == 'error':
            print(f"Test error: {result.error}")
        else:
            print("Unexpected: test passed!")
        
    finally:
        await client.close()


if __name__ == "__main__":
    print("WebREPL WBP Test Client Examples")
    print("=" * 60)
    
    # Run examples (comment out ones you don't want to run)
    asyncio.run(example_basic_test())
    # asyncio.run(example_http_test())
    # asyncio.run(example_terminal_output())
    # asyncio.run(example_error_handling())
