#!/usr/bin/env python3
"""
Simple ESP-IDF build script for Windows
Directly runs idf.py build with proper environment
"""

import subprocess
import sys
import os

def main():
    print("=" * 60)
    print("ESP32-P4 Voice Assistant - Simple Build")
    print("=" * 60)

    project_dir = r"D:\platformio\P4\esp32-p4-voice-assistant"
    idf_path = r"C:\Espressif\frameworks\esp-idf-v5.5"

    # Change to project directory
    os.chdir(project_dir)
    print(f"Working directory: {os.getcwd()}\n")

    # Build command using CMD with proper environment
    # We use 'call' to run export.bat in the same shell session
    cmd = f'call "{idf_path}\\export.bat" && idf.py build'

    print("Starting build...\n")

    try:
        # Run build with live output
        process = subprocess.Popen(
            cmd,
            shell=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            universal_newlines=True,
            cwd=project_dir
        )

        # Print output in real-time
        for line in process.stdout:
            print(line, end='')

        # Wait for completion
        return_code = process.wait()

        print("\n" + "=" * 60)
        if return_code == 0:
            print("BUILD SUCCESS!")
            print("\nBinary location:")
            print("  build/esp32_p4_voice_assistant.bin")
        else:
            print("BUILD FAILED!")
            print(f"Exit code: {return_code}")
        print("=" * 60)

        return return_code

    except Exception as e:
        print(f"\nERROR: {e}", file=sys.stderr)
        return 1

if __name__ == "__main__":
    sys.exit(main())
