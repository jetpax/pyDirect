"""
WebREPL Binary Protocol (WBP) Client for HIL Testing

This client implements the WebREPL Binary Protocol (WBP) specification
to execute tests remotely on MicroPython devices without requiring
test files to be uploaded to the device.

⚠️ IMPORTANT LIMITATION:
This client requires WebREPL to be working, which depends on:
  httpserver → wsserver → webrepl → (tests httpserver)

If httpserver is completely broken, WebREPL won't connect, so these
tests cannot be used. For testing broken httpserver functionality,
use device-side tests (test_device_side.py) instead.

Usage:
    from webrepl_client import WebREPLTestClient
    
    client = WebREPLTestClient('ws://192.168.4.1/WebREPL', 'password')
    await client.connect()
    result = await client.execute_m2m("print('hello')")
    await client.close()
"""

import asyncio
import json
import ssl
import websockets
import cbor2
from typing import Optional, Dict, Any, List, Callable
from dataclasses import dataclass

# WBP Channel Constants
CH_EVENT = 0
CH_TRM = 1
CH_M2M = 2
CH_DBG = 3
CH_FILE = 23

# Execution Opcodes
OP_EXE = 0  # Execute
OP_INT = 1  # Interrupt
OP_RST = 2  # Reset

# Response Opcodes
OP_RES = 0  # Result
OP_CON = 1  # Continuation
OP_PRO = 2  # Progress
OP_COM = 3  # Completions

# Event Opcodes
EVENT_AUTH = 0
EVENT_AUTH_OK = 1
EVENT_AUTH_FAIL = 2
EVENT_INFO = 3
EVENT_LOG = 4

# File Operation Opcodes
FILE_RRQ = 1  # Read Request
FILE_WRQ = 2  # Write Request
FILE_DATA = 3
FILE_ACK = 4
FILE_ERROR = 5


@dataclass
class TestResult:
    """Result from a test execution."""
    status: str  # 'pass', 'fail', 'error'
    message: Optional[str] = None
    error: Optional[str] = None
    error_type: Optional[str] = None
    data: Optional[Any] = None


