# ESP32-P4 Voice Assistant (JC-ESP32P4-M3-DEV)

Firmware for **JC-ESP32P4-M3-DEV** that turns the ESP32-P4 into a local voice assistant for the **Home Assistant Assist Pipeline** (WebSocket) with **MQTT HA Discovery** for control and status.

## Features

- Offline wake word (WakeNet9) on a 16 kHz mono audio stream (runtime-adjustable threshold).
- HA Assist pipeline over WebSocket: STT/intent/TTS events, audio streaming both directions.
- Local MP3 player from SD card + integration with the voice pipeline (pause/disable WWD during music).
- Local timer fallback (start from STT text) works even when HA does not support timers.
- MQTT Home Assistant Discovery: sensors, switches, and controls (LED, OTA, volume, music).
- OTA update: URL input + trigger from Home Assistant (MQTT) + LED status during OTA.
- Safe Mode (boot-loop protection) + watchdog + reset reason diagnostics.
- Web dashboard + WebSerial (real-time logs).

## Hardware

- Board: `JC-ESP32P4-M3-DEV` (ESP32-P4 + ESP32-C6 Wi-Fi coprocessor over SDIO).
- Audio codec: `ES8311`.
- RGB LED module: `HW-478` (3 pins) on `GPIO45/46/47`.

### RGB LED (HW-478) - important

If **common is tied to GND** (common-cathode), the LED is **active-high** and works with the default config.

- Pins: `R=GPIO45`, `G=GPIO46`, `B=GPIO47`, `COM -> GND`
- If you use **common-anode** (COM -> 3V3), set `LED_ACTIVE_LOW=1` (see `main/led_status.h`).

## Configuration (secrets do not go in git)

`main/config.h` is in `.gitignore` and is local only.

1. Copy template: `main/config.h.example -> main/config.h`
2. Fill in Wi-Fi, HA, and MQTT settings.
3. On first boot values are stored in NVS (later read from NVS).

## Getting Started

1. Activate the ESP-IDF 5.5 environment.
2. Build the firmware: `python build.py`.
3. Flash the board: `python flash.py -p COM13` (replace with your port).
4. Open a monitor: `idf.py -p COM13 monitor` and wait for the ready message.
5. In Home Assistant, enable MQTT integration and verify the new entities appear; then set `Wake Word Detection` and `Output Volume` to your preference.

If you prefer OTA updates, see the OTA section below.

## Build / Flash (Windows)

Prereq: ESP-IDF 5.5 environment activated.

- Build: `python build.py`
- Flash: `python flash.py -p COM13`
- Monitor: `idf.py -p COM13 monitor`
- If the COM port stays locked: `kill_monitor.bat` or close all `idf.py monitor`/`esptool` processes.

OTA binary: `build/esp32_p4_voice_assistant.bin`

## OTA (local server + HA)

1. Start a local server: `ota_server.bat` (or `python -m http.server 8080` from the repo root).
2. In Home Assistant set `OTA URL` to `http://<PC_IP>:8080/build/esp32_p4_voice_assistant.bin`.
3. Press `Start OTA`.

During OTA, the LED status is `OTA` (white fast pulsing).

## Home Assistant (MQTT Discovery entities)

Entities appear automatically (MQTT Discovery).

- Sensors: `VA Status`, `VA Response`, `WiFi Signal` (RSSI dBm), `IP Address`, `Firmware Version`, `Free Memory`, `Uptime`, `Network Type`, `Music State`, `OTA Status/Progress`, `SD Card Status`, `WebSerial Requests`
- Controls: `Wake Word Detection`, `Auto Gain Control`, `LED Status Indicator` (switch), `LED Brightness`, `Output Volume`, `AGC Target Level`, `WWD Detection Threshold`, `VAD` (threshold/silence/min/max)
- OTA: `OTA URL` (text), `Start OTA` (button)
- Test/utility: `Test TTS`, `Restart Device`, `Play Music`/`Stop Music`, `LED Test`, `Diagnostic Dump` (button)

`VA Status` and `VA Response` are also intended for display on other devices (e.g., ESPHome CYD screen) via HA entities.

## LED statuses

LED statuses are implemented in `main/led_status.c` and tied to VA/OTA events.

- `IDLE`: green (dim)
- `LISTENING`: blue pulsing
- `PROCESSING`: yellow blinking
- `SPEAKING`: cyan fast pulsing (from `tts-start`, before download/playback)
- `OTA`: white fast pulsing
- `ERROR`: red fast blinking (Safe Mode / error)
- `CONNECTING`: blue breathing

## Troubleshooting

- LED turns red after ~1 minute: the device entered Safe Mode (boot-loop protection). Check the serial log; `main/sys_diag.c` now uses a worker task (NVS is no longer touched from the timer callback).
- No MQTT entities in HA: verify broker URI/credentials in `main/config.h`/NVS and that MQTT integration in HA is enabled.
- Duplicate MQTT entities after renaming: reload the MQTT integration in HA or clear retained discovery topics on the broker.
- Too loud audio: use the `Output Volume` slider in HA (value is stored in NVS).

## Helper scripts

`help_scripts/` contains scripts to read HA states/logs over the WebSocket API (token is read from `main/config.h` locally). Most used: `list_ha_states_from_config.py` and `check_ha_logs_from_config.py`.

## Technical specifications

Detailed list of technologies/components is in `TECHNICAL_SPECIFICATIONS.md`.

## License / Credits

- ESP-IDF / ESP-SR / Home Assistant
- License: Apache-2.0 (see `LICENSE`)
