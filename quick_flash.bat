@echo off
call C:\Espressif\frameworks\esp-idf-v5.5\export.bat
cd /d D:\platformio\P4\esp32-p4-voice-assistant
idf.py -p COM13 flash
