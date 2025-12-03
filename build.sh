#!/bin/bash
# ESP32-P4 Voice Assistant Build Script (Unix/Git Bash)

echo "========================================"
echo "ESP32-P4 Voice Assistant Build"
echo "========================================"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IDF_PATH="${IDF_PATH:-/c/Espressif/frameworks/esp-idf-v5.5}"

echo "Setting up ESP-IDF environment..."
source "$IDF_PATH/export.sh"

cd "$SCRIPT_DIR"

echo ""
echo "IDF Version:"
idf.py --version

echo ""
echo "Building project..."
idf.py build

echo ""
echo "========================================"
if [ $? -eq 0 ]; then
    echo "Build SUCCESS!"
    echo "Ready to flash with: idf.py -p COM13 flash"
else
    echo "Build FAILED!"
    echo "Check errors above"
fi
echo "========================================"
