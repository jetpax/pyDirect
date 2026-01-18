# Example: Basic pyDirect setup
# This demonstrates how to include pyDirect in a MicroPython build
#
# Usage:
#   make USER_C_MODULES=/path/to/pyDirect/examples/basic_setup.cmake

# Include the main pyDirect orchestrator
include(/path/to/pyDirect/micropython.cmake)

# Or include individual modules:
# include(/path/to/pyDirect/httpserver/micropython.cmake)
# include(/path/to/pyDirect/usbmodem/micropython.cmake)

# Example with custom options:
# cmake -DMODULE_PYDIRECT_HTTPSERVER=ON -DMODULE_PYDIRECT_USBMODEM=OFF ...

