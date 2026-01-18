# httpserver HIL Tests

Hardware-In-the-Loop (HIL) tests for the httpserver module using WebREPL Binary Protocol (WBP).

## Overview

These tests execute remotely on MicroPython devices via WebREPL without requiring test files to be uploaded to the device. Tests are written in Python and run on the host machine, communicating with the device through WebSocket using the WebREPL Binary Protocol.

## ⚠️ Important: Circular Dependency Limitation

**These tests have a fundamental limitation**: WebREPL runs over WebSocket, which depends on `wsserver`, which in turn depends on `httpserver`. This creates a circular dependency:

```
httpserver → wsserver → webrepl → (tests httpserver)
```

**Implications:**
- If `httpserver` is completely broken, WebREPL won't work, so we can't test it via WebREPL
- These tests assume basic `httpserver` functionality is working (enough to establish WebSocket connections)
- For testing broken `httpserver` functionality, use **device-side tests** (see below)

**What these tests ARE good for:**
- Testing higher-level functionality when basic HTTP/WebSocket is working
- Integration testing of the full stack
- Regression testing after fixes
- Testing API usage patterns

**What these tests CANNOT test:**
- Complete `httpserver` failure scenarios
- Initial `httpserver` startup when it's broken
- Low-level HTTP server issues that prevent WebSocket connections

## Fallback: Device-Side Tests

For testing broken or low-level `httpserver` functionality, use device-side tests similar to `twai/tests/`:

```python
# tests/test_device_side.py
# Upload to device and run: exec(open('test_device_side.py').read())

import httpserver
import time

# Test basic start/stop without WebREPL dependency
assert httpserver.start(8080) == True
time.sleep(0.5)
assert httpserver.stop() == True
print("✓ Basic test passed")
```

## Benefits

- **No device filesystem needed**: Tests live on host machine
- **Version controlled**: Tests alongside source code
- **CI/CD friendly**: No file upload step required
- **Tests WebREPL itself**: Exercises the protocol while testing httpserver
- **Structured results**: JSON responses via M2M channel
- **Remote execution**: Run tests from anywhere

## Prerequisites

1. **Python 3.7+** with asyncio support
2. **Dependencies**: Install test requirements
   ```bash
   pip install -r requirements.txt
   ```
3. **Device setup**: ESP32 device running MicroPython with:
   - httpserver module compiled and loaded
   - WebREPL enabled and configured
   - Network connectivity (WiFi AP or Station mode)
   - WebREPL password set

## Installation

### Option 1: Virtual Environment (Recommended)

Python 3.13+ uses externally-managed environments, so use a virtual environment:

```bash
cd httpserver/tests

# Run setup script (creates venv and installs dependencies)
./setup_venv.sh

# Or manually:
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```

**To activate the virtual environment:**
```bash
source venv/bin/activate
```

### Option 2: System-wide (Not Recommended)

If you must install system-wide (not recommended on Python 3.13+):
```bash
pip install --break-system-packages -r requirements.txt
```

### VS Code Setup

1. **Select Python Interpreter:**
   - Press `Cmd+Shift+P` (Mac) or `Ctrl+Shift+P` (Windows/Linux)
   - Type "Python: Select Interpreter"
   - Choose the virtual environment: `./venv/bin/python`

2. **Terminal will auto-activate:**
   - VS Code should detect the virtual environment
   - Terminal will show `(venv)` prefix when activated

3. **If terminal doesn't activate automatically:**
   ```bash
   source venv/bin/activate
   ```

## Running Tests

### Basic HTTP Server Tests

```bash
python test_http_basic.py
```

With custom URL/password:
```bash
python test_http_basic.py --url ws://192.168.4.1/WebREPL --password yourpassword
```

