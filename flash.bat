@echo off
REM ESP32-P4 Voice Assistant Flash Script

setlocal

echo ========================================
echo ESP32-P4 Voice Assistant Flash
echo ========================================

if "%IDF_PATH%"=="" set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5"
if "%COM_PORT%"=="" set "COM_PORT=COM13"

if not exist "%IDF_PATH%\export.bat" (
    echo ESP-IDF not found at %IDF_PATH%
    exit /b 1
)

pushd "%~dp0"

call "%IDF_PATH%\export.bat"

echo.
echo Flashing to board on %COM_PORT% (USB/JTAG)...
echo Board: JC-ESP32P4-M3-DEV
echo.
idf.py -p %COM_PORT% flash monitor

popd
pause
