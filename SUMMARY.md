# ESP32-P4 Voice Assistant - Project Summary

## Project Location
Current workspace: `D:\AI\ESP32P4\JC-ESP32P4-M3-DEV-Voice-Assistant_NEW`

## Capabilities (Current)
- Home Assistant voice pipeline: wake word (WakeNet9 "Hi ESP") → VAD → STT/TTS via HA → playback
- MQTT auto-discovery with HA: sensors (RSSI/free memory/uptime), WWD switch, restart & test-TTS buttons, VAD/WWD tuning numbers
- Wake word confirmation beep and reliable codec reconfiguration between microphone/TTS paths
- WiFi via ESP32-C6 (ESP-Hosted), mDNS resolution, WebSocket HA client

## Build & Flash (Windows CMD or ESP-IDF PowerShell)
```cmd
cd D:\AI\ESP32P4\JC-ESP32P4-M3-DEV-Voice-Assistant_NEW
build.bat
flash.bat   rem uses COM13 by default; override with COM_PORT env
```

Manual commands:
```cmd
call C:\Espressif\frameworks\esp-idf-v5.5\export.bat
idf.py build
idf.py -p COM13 flash monitor
```

## Configuration (main/config.h - not committed)
- WiFi: `WIFI_SSID`, `WIFI_PASSWORD` (use your own values)
- Home Assistant: `HA_HOST`, `HA_HOSTNAME`, `HA_PORT`, `HA_TOKEN`
- MQTT: `MQTT_BROKER_URI`, `MQTT_USERNAME`, `MQTT_PASSWORD`, `MQTT_CLIENT_ID`

## Helpful Scripts
- `build.bat`, `flash.bat`, `quick_build.bat`, `quick_flash.bat`
- `build.py`, `simple_build.py`, `flash.py` (Python wrappers using local repo path)

## Troubleshooting Hints
- Use `idf.py -p <COM> monitor` for logs; exit with `Ctrl+]`
- If codec audio is silent, confirm `bsp_extra_codec_mute_set(false)` log appears
- For WiFi issues, verify ESP32-C6 ESP-Hosted firmware and SDIO wiring
- MQTT entities not showing: check broker URI/creds and see `MQTT_INTEGRATION.md`

## References
- `README.md` for full hardware/software details
- `MQTT_INTEGRATION.md`, `WIFI_IMPLEMENTATION.txt`, `WAKENET_SD_CARD_SETUP.md` for focused guides
