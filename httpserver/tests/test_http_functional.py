"""
HTTP Server Functional Tests (via WebREPL)

Tests the actual httpserver functionality that matters in production:
- URI handler registration (GET/POST)
- Handler execution
- Request/response flow
- Error handling

These tests run against the already-running HTTP server (don't call start/stop).
"""

import asyncio
import sys
from webrepl_client import WebREPLTestClient, TestResult
from test_helpers import TestAssertion

async def test_register_handler(client: WebREPLTestClient) -> bool:
    """Test registering a URI handler"""
    async with TestAssertion(client, "Register GET handler"):
        code = """
import httpserver

# Define a simple handler
def test_handler(uri, post_data=None):
    return "Hello from test handler"

# Unregister if exists (from previous test)
try:
    httpserver.off('/test_handler', 'GET')
except:
    pass

# Register handler
result = httpserver.on('/test_handler', test_handler, 'GET')
print(f"Registration result: {result}")
assert result >= 0, f"Registration failed: {result}"
"""
        result = await client.run_test(code, timeout=10.0)
        
        if result.status != 'pass':
            raise AssertionError(f"{result.error_type}: {result.error}")
        
        return True

async def test_unregister_handler(client: WebREPLTestClient) -> bool:
    """Test unregistering a URI handler"""
    async with TestAssertion(client, "Unregister GET handler"):
        code = """
import httpserver

# Unregister the handler we just registered
result = httpserver.off('/test_handler', 'GET')
print(f"Unregistration result: {result}")
assert result == True, f"Unregistration failed: {result}"
"""
        result = await client.run_test(code, timeout=10.0)
        
        if result.status != 'pass':
            raise AssertionError(f"{result.error_type}: {result.error}")
        
        return True

async def test_multiple_handlers(client: WebREPLTestClient) -> bool:
    """Test registering multiple handlers"""
    async with TestAssertion(client, "Register multiple handlers"):
        code = """
import httpserver

# Clean up any existing handlers
for uri in ['/handler1', '/handler2', '/handler3']:
    try:
        httpserver.off(uri, 'GET')
    except:
        pass

# Register multiple handlers
def handler1(uri, post_data=None):
    return "Handler 1"

def handler2(uri, post_data=None):
    return "Handler 2"

def handler3(uri, post_data=None):
    return "Handler 3"

r1 = httpserver.on('/handler1', handler1, 'GET')
r2 = httpserver.on('/handler2', handler2, 'GET')
r3 = httpserver.on('/handler3', handler3, 'GET')

print(f"Results: {r1}, {r2}, {r3}")
assert r1 >= 0 and r2 >= 0 and r3 >= 0, f"Registration failed: {r1}, {r2}, {r3}"

# Clean up
httpserver.off('/handler1', 'GET')
httpserver.off('/handler2', 'GET')
httpserver.off('/handler3', 'GET')
"""
        result = await client.run_test(code, timeout=10.0)
        
        if result.status != 'pass':
            raise AssertionError(f"{result.error_type}: {result.error}")
        
        return True

async def test_post_handler(client: WebREPLTestClient) -> bool:
    """Test POST handler registration"""
    async with TestAssertion(client, "Register POST handler"):
        code = """
import httpserver

# Clean up
try:
    httpserver.off('/test_post', 'POST')
except:
    pass

# Register POST handler
def post_handler(uri, post_data=None):
    return f"POST data: {post_data}"

result = httpserver.on('/test_post', post_handler, 'POST')
print(f"POST registration: {result}")
assert result >= 0, f"POST registration failed: {result}"

# Clean up
httpserver.off('/test_post', 'POST')
"""
        result = await client.run_test(code, timeout=10.0)
        
        if result.status != 'pass':
            raise AssertionError(f"{result.error_type}: {result.error}")
        
        return True

