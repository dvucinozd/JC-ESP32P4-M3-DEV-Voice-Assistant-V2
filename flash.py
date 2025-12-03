#!/usr/bin/env python3
"""
ESP32-P4 Voice Assistant Flash Script
Flashes firmware to board and starts monitor
"""

from pathlib import Path
import subprocess
import sys
import os

COM_PORT = os.environ.get("ESP32_COM_PORT", "COM13")


def main():
    print("=" * 60)
    print("ESP32-P4 Voice Assistant Flash")
    print("=" * 60)

    script_dir = Path(__file__).resolve().parent
    project_dir = script_dir
    idf_path = Path(os.environ.get("IDF_PATH", r"C:\Espressif\frameworks\esp-idf-v5.5"))
    idf_export = idf_path / "export.bat"

    if not idf_export.exists():
        print(f"IDF export script not found at: {idf_export}")
        return 1

    os.chdir(project_dir)

    print(f"Board: JC-ESP32P4-M3-DEV")
    print(f"Port: {COM_PORT}")
    print(f"Working directory: {project_dir}\n")

    flash_cmd = f'cmd.exe /c "call \"{idf_export}\" && idf.py -p {COM_PORT} flash monitor"'

    print("Starting flash process...")
    print("Make sure board is connected!")
    print("To exit monitor: Ctrl + ]")
    print("-" * 60)

    try:
        result = subprocess.run(
            flash_cmd,
            shell=True,
            capture_output=False,
            text=True,
            cwd=project_dir,
        )
        return result.returncode

    except KeyboardInterrupt:
        print("\n\nFlash interrupted by user")
        return 0
    except Exception as exc:
        print(f"\nERROR: {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
