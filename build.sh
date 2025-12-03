#!/bin/bash
# ESP32-P4 Voice Assistant Build Script (Unix/Git Bash)

echo "========================================"
echo "ESP32-P4 Voice Assistant Build"
echo "========================================"

export IDF_PATH="/c/Espressif/frameworks/esp-idf-v5.5"

# Source ESP-IDF export script
echo "Setting up ESP-IDF environment..."
source "$IDF_PATH/export.sh"

# Navigate to project
cd /d/platformio/P4/esp32-p4-voice-assistant

# Show IDF version
echo ""
echo "IDF Version:"
idf.py --version

# Build project
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