class WebREPLTestClient:
    """
    WebREPL Binary Protocol client for remote test execution.
    
    This client connects to a MicroPython device via WebSocket using
    the WebREPL Binary Protocol (WBP) and can execute Python code remotely,
    returning structured results.
    """
    
    def __init__(self, url: str, password: str, timeout: float = 30.0, ssl_verify: bool = True, debug: bool = False):
        """
        Initialize WebREPL test client.
        
        Args:
            url: WebSocket URL (e.g., 'ws://192.168.4.1/WebREPL' or 'wss://...')
            password: WebREPL password
            timeout: Connection timeout in seconds
            ssl_verify: Verify SSL certificates (set False for self-signed certs in development)
            debug: Enable debug output for message tracing
        """
        self.url = url
        self.password = password
        self.timeout = timeout
        self.ssl_verify = ssl_verify
        self.no_ssl_verify = not ssl_verify  # Keep both for compatibility
        self.debug = debug
        self.ws = None
        self.authenticated = False
        self.response_id = 0
        self.pending_responses: Dict[str, asyncio.Future] = {}
        self.response_buffers: Dict[str, str] = {}  # Buffer for M2M RES messages
        self.channel_handlers: Dict[int, Callable] = {}
        self.event_handlers: Dict[int, Callable] = {}
        self._receiver_task = None  # Initialize receiver task attribute
    
    async def connect(self) -> bool:
        """
        Connect to WebREPL and authenticate.
        
        Returns:
            True if authentication successful
            
        Raises:
            Exception: If connection or authentication fails
        """
        try:
            # Configure SSL context for wss:// connections
            ssl_context = None
            if self.url.startswith('wss://'):
                ssl_context = ssl.create_default_context()
                if not self.ssl_verify:
                    # Disable certificate verification for self-signed certs (development only)
                    ssl_context.check_hostname = False
                    ssl_context.verify_mode = ssl.CERT_NONE
            
            self.ws = await asyncio.wait_for(
                websockets.connect(
                    self.url,
                    subprotocols=['WebREPL.binary.v1'],
                    ping_interval=20,  # Send PING every 20 seconds
                    ping_timeout=10,   # Wait 10 seconds for PONG response
                    ssl=ssl_context
                ),
                timeout=self.timeout
            )
            
            # Start message receiver task
            self._receiver_task = asyncio.create_task(self._receiver_loop())
            
            # Authenticate
            await self.send([CH_EVENT, EVENT_AUTH, self.password])
            
            # Wait for AUTH_OK with timeout
            auth_future = asyncio.Future()
            self.event_handlers[EVENT_AUTH_OK] = lambda: auth_future.set_result(True)
            self.event_handlers[EVENT_AUTH_FAIL] = lambda err: auth_future.set_exception(
                Exception(f"Authentication failed: {err}")
            )
            
            try:
                await asyncio.wait_for(auth_future, timeout=5.0)
                self.authenticated = True
                return True
            except asyncio.TimeoutError:
                raise Exception("Authentication timeout")
                
        except Exception as e:
            if self.ws:
                await self.ws.close()
            raise Exception(f"Connection failed: {e}")
    
    def _is_connection_open(self) -> bool:
        """Check if WebSocket connection is open."""
        if not self.ws:
            return False
        # Check connection state - websockets API varies by version
        try:
            # websockets 15.x uses close_code to check state (None = open)
            if hasattr(self.ws, 'close_code'):
                return self.ws.close_code is None
            # Older versions might use 'closed' attribute
            elif hasattr(self.ws, 'closed'):
                return not self.ws.closed
            # Some versions use state enum
            elif hasattr(self.ws, 'state'):
                state = self.ws.state
                if hasattr(state, 'name'):
                    return state.name == 'OPEN'
                elif hasattr(state, 'value'):
                    # State might be an enum with value
                    return state.value == 1  # OPEN state
            # If we can't determine, assume open if ws exists and authenticated
            return self.authenticated
        except Exception:
            # If checking fails, assume closed to be safe
            return False
    
    async def _receiver_loop(self):
        """Background task to receive and route messages."""
        try:
            while self._is_connection_open():
                try:
                    data = await self.ws.recv()
                    msg = cbor2.loads(data)
                    await self._handle_message(msg)
                except websockets.exceptions.ConnectionClosed:
                    # Connection closed normally
                    break
                except websockets.exceptions.InvalidState:
                    # Connection is in invalid state (likely closed)
                    break
                except AttributeError as e:
                    # Handle attribute errors gracefully
                    if 'closed' in str(e):
                        break
                    raise
                except Exception as e:
                    # Log but continue - might be a message parsing error
                    print(f"Receiver error processing message: {e}")
                    continue
        except websockets.exceptions.ConnectionClosed:
            # Connection closed normally
            pass
        except Exception as e:
            # Only print if it's not a normal closure or attribute error
            if "invalid state" not in str(e).lower() and "closed" not in str(e).lower():
                print(f"Receiver error: {e}")
    
    async def _handle_message(self, msg: List):
        """Route incoming messages to appropriate handlers."""
        if not msg or len(msg) < 2:
            return
        
        channel = msg[0]
        
        if channel == CH_EVENT:
            await self._handle_event(msg)
        elif channel == CH_M2M:
            await self._handle_m2m(msg)
        elif channel == CH_TRM:
            await self._handle_channel(msg, CH_TRM)
        elif channel == CH_FILE:
            await self._handle_file(msg)
        else:
            # Generic channel handler
            await self._handle_channel(msg, channel)
    
    async def _handle_event(self, msg: List):
        """Handle event channel messages."""
        if len(msg) < 2:
            return
        
        event = msg[1]
        handler = self.event_handlers.get(event)
        
        if handler:
            if event == EVENT_AUTH_FAIL and len(msg) > 2:
                handler(msg[2])
            else:
                handler()
    
    async def _handle_m2m(self, msg: List):
        """Handle M2M channel messages."""
        if len(msg) < 2:
            return
        
        if self.debug:
            print(f"[DEBUG] M2M message: {msg}")
        
        opcode = msg[1]
        msg_id = None
        
        # Extract message ID if present
        # RES format: [channel, OP_RES, data, msg_id]
        # PRO format: [channel, OP_PRO, status, ?error/null, msg_id]
        # Per modwebrepl.c: when id present, format is [ch, op, status, error/null, id]
        if opcode == OP_RES and len(msg) > 3:
            msg_id = msg[3]
        elif opcode == OP_PRO:
            # PRO format per modwebrepl.c wbp_send_progress():
            # Success with id: [ch, op, status, null, id] - 5 elements
            # Error with id: [ch, op, status, "error", id] - 5 elements  
            # No id: [ch, op, status] or [ch, op, status, error] - 3 or 4 elements
            if len(msg) >= 5:
                # [ch, op, status, error/null, id] - id is always last when present
                msg_id = msg[4] if isinstance(msg[4], str) else None
            elif len(msg) == 4:
                # Could be [ch, op, status, error] (no id) or [ch, op, status, id] (no error)
                # Check if last element looks like our msg_id format
                last_elem = msg[3]
                if isinstance(last_elem, str) and last_elem.startswith('req-'):
                    msg_id = last_elem
        
        if msg_id and msg_id in self.pending_responses:
            future = self.pending_responses[msg_id]
            
            if opcode == OP_RES:
                # Result data - BUFFER IT (don't resolve immediately!)
                # Device sends character-by-character due to drain task bug
                result_data = msg[2] if len(msg) > 2 else ''
                if self.debug:
                    print(f"[DEBUG] RES for {msg_id}: {result_data}")
                
                # Append to buffer
                if msg_id not in self.response_buffers:
                    self.response_buffers[msg_id] = []
                self.response_buffers[msg_id].append(result_data)
                
            elif opcode == OP_PRO:
                # Progress/status - NOW resolve with buffered data
                status = msg[2] if len(msg) > 2 else 0
                # Error is at position 3 if present
                # Format: [ch, op, status, error/null, id] when id present (5 elements)
                error = None
                if len(msg) > 3:
                    error_val = msg[3]
                    # If it's a string, it's an error message (not None/null)
                    # msg_id would be at position 4 in 5-element array
                    if isinstance(error_val, str):
                        # Only treat as error if it's not the msg_id
                        if len(msg) < 5 or error_val != msg[4]:
                            error = error_val
                    # None/null means no error (success case)
                
                # Get buffered data
                buffered_parts = self.response_buffers.get(msg_id, [])
                full_response = ''.join(buffered_parts)
                
                if self.debug:
                    print(f"[DEBUG] PRO for {msg_id}: status={status}, error={error}, buffer_parts={len(buffered_parts)}, full_len={len(full_response)}")
                
                # Clean up buffer
                if msg_id in self.response_buffers:
                    del self.response_buffers[msg_id]
                
                if not future.done():
                    future.set_result(('progress', status, error, full_response))
                    
        elif self.debug and opcode == OP_PRO:
            # PRO message but no matching msg_id - might be issue
            print(f"[DEBUG] PRO message without matching msg_id: {msg}")
    
    async def _handle_channel(self, msg: List, channel: int):
        """Handle generic channel messages."""
        handler = self.channel_handlers.get(channel)
        if handler:
            handler(msg)
    
    async def _handle_file(self, msg: List):
        """Handle file operation messages."""
        # File operations can be handled by specific handlers if needed
        pass
    
    async def send(self, msg: List):
        """Send CBOR-encoded message."""
        if not self._is_connection_open():
            raise Exception("Not connected or connection closed")
        
        try:
            data = cbor2.dumps(msg)
            await self.ws.send(data)
        except websockets.exceptions.ConnectionClosed:
            raise Exception("Connection closed while sending")
        except websockets.exceptions.InvalidState:
            raise Exception("Connection in invalid state")
        except AttributeError as e:
            if 'closed' in str(e):
                raise Exception("Connection closed")
            raise
    
    async def execute_m2m(self, code: str, timeout: float = 10.0) -> str:
        """
        Execute code on M2M channel and return result as string.
        
        Args:
            code: Python code to execute
            timeout: Execution timeout in seconds
            
        Returns:
            Result string (typically JSON from print() statements)
            
        Raises:
            Exception: If execution fails or times out
        """
        if not self.authenticated:
            raise Exception("Not authenticated")
        
        if not self._is_connection_open():
            raise Exception("Connection closed")
        
        msg_id = f"req-{self.response_id}"
        self.response_id += 1
        
        # Create future for response
        future = asyncio.Future()
        self.pending_responses[msg_id] = future
        
        try:
            # Send execution request
            await self.send([CH_M2M, OP_EXE, code, 0, msg_id])
            
            # Wait for PRO message (which includes buffered response)
            # The _handle_m2m buffers all RES messages and returns them with PRO
            start_time = asyncio.get_event_loop().time()
            
            # Check if connection is still alive
            if not self._is_connection_open():
                raise Exception("Connection closed during execution")
            
            # Check timeout
            elapsed = asyncio.get_event_loop().time() - start_time
            if elapsed >= timeout:
                raise Exception(f"Execution timeout after {timeout}s (no response received)")
            
            try:
                # Wait for PRO message with remaining timeout
                remaining_timeout = max(0.1, timeout - elapsed)
                response_type, *args = await asyncio.wait_for(future, timeout=remaining_timeout)
                
                if response_type == 'progress':
                    # PRO message includes: status, error, full_response (buffered)
                    status = args[0] if len(args) > 0 else 0
                    error = args[1] if len(args) > 1 else None
                    full_response = args[2] if len(args) > 2 else ''
                    
                    if status != 0:
                        # Execution failed
                        error_details = error or 'Execution failed'
                        if full_response:
                            # If we got JSON output, try to parse for better error
                            try:
                                parsed = json.loads(full_response.strip())
                                if 'error' in parsed:
                                    error_details = f"{parsed.get('type', 'Error')}: {parsed.get('error', error_details)}"
                                else:
                                    error_details = f"{error_details} (output: {full_response})"
                            except json.JSONDecodeError:
                                # Not JSON, just include as text
                                error_details = f"{error_details} (output: {full_response})"
                        raise Exception(f"Execution error: {error_details}")
                    
                    return full_response
                else:
                    raise Exception(f"Unexpected response type: {response_type}")
                    
            except asyncio.TimeoutError:
                raise Exception(f"Execution timeout after {timeout}s (no response received)")
                    
        finally:
            # Clean up pending response
            if msg_id in self.pending_responses:
                del self.pending_responses[msg_id]
            if msg_id in self.response_buffers:
                del self.response_buffers[msg_id]
    
    async def run_test(self, test_code: str, timeout: float = 10.0) -> TestResult:
        """
        Run a test and return structured result.
        
        Wraps test code to capture exceptions and return JSON result.
        Test code should NOT print anything - only the final JSON result is output.
        
        Args:
            test_code: Python test code to execute (should not print)
            timeout: Execution timeout in seconds
            
        Returns:
            TestResult with status and details
        """
        # Wrap test code to return JSON result
        # Indent test_code to be inside the try block (4 spaces)
        indented_test_code = '\n'.join('    ' + line if line.strip() else line 
                                       for line in test_code.split('\n'))
        
        wrapped_code = """import json
try:
""" + indented_test_code + """
    result = {"status": "pass", "message": "Test passed"}
except AssertionError as e:
    result = {"status": "fail", "error": str(e), "error_type": "AssertionError"}
except Exception as e:
    result = {"status": "error", "error": str(e), "error_type": type(e).__name__}
print(json.dumps(result))
"""
        
        try:
            response = await self.execute_m2m(wrapped_code, timeout=timeout)
            if not response or not response.strip():
                # No output - might indicate the code didn't execute or produce output
                return TestResult(
                    status='error',
                    error="No output received from test execution",
                    error_type='NoOutput'
                )
            
            # Response should be pure JSON (test code must not print)
            result_dict = json.loads(response.strip())
            return TestResult(**result_dict)
        except json.JSONDecodeError as e:
            return TestResult(
                status='error',
                error=f"Failed to parse result: {e}",
                error_type='JSONDecodeError',
                data=response if 'response' in locals() else None
            )
        except Exception as e:
            error_msg = str(e)
            # If it's an execution error from execute_m2m, preserve the details
            return TestResult(
                status='error',
                error=error_msg,
                error_type=type(e).__name__
            )
    
    async def execute_terminal(self, code: str, timeout: float = 10.0) -> List[str]:
        """
        Execute code on terminal channel and collect output.
        
        Args:
            code: Python code to execute
            timeout: Execution timeout in seconds
            
        Returns:
            List of output lines
        """
        if not self.authenticated:
            raise Exception("Not authenticated")
        
        output_lines = []
        
        def terminal_handler(msg):
            if len(msg) >= 3 and msg[1] == OP_RES:
                output_lines.append(msg[2])
        
        self.channel_handlers[CH_TRM] = terminal_handler
        
        try:
            await self.send([CH_TRM, OP_EXE, code, 0])
            await asyncio.sleep(timeout)  # Wait for output
        finally:
            self.channel_handlers.pop(CH_TRM, None)
        
        return output_lines
    
    async def interrupt(self, channel: int = CH_TRM):
        """Send interrupt signal (Ctrl-C) to specified channel."""
        await self.send([channel, OP_INT])
    
    async def close(self):
        """Close WebSocket connection."""
        if self._receiver_task:
            self._receiver_task.cancel()
            try:
                await self._receiver_task
            except asyncio.CancelledError:
                pass
        
        if self.ws:
            await self.ws.close()
            self.ws = None
        
        self.authenticated = False
