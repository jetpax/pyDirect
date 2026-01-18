# pyDirect Agent Instructions

**pyDirect** is a suite of C-based accelerator modules that provide high-performance functionality for MicroPython applications. These modules implement critical functionality directly in C with minimal Python overhead, achieving near-native performance.

Always reference these instructions first and fallback to search or bash commands only when you encounter unexpected
information that does not match the info here.

## Shared Ground Rules
- defer ISR work to task context, and follow C99 with two-space indentation/no tabs.
- Match file organization: core stack under `src`, MCU/BSP support in `hw/{mcu,bsp}`, examples under `examples/{device,host,dual}`, docs in `docs`, tests under `test/{unit-test,fuzz,hil}`.
- Prefer `.clang-format` for C/C++ formatting, run `pre-commit run --all-files` before submitting, and document board/HIL coverage when applicable.
- Commit in imperative mood, keep changes scoped, and supply PRs with linked issues plus test/build evidence.



## Build Options



## Flashing and Deployment

- **Flash with serial**:

- **Flash with JTAG**:



## Hardware-in-the-Loop (HIL) Testing

pyDirect modules support HIL testing via WebREPL Binary Protocol, allowing remote test execution on ESP32 devices without disrupting the connection.

### WebREPL HIL Test Infrastructure

**Location**: `<module>/tests/` (e.g., `httpserver/tests/`, `twai/tests/`)

**Core Components**:
- `webrepl_client.py` - WebREPL Binary Protocol client library
- `test_helpers.py` - Reusable assertion functions and context managers
- `test_*.py` - Individual test suites

**Requirements**:
```bash
# One-time setup
cd <module>/tests
python3 -m venv venv
source venv/bin/activate
pip install websockets cbor2
```

### Writing HIL Tests

**Key Principles**:
1. **JSON-only output**: Test code must NOT print - only the final JSON result is output
2. **Production code stays clean**: Tests bear the burden, not the protocol implementation
3. **No server lifecycle tests**: Tests that start/stop httpserver/wsserver conflict with WebREPL

**Test Code Pattern**:
```python
async def test_feature(client: WebREPLTestClient) -> bool:
    """Test description"""
    async with TestAssertion(client, "Test name"):
        code = """
import module_to_test

# Test logic (NO print statements)
result = module_to_test.do_something()
assert result == expected, "Error message"

# Assertions only - JSON added by wrapper automatically
"""
        result = await client.run_test(code, timeout=15.0)
        
        if result.status != 'pass':
            raise AssertionError(f"{result.error_type}: {result.error}")
        
        return True
```

**Protocol Details**:
- Test code is wrapped with try/except and returns JSON: `{"status": "pass|fail|error", "message": "...", "error": "...", "error_type": "..."}`
- ESP-IDF logging (e.g., `I (timestamp) TAG: message`) appears alongside JSON but doesn't interfere
- Only the JSON line is parsed for test results

**Running Tests**:
```bash
cd <module>/tests
source venv/bin/activate
python test_<name>.py --url ws://192.168.1.154/webrepl --password <password>

# For HTTPS with self-signed cert:
python test_<name>.py --url wss://192.168.1.154/webrepl --password <password> --no-ssl-verify

# Debug mode (show all protocol messages):
python test_<name>.py --url <url> --password <password> --debug
```

**Examples**:
- `httpserver/tests/test_http_functional.py` - HTTP URI handlers (doesn't start/stop server)
- `httpserver/tests/test_connection_recovery.py` - WebSocket connection cleanup
- `twai/tests/test_twai_loopback.py` - CAN/TWAI loopback mode (no external hardware)



## Documentation


## Code Quality and Validation



## Static Analysis with PVS-Studio


## Validation Checklist

### ALWAYS Run These After Making Changes


### Manual Testing Scenarios

### Board Selection for Testing

## Release Instructions

**DO NOT commit files automatically - only modify files and let the maintainer review before committing.**

1. Bump the release version variable at the top of `tools/make_release.py`.
2. Execute `python3 tools/make_release.py` to refresh:
   - `src/tusb_option.h` (version defines)
   - `repository.yml` (version mapping)
   - `library.json` (PlatformIO version)
   - `sonar-project.properties` (SonarQube version)
   - `docs/reference/boards.rst` (generated board documentation)
   - `hw/bsp/BoardPresets.json` (CMake presets)
3. Generate release notes for `docs/info/changelog.rst`:
   - Get commit list: `git log <last-release-tag>..HEAD --oneline`
   - **Visit GitHub PRs** for merged pull requests to understand context and gather details
   - Use GitHub tools to search/read PRs: `github-mcp-server-list_pull_requests`, `github-mcp-server-pull_request_read`
   - Extract key changes, API modifications, bug fixes, and new features from PR descriptions
   - Add new changelog entry following the existing format:
     - Version heading with equals underline (e.g., `0.20.0` followed by `======`)
     - Release date in italics (e.g., `*November 19, 2024*`)
     - Major sections: General, API Changes, Controller Driver (DCD & HCD), Device Stack, Host Stack, Testing
     - Use bullet lists with descriptive categorization
     - Reference function names, config macros, and file paths using RST inline code (double backticks)
     - Include meaningful descriptions, not just commit messages
4. **Validation before commit**:
   - Run unit tests: `cd test/unit-test && ceedling test:all`
   - Build at least one example: `cd examples/device/cdc_msc && make BOARD=stm32f407disco all`
   - Verify changed files look correct: `git diff --stat`
5. **Leave files unstaged** for maintainer to review, modify if needed, and commit with message: `Bump version to X.Y.Z`
6. **After maintainer commits**: Create annotated tag with `git tag -a vX.Y.Z -m "Release X.Y.Z"`
7. Push commit and tag: `git push origin <branch> && git push origin vX.Y.Z`
8. Create GitHub release from the tag with changelog content

## Repository Structure Quick Reference


#### Build Time Reference
- **Dependency fetch**: <1 second
- **Single example build**: 1-3 seconds
- **Unit tests**: ~4 seconds
- **Documentation build**: ~2.5 seconds
- **Full board examples**: 15-20 seconds
- **Toolchain installation**: 2-5 minutes (one-time)

#### Key Files to Know


#### Debugging Build Issues


#### Working with USB Device Classes



#### MCU Family Support


### Code Style Guidelines

#### General Coding Standards
- Use C99 standard
- Memory-safe: no dynamic allocation
- Thread-safe: defer all interrupt events to non-ISR task functions
- 2-space indentation, no tabs
- Use snake_case for variables/functions
- Use UPPER_CASE for macros and constants
- Follow existing variable naming patterns in files you're modifying
- Include proper header comments with MIT license
- Add descriptive comments for non-obvious functions

#### Best Practices
- When including headers, group in order: C stdlib, tusb common, drivers, classes
- Always check return values from functions that can fail
- Use TU_ASSERT() for error checking with return statements
- Follow the existing code patterns in the files you're modifying

Remember: pyDirect is designed for embedded systems - builds are fast, tests are focused, and the codebase is optimized for resource-constrained environments.