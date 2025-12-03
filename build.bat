@echo off
REM ESP32-P4 Voice Assistant Build Script

setlocal

echo ========================================
echo ESP32-P4 Voice Assistant Build
echo ========================================

if "%IDF_PATH%"=="" set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5"

if not exist "%IDF_PATH%\export.bat" (
    echo ESP-IDF not found at %IDF_PATH%
    exit /b 1
)

pushd "%~dp0"

call "%IDF_PATH%\export.bat"

echo.
echo IDF Version:
idf.py --version

echo.
echo Building project...
idf.py build

echo.
echo ========================================
echo Build complete!
echo ========================================

popd
pause