For secure WebSocket (wss://) with self-signed certificates:
```bash
python test_http_basic.py --url wss://192.168.1.154/webrepl --password yourpassword --no-ssl-verify
```

### WebSocket Server Tests

```bash
python test_websocket.py
```

### Integration Tests

```bash
python test_integration.py
```

### Run All Tests

```bash
# Run all test suites
python test_http_basic.py
python test_websocket.py
python test_integration.py
```

## Test Structure

```
tests/
├── webrepl_client.py      # WebREPL WBP client library
├── test_helpers.py         # Test helper utilities
├── test_http_basic.py     # HTTP server basic tests (via WebREPL)
├── test_websocket.py      # WebSocket server tests (via WebREPL)
├── test_integration.py    # Full stack integration tests (via WebREPL)
├── test_device_side.py   # Device-side tests (no WebREPL dependency)
├── requirements.txt       # Python dependencies
└── README.md              # This file
```

### Test Types

**WebREPL-based tests** (remote execution):
- `test_http_basic.py` - HTTP server tests via WebREPL
- `test_websocket.py` - WebSocket tests via WebREPL
- `test_integration.py` - Integration tests via WebREPL

**Device-side tests** (direct execution):
- `test_device_side.py` - Tests that run directly on device (no WebREPL needed)
  - Use when WebREPL is broken
  - Use for low-level testing
  - Upload via serial/UART or copy-paste into REPL

## Test Categories

### HTTP Basic Tests (`test_http_basic.py`)

- Server start/stop
- Handler registration
- Multiple handlers
- Queue processing
- GET/POST handlers
- Error handling

### WebSocket Tests (`test_websocket.py`)

- WebSocket server start/stop
- HTTP + WebSocket integration
- Configuration options
- Multiple starts handling

### Integration Tests (`test_integration.py`)

- Full stack (HTTP + WebSocket + WebFiles)
- WebFiles integration
- Concurrent operations
- Stress testing
- Memory cleanup

## Writing New Tests

### Using Test Helpers

```python
from webrepl_client import WebREPLTestClient
from test_helpers import assert_http_server_start_stop

async def test_my_feature(client):
    await assert_http_server_start_stop(client, port=8080)
```

### Custom Test Code

```python
from webrepl_client import WebREPLTestClient

async def test_custom(client):
    code = """
import httpserver
assert httpserver.start(8080) == True
httpserver.stop()
"""
    result = await client.run_test(code)
    assert result.status == 'pass'
```

### Using Context Managers

```python
import test_helpers

async def test_with_context(client):
    async with test_helpers.TestAssertion(client, "My test"):
        result = await client.run_test("...")
        assert result.status == 'pass'
```

## WebREPL Client API

### Basic Usage

```python
from webrepl_client import WebREPLTestClient

client = WebREPLTestClient('ws://192.168.4.1/WebREPL', 'password')
await client.connect()

# Execute code and get result
result = await client.execute_m2m("print('hello')")

# Run test with structured result
test_result = await client.run_test("assert True")

await client.close()
```

### Methods

- `connect()`: Connect and authenticate
- `execute_m2m(code)`: Execute code on M2M channel, returns string result
- `run_test(code)`: Execute test code, returns `TestResult` object
- `execute_terminal(code)`: Execute on terminal channel, returns output lines
- `interrupt(channel)`: Send interrupt signal
- `close()`: Close connection

## Test Result Format

```python
@dataclass
class TestResult:
    status: str      # 'pass', 'fail', 'error'
    message: str     # Optional success message
    error: str       # Optional error message
    error_type: str  # Optional exception type
    data: Any        # Optional additional data
```

## Troubleshooting

### Connection Failed

- Verify device IP address and WebREPL port
- Check WiFi connectivity
- Ensure WebREPL is enabled on device
- **If httpserver is broken**: Use device-side tests instead (see above)
- Check firewall settings

### Authentication Failed

- Verify WebREPL password matches device configuration
- Check device logs for authentication errors
- Ensure WebREPL is not already connected elsewhere
- **If WebSocket connection fails**: This may indicate `httpserver` or `wsserver` issues - use device-side tests

### Test Timeout

- Increase timeout parameter in test code
- Check device is responsive (try simple `print()` test)
- Verify network latency is acceptable
- **If WebREPL is unresponsive**: May indicate `httpserver`/`wsserver` issues

### Import Errors

- Ensure all dependencies are installed: `pip install -r requirements.txt`
- Check Python version (3.7+ required)
- Verify you're in the `tests/` directory

### httpserver Not Working

**If `httpserver` is completely broken:**
1. Use device-side tests (upload to device via serial/UART)
2. Test via serial REPL directly
3. Check device logs for `httpserver` initialization errors
4. Verify `httpserver` module is compiled and loaded correctly

## CI/CD Integration

Example GitHub Actions workflow:

```yaml
name: HIL Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      - uses: actions/setup-python@v2
        with:
          python-version: '3.9'
      - run: pip install -r httpserver/tests/requirements.txt
      - run: python httpserver/tests/test_http_basic.py --url ${{ secrets.WEBREPL_URL }} --password ${{ secrets.WEBREPL_PASSWORD }}
```

## Protocol Details

These tests use the **WebREPL Binary Protocol (WBP)** specification:
- Channel 0: Events (authentication)
- Channel 1: Terminal REPL
- Channel 2: Machine-to-Machine (M2M) - used for structured test results
- Channel 23: File operations

See `webrepl_binary_protocol_rfc.md` for full protocol specification.

## Contributing

When adding new tests:

1. Follow existing test structure
2. Use test helpers when possible
3. Include descriptive test names
4. Add cleanup code to prevent resource leaks
5. Update this README if adding new test categories

## Testing Strategy

### When to Use WebREPL Tests

✅ Use WebREPL tests when:
- Basic `httpserver` functionality is working
- Testing API usage patterns
- Integration testing
- Regression testing after fixes
- CI/CD automation

### When to Use Device-Side Tests

✅ Use device-side tests when:
- `httpserver` is broken and WebREPL won't connect
- Testing low-level functionality
- Initial development/debugging
- Testing failure scenarios
- WebSocket/WebREPL is not available

### Recommended Workflow

1. **Start with device-side tests** (`test_device_side.py`) to verify basic functionality
2. **Once basic functionality works**, use WebREPL tests for comprehensive testing
3. **For CI/CD**, use WebREPL tests (faster, automated)
4. **For debugging failures**, fall back to device-side tests

## See Also

- [WebREPL Binary Protocol RFC](../../webrepl/webrepl_binary_protocol_rfc.md)
- [httpserver README](../README.md)
- [twai/tests](../../twai/tests/) - Example of device-side tests
