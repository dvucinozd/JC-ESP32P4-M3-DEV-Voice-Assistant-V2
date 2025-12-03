#!/usr/bin/env python3
"""
Simple ESP-IDF build script for Windows
Directly runs idf.py build with proper environment
"""

from pathlib import Path
import subprocess
import sys
import os


def main():
    print("=" * 60)
    print("ESP32-P4 Voice Assistant - Simple Build")
    print("=" * 60)

    project_dir = Path(__file__).resolve().parent
    idf_path = Path(os.environ.get("IDF_PATH", r"C:\Espressif\frameworks\esp-idf-v5.5"))
    idf_export = idf_path / "export.bat"

    if not idf_export.exists():
        print(f"IDF export script not found at: {idf_export}")
        return 1

    os.chdir(project_dir)
    print(f"Working directory: {project_dir}\n")

    cmd = f'cmd.exe /c "call \"{idf_export}\" && idf.py build"'

    print("Starting build...\n")

    try:
        process = subprocess.Popen(
            cmd,
            shell=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            universal_newlines=True,
            cwd=project_dir,
        )

        for line in process.stdout:
            print(line, end="")

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

    except Exception as exc:
        print(f"\nERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
