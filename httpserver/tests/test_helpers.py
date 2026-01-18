"""
Test helper utilities for httpserver HIL testing.

Provides convenience functions and context managers for writing tests.
"""

import asyncio
from typing import Optional
from webrepl_client import WebREPLTestClient, TestResult


class TestAssertion:
    """Context manager for test assertions with automatic reporting."""
    
    def __init__(self, client: WebREPLTestClient, description: str):
        self.client = client
        self.description = description
        self.passed = False
    
    async def __aenter__(self):
        return self
    
    async def __aexit__(self, exc_type, exc_val, exc_tb):
        if exc_type:
            print(f"✗ {self.description}: {exc_val}")
            return False
        
        if not self.passed:
            print(f"✓ {self.description}")
            self.passed = True
        
        return True


async def assert_http_server_start_stop(
    client: WebREPLTestClient,
    port: int = 8080,
    description: Optional[str] = None
) -> bool:
    """
    Assert HTTP server can start and stop successfully.
    
    Args:
        client: WebREPL test client
        port: Port number to test
        description: Optional test description
        
    Returns:
        True if test passes
        
    Raises:
        AssertionError: If test fails
    """
    desc = description or f"HTTP server start/stop on port {port}"
    
    async with TestAssertion(client, desc):
        code = f"""
import httpserver
import time

# Test start
result = httpserver.start({port})
assert result == True, "start() returned " + str(result)
time.sleep(0.5)  # Wait for initialization

# Test stop
result = httpserver.stop()
assert result == True, "stop() returned " + str(result)
"""
        result = await client.run_test(code, timeout=15.0)
        
        if result.status != 'pass':
            error_msg = result.error or result.message or "Unknown error"
            error_type = result.error_type or "Unknown"
            full_error = f"{error_type}: {error_msg}"
            if result.data:
                full_error += f" (data: {result.data})"
            raise AssertionError(full_error)
        
        return True


async def assert_handler_registration(
    client: WebREPLTestClient,
    uri: str = "/test",
    description: Optional[str] = None
) -> bool:
    """
    Assert HTTP handler can be registered successfully.
    
    Args:
        client: WebREPL test client
        uri: URI pattern to register
        description: Optional test description
        
    Returns:
        True if test passes
        
    Raises:
        AssertionError: If test fails
    """
    desc = description or f"Handler registration for '{uri}'"
    
    async with TestAssertion(client, desc):
        code = f"""
import httpserver

httpserver.start(8080)

def handle_test(uri):
    return f"<html><body>URI: {{uri}}</body></html>"

handler_id = httpserver.on("{uri}", handle_test)
assert handler_id >= 0

httpserver.stop()
"""
        result = await client.run_test(code, timeout=15.0)
        
        if result.status != 'pass':
            error_msg = result.error or result.message or "Unknown error"
            error_type = result.error_type or "Unknown"
            full_error = f"{error_type}: {error_msg}"
            if result.data:
                full_error += f" (data: {result.data})"
            raise AssertionError(full_error)
        
        return True


async def assert_websocket_server(
    client: WebREPLTestClient,
    description: Optional[str] = None
) -> bool:
    """
    Assert WebSocket server can start and stop successfully.
    
    Args:
        client: WebREPL test client
        description: Optional test description
        
    Returns:
        True if test passes
        
    Raises:
        AssertionError: If test fails
    """
    desc = description or "WebSocket server start/stop"
    
    async with TestAssertion(client, desc):
        code = """
import httpserver, wsserver
import time

httpserver.start(8080)
assert wsserver.start() == True
time.sleep(0.5)

wsserver.stop()
httpserver.stop()
"""
        result = await client.run_test(code)
        
        if result.status != 'pass':
            error_msg = result.error or result.message or "Unknown error"
            error_type = result.error_type or "Unknown"
            full_error = f"{error_type}: {error_msg}"
            if result.data:
                full_error += f" (data: {result.data})"
            raise AssertionError(full_error)
        
        return True


async def assert_webfiles_serve(
    client: WebREPLTestClient,
    base_path: str = "/files",
    uri_prefix: str = "/*",
    description: Optional[str] = None
) -> bool:
    """
    Assert webfiles can serve static files.
    
    Args:
        client: WebREPL test client
        base_path: Base filesystem path
        uri_prefix: URI prefix pattern
        description: Optional test description
        
    Returns:
        True if test passes
        
    Raises:
        AssertionError: If test fails
    """
    desc = description or f"Webfiles serve from '{base_path}'"
    
    async with TestAssertion(client, desc):
        code = f"""
import httpserver, webfiles

httpserver.start(8080)
assert webfiles.serve("{base_path}", "{uri_prefix}") == True
httpserver.stop()
"""
        result = await client.run_test(code)
        
        if result.status != 'pass':
            error_msg = result.error or result.message or "Unknown error"
            error_type = result.error_type or "Unknown"
            full_error = f"{error_type}: {error_msg}"
            if result.data:
                full_error += f" (data: {result.data})"
            raise AssertionError(full_error)
        
        return True


async def assert_full_stack(
    client: WebREPLTestClient,
    port: int = 8080,
    description: Optional[str] = None
) -> bool:
    """
    Assert full HTTP/WebSocket/Webfiles stack can run together.
    
    Args:
        client: WebREPL test client
        port: Port number
        description: Optional test description
        
    Returns:
        True if test passes
        
    Raises:
        AssertionError: If test fails
    """
    desc = description or f"Full stack integration on port {port}"
    
    async with TestAssertion(client, desc):
        code = f"""
import httpserver, wsserver, webfiles
import time

# Start HTTP server
assert httpserver.start({port}) == True

# Start WebSocket server
assert wsserver.start() == True

# Register file server
assert webfiles.serve("/files", "/*") == True

# Register API handler
def handle_api(uri, post_data):
    return '{{"status": "ok"}}'

handler_id = httpserver.on("/api", handle_api, "POST")
assert handler_id >= 0

# Run for a short time
time.sleep(1)

# Process some queue items
processed = httpserver.process_queue()

# Cleanup
wsserver.stop()
httpserver.stop()
"""
        result = await client.run_test(code)
        
        if result.status != 'pass':
            error_msg = result.error or result.message or "Unknown error"
            error_type = result.error_type or "Unknown"
            full_error = f"{error_type}: {error_msg}"
            if result.data:
                full_error += f" (data: {result.data})"
            raise AssertionError(full_error)
        
        return True


async def run_with_cleanup(
    client: WebREPLTestClient,
    test_code: str,
    cleanup_code: Optional[str] = None
) -> TestResult:
    """
    Run test code with guaranteed cleanup.
    
    Args:
        client: WebREPL test client
        test_code: Test code to execute
        cleanup_code: Cleanup code (default: stop all servers)
        
    Returns:
        TestResult from test execution
    """
    if cleanup_code is None:
        cleanup_code = """
import httpserver, wsserver
try:
    wsserver.stop()
except:
    pass
try:
    httpserver.stop()
except:
    pass
"""
    
    try:
        result = await client.run_test(test_code)
        return result
    finally:
        # Always run cleanup
        try:
            await client.run_test(cleanup_code)
        except:
            pass  # Ignore cleanup errors
