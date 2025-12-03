#!/usr/bin/env python3
"""
ESP32-P4 Voice Assistant Build Script
Runs idf.py build in proper Windows environment
"""

import subprocess
import sys
import os

def main():
    print("="*60)
    print("ESP32-P4 Voice Assistant Build")
    print("="*60)

    # Set environment
    idf_path = r"C:\Espressif\frameworks\esp-idf-v5.5"
    project_dir = r"D:\platformio\P4\esp32-p4-voice-assistant"

    # Change to project directory
    os.chdir(project_dir)
    print(f"Working directory: {os.getcwd()}")

    # Build command (run in cmd.exe with IDF export)
    build_cmd = f'cmd.exe /c "call {idf_path}\\export.bat && idf.py build"'

    print("\nStarting build process...")
    print("This may take 5-10 minutes on first build...\n")

    try:
        # Run build
        result = subprocess.run(
            build_cmd,
            shell=True,
            capture_output=False,  # Show output in real-time
            text=True,
            cwd=project_dir
        )

        print("\n" + "="*60)
        if result.returncode == 0:
            print("✓ BUILD SUCCESS!")
            print("\nNext step: Flash to board")
            print("Run: python flash.py")
            print("  or: idf.py -p COM13 flash monitor")
        else:
            print("✗ BUILD FAILED!")
            print(f"Exit code: {result.returncode}")
            print("\nCheck errors above for details")
        print("="*60)

        return result.returncode

    except Exception as e:
        print(f"\n✗ ERROR: {e}")
        return 1

if __name__ == "__main__":
    sys.exit(main())
