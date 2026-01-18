#!/bin/bash
# Setup script for httpserver test environment

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "Setting up Python virtual environment for httpserver tests..."
echo

# Check Python version
if ! command -v python3 &> /dev/null; then
    echo "Error: python3 not found"
    exit 1
fi

PYTHON_VERSION=$(python3 --version)
echo "Using: $PYTHON_VERSION"
echo

# Create virtual environment if it doesn't exist
if [ ! -d "venv" ]; then
    echo "Creating virtual environment..."
    python3 -m venv venv
    echo "✓ Virtual environment created"
else
    echo "Virtual environment already exists"
fi

# Activate virtual environment
echo "Activating virtual environment..."
source venv/bin/activate

# Upgrade pip
echo "Upgrading pip..."
pip install --upgrade pip setuptools wheel

# Install requirements
echo "Installing test requirements..."
pip install -r requirements.txt

echo
echo "✓ Setup complete!"
echo
echo "To activate the virtual environment in your terminal:"
echo "  source venv/bin/activate"
echo
echo "To run tests:"
echo "  python test_http_basic.py"
echo "  python test_websocket.py"
echo "  python test_integration.py"
echo

