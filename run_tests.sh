#!/bin/bash
# Parley Firmware Test Runner (Linux/macOS)
# Runs unit tests on native platform without ESP32

set -e

echo ""
echo "========================================"
echo "  PARLEY FIRMWARE TEST RUNNER"
echo "========================================"
echo ""

# Check if platformio is installed
if ! command -v pio &> /dev/null; then
    echo "ERROR: PlatformIO is not installed"
    echo "Install it with: pip install platformio"
    exit 1
fi

# Run tests for nodes firmware
echo "[1/2] Running nodes firmware tests..."
echo ""
cd firmware/nodes
pio test -e test
cd ../..

echo ""
echo "[2/2] Running gateway firmware tests..."
echo ""
cd firmware/gateway
pio test -e test
cd ../..

echo ""
echo "========================================"
echo "  ALL TESTS PASSED"
echo "========================================"
echo ""