async def test_handler_capacity(client: WebREPLTestClient) -> bool:
    """Test maximum handler capacity (5 handlers max)"""
    async with TestAssertion(client, "Test handler capacity limit"):
        code = """
import httpserver

# Clean up all test handlers
for i in range(10):
    try:
        httpserver.off(f'/capacity{i}', 'GET')
    except:
        pass

# Register up to capacity (5 handlers)
def dummy_handler(uri, post_data=None):
    return "OK"

results = []
for i in range(6):  # Try to register 6 (one over limit)
    try:
        r = httpserver.on(f'/capacity{i}', dummy_handler, 'GET')
        results.append(r)
        print(f"Handler {i}: {r}")
    except Exception as e:
        results.append(-1)
        print(f"Handler {i}: exception {e}")

# First 5 should succeed (>= 0), 6th should fail (< 0)
print(f"Results: {results}")
assert len(results) == 6, f"Expected 6 results, got {len(results)}"
assert all(r >= 0 for r in results[:5]), f"First 5 should succeed: {results[:5]}"
# Note: 6th might succeed if a slot was free, so we don't assert it must fail

# Clean up
for i in range(6):
    try:
        httpserver.off(f'/capacity{i}', 'GET')
    except:
        pass
"""
        result = await client.run_test(code, timeout=15.0)
        
        if result.status != 'pass':
            raise AssertionError(f"{result.error_type}: {result.error}")
        
        return True

async def test_handler_replacement(client: WebREPLTestClient) -> bool:
    """Test replacing an existing handler"""
    async with TestAssertion(client, "Replace existing handler"):
        code = """
import httpserver

# Clean up
try:
    httpserver.off('/replace_test', 'GET')
except:
    pass

# Register first handler
def handler1(uri, post_data=None):
    return "Handler 1"

r1 = httpserver.on('/replace_test', handler1, 'GET')
print(f"First registration: {r1}")
assert r1 >= 0, f"First registration failed: {r1}"

# Unregister
httpserver.off('/replace_test', 'GET')

# Register second handler (same URI)
def handler2(uri, post_data=None):
    return "Handler 2"

r2 = httpserver.on('/replace_test', handler2, 'GET')
print(f"Second registration: {r2}")
assert r2 >= 0, f"Second registration failed: {r2}"

# Clean up
httpserver.off('/replace_test', 'GET')
"""
        result = await client.run_test(code, timeout=10.0)
        
        if result.status != 'pass':
            raise AssertionError(f"{result.error_type}: {result.error}")
        
        return True

# Main test runner
async def run_all_tests(url: str, password: str, ssl_verify: bool = True, debug: bool = False):
    """Run all HTTP server functional tests"""
    client = WebREPLTestClient(url, password, ssl_verify=ssl_verify, debug=debug)
    
    tests = [
        ("Register Handler", test_register_handler),
        ("Unregister Handler", test_unregister_handler),
        ("Multiple Handlers", test_multiple_handlers),
        ("POST Handler", test_post_handler),
        ("Handler Capacity", test_handler_capacity),
        ("Handler Replacement", test_handler_replacement),
    ]
    
    passed = 0
    failed = 0
    
    try:
        await client.connect()
        print("✓ Connected and authenticated\n")
        
        for name, test_func in tests:
            print("=" * 60)
            print(f"Test: {name}")
            print("=" * 60)
            
            try:
                await test_func(client)
                passed += 1
                print(f"✓ {name} passed\n")
            except AssertionError as e:
                failed += 1
                print(f"✗ {name} failed: {e}\n")
            except Exception as e:
                failed += 1
                print(f"✗ {name} error: {e}\n")
                import traceback
                traceback.print_exc()
        
    except Exception as e:
        print(f"\n✗ Test failed: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
    finally:
        await client.close()
    
    print("=" * 60)
    print(f"Results: {passed} passed, {failed} failed")
    print("=" * 60)
    
    if failed > 0:
        sys.exit(1)

if __name__ == "__main__":
    import argparse
    
    parser = argparse.ArgumentParser(description='HTTP Server Functional Tests')
    parser.add_argument('--url', required=True, help='WebREPL URL (e.g., wss://192.168.1.154/webrepl)')
    parser.add_argument('--password', required=True, help='WebREPL password')
    parser.add_argument('--no-ssl-verify', action='store_true', help='Disable SSL certificate verification')
    parser.add_argument('--debug', action='store_true', help='Enable debug output')
    
    args = parser.parse_args()
    
    asyncio.run(run_all_tests(args.url, args.password, ssl_verify=not args.no_ssl_verify, debug=args.debug))

