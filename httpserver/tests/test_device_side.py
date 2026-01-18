"""
Device-side tests for httpserver module.

These tests run directly on the device (via serial REPL or uploaded file)
and do NOT require WebREPL to be working. Use these when:
- httpserver is broken and WebREPL won't connect
- Testing low-level httpserver functionality
- Initial development/debugging

Usage:
    # Via serial REPL:
    exec(open('test_device_side.py').read())
    
    # Or copy-paste into REPL
"""

import time

print("=" * 60)
print("Device-Side httpserver Tests")
print("=" * 60)
print()
print("These tests run directly on device without WebREPL dependency")
print()

# Test 1: Basic start/stop
print("[1] Testing HTTP server start/stop...")
try:
    from esp32 import httpserver
    
    assert httpserver.start(8080) == True
    print("  ✓ Server started")
    time.sleep(0.5)
    
    assert httpserver.stop() == True
    print("  ✓ Server stopped")
    print("  ✓ Test 1 PASSED")
except Exception as e:
    print(f"  ✗ Test 1 FAILED: {e}")
    raise

# Test 2: Handler registration
print("\n[2] Testing handler registration...")
try:
    from esp32 import httpserver
    
    httpserver.start(8080)
    
    def handle_test(uri):
        return f"<html><body>URI: {uri}</body></html>"
    
    handler_id = httpserver.on("/test", handle_test)
    assert handler_id >= 0
    print(f"  ✓ Handler registered (ID: {handler_id})")
    
    httpserver.stop()
    print("  ✓ Test 2 PASSED")
except Exception as e:
    print(f"  ✗ Test 2 FAILED: {e}")
    raise

# Test 3: Multiple handlers
print("\n[3] Testing multiple handlers...")
try:
    from esp32 import httpserver
    
    httpserver.start(8080)
    
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
    print(f"  ✓ Registered 3 handlers: {id1}, {id2}, {id3}")
    
    httpserver.stop()
    print("  ✓ Test 3 PASSED")
except Exception as e:
    print(f"  ✗ Test 3 FAILED: {e}")
    raise

# Test 4: Queue processing
print("\n[4] Testing queue processing...")
try:
    from esp32 import httpserver
    
    httpserver.start(8080)
    
    def handle_test(uri):
        return "<html><body>Test</body></html>"
    
    httpserver.on("/test", handle_test)
    
    # Process queue multiple times
    for i in range(10):
        processed = httpserver.process_queue()
        time.sleep_ms(50)
    
    httpserver.stop()
    print("  ✓ Queue processing works")
    print("  ✓ Test 4 PASSED")
except Exception as e:
    print(f"  ✗ Test 4 FAILED: {e}")
    raise

# Test 5: WebSocket server (if available)
print("\n[5] Testing WebSocket server...")
try:
    from esp32 import httpserver, wsserver
    
    httpserver.start(8080)
    
    assert wsserver.start() == True
    print("  ✓ WebSocket server started")
    time.sleep(0.5)
    
    assert wsserver.is_running() == True
    print("  ✓ WebSocket server is running")
    
    wsserver.stop()
    httpserver.stop()
    print("  ✓ Test 5 PASSED")
except ImportError:
    print("  ⚠ WebSocket module not available, skipping")
except Exception as e:
    print(f"  ✗ Test 5 FAILED: {e}")
    raise

# Test 6: Error handling
print("\n[6] Testing error handling...")
try:
    from esp32 import httpserver
    
    # Stop without starting should not crash
    try:
        httpserver.stop()
    except Exception:
        pass  # Exception is acceptable
    
    print("  ✓ Error handling works")
    print("  ✓ Test 6 PASSED")
except Exception as e:
    print(f"  ✗ Test 6 FAILED: {e}")
    raise

print("\n" + "=" * 60)
print("All device-side tests passed!")
print("=" * 60)
