#!/usr/bin/env python3
"""
ESP32-P4 Voice Assistant Build Script
Runs idf.py build in proper Windows environment
"""

from pathlib import Path
import subprocess
import sys
import os


def main():
    print("=" * 60)
    print("ESP32-P4 Voice Assistant Build")
    print("=" * 60)

    script_dir = Path(__file__).resolve().parent
    project_dir = script_dir
    idf_path = Path(os.environ.get("IDF_PATH", r"C:\Espressif\frameworks\esp-idf-v5.5"))
    idf_export = idf_path / "export.bat"

    if not idf_export.exists():
        print(f"IDF export script not found at: {idf_export}")
        return 1

    os.chdir(project_dir)
    print(f"Working directory: {project_dir}")

    # Build command (run in cmd.exe with IDF export)
    build_cmd = f'cmd.exe /c "call \"{idf_export}\" && idf.py build"'

    print("\nStarting build process...")
    print("This may take a few minutes on first build...\n")

    try:
        result = subprocess.run(
            build_cmd,
            shell=True,
            capture_output=False,  # Show output in real-time
            text=True,
            cwd=project_dir,
        )

        print("\n" + "=" * 60)
        if result.returncode == 0:
            print("BUILD SUCCESS!")
            print("\nNext step: Flash to board")
            print("Run: python flash.py")
            print("  or: idf.py -p COM13 flash monitor")
        else:
            print("BUILD FAILED!")
            print(f"Exit code: {result.returncode}")
            print("\nCheck errors above for details")
        print("=" * 60)

        return result.returncode

    except Exception as exc:
        print(f"\nERROR: {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
