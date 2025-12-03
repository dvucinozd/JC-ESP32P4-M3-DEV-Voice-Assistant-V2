#!/usr/bin/env python3
"""
ESP32-P4 Voice Assistant Flash Script
Flashes firmware to board and starts monitor
"""

import subprocess
import sys
import os

COM_PORT = "COM13"

def main():
    print("="*60)
    print("ESP32-P4 Voice Assistant Flash")
    print("="*60)

    # Set environment
    idf_path = r"C:\Espressif\frameworks\esp-idf-v5.5"
    project_dir = r"D:\platformio\P4\esp32-p4-voice-assistant"

    # Change to project directory
    os.chdir(project_dir)

    print(f"Board: JC-ESP32P4-M3-DEV")
    print(f"Port: {COM_PORT}")
    print(f"Working directory: {os.getcwd()}\n")

    # Flash command
    flash_cmd = f'cmd.exe /c "call {idf_path}\\export.bat && idf.py -p {COM_PORT} flash monitor"'

    print("Starting flash process...")
    print("Make sure board is connected!\n")
    print("To exit monitor: Press Ctrl+]")
    print("-"*60)

    try:
        # Run flash
        result = subprocess.run(
            flash_cmd,
            shell=True,
            capture_output=False,
            text=True,
            cwd=project_dir
        )

        return result.returncode

    except KeyboardInterrupt:
        print("\n\nFlash interrupted by user")
        return 0
    except Exception as e:
        print(f"\n✗ ERROR: {e}")
        return 1

if __name__ == "__main__":
    sys.exit(main())
