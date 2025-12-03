@echo off
setlocal

if "%IDF_PATH%"=="" set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5"

if not exist "%IDF_PATH%\export.bat" (
    echo ESP-IDF not found at %IDF_PATH%
    exit /b 1
)

pushd "%~dp0"
call "%IDF_PATH%\export.bat"
idf.py build
popd
