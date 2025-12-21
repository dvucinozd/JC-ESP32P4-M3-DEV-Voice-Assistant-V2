# Technical Specifications (Project)

This document describes the technologies, protocols, components, and firmware architecture for **JC-ESP32P4-M3-DEV Voice Assistant**.

## Hardware

- Target board: `JC-ESP32P4-M3-DEV`
- MCU: `ESP32-P4` (RISC-V, PSRAM)
- Wi-Fi coprocessor: `ESP32-C6` (ESP-Hosted / WiFi Remote over SDIO)
- Audio codec: `ES8311`
- RGB status LED (optional): `HW-478` (GPIO45/46/47)
- SD card (optional): local music; WakeNet models in flash (SD mode is optional and depends on configuration)

## Firmware platform

- Framework: **ESP-IDF** (project configured for **ESP-IDF 5.5.x**; see `sdkconfig.defaults`)
- RTOS: **FreeRTOS** (tasks + event loop + timers)
- Build: CMake (`CMakeLists.txt`, `main/CMakeLists.txt`)

## Languages and tools

- C (most of the firmware)
- Python scripts (build/flash and HA debug in `help_scripts/`)
- Windows batch helpers (`build.bat`, `flash.bat`, `ota_server.bat`, `kill_monitor.bat`)

## Components and libraries (ESP Component Manager)

Defined in `main/idf_component.yml`:

- `espressif/esp-sr` - WakeNet9 + AFE (AEC/NS/AGC/VAD depending on configuration)
- `espressif/esp_wifi_remote` - WiFi via ESP32-C6 (ESP-Hosted)
- `espressif/esp_websocket_client` - WebSocket client (Home Assistant Assist WS API)
- `espressif/mdns` - mDNS (e.g., hostname resolution)
- `espressif/button` - button helpers
- `espressif/led_strip` - LED strip driver (optional / future use)

Other dependencies (see `main/CMakeLists.txt` `REQUIRES` / `PRIV_REQUIRES`):

- `mqtt` - ESP-IDF MQTT client (MQTT Discovery + control topics)
- `json` / `cJSON` - JSON parsing/generation (MQTT discovery payloads, HA events)
- `esp_http_server` - web dashboard + WebSerial
- `esp_http_client`, `app_update`, `esp_https_ota` - OTA update
- `nvs_flash` - NVS settings and diagnostics
- MP3/audio stack (managed components):
  - `chmorgan__esp-libhelix-mp3`
  - `chmorgan__esp-audio-player`
  - `chmorgan__esp-file-iterator`

## Protocols and integrations

### Home Assistant (Assist Pipeline)

- Transport: WebSocket (`/api/websocket`) via `esp_websocket_client`
- Flow: streaming audio to HA + event parsing (e.g., `tts-start`, `intent-end`)
- Publications in HA (for external displays like ESPHome CYD): `va_status` and `va_response` (MQTT sensors)

### MQTT (Home Assistant Discovery)

- Discovery prefix: `homeassistant/.../config` (retain)
- State/command prefix: `esp32p4/<entity>/{state|set}`
- Entity types: sensor, switch, number, text, button

## Audio architecture

- Input: microphone over I2S/codec, 16 kHz mono (WakeNet9 requirement)
- Processing: AFE (AEC/NS/AGC/VAD) and wake word detection (WakeNet9)
- Output: TTS playback via codec; local music (MP3) via audio player
- Volume control: runtime changes + stored in NVS (HA slider `Output Volume`)

## LED status and PWM

- Driver: LEDC PWM (RGB channels)
- Modes: `IDLE`, `LISTENING`, `PROCESSING`, `SPEAKING`, `OTA`, `ERROR`, `CONNECTING`
- Polarity configuration:
  - `LED_ACTIVE_LOW=0` for common-GND (common-cathode, active-high)
  - `LED_ACTIVE_LOW=1` for common-3V3 (common-anode, active-low)

## OTA update

- URL is set via HA entity (MQTT `text`) and started via HA button (MQTT `button`)
- Download/flash: `esp_http_client` + `app_update`/`esp_https_ota` (HTTP also enabled)
- LED status during OTA: `LED_STATUS_OTA`

## Settings and persistent storage

- NVS: `settings_manager` (Wi-Fi, HA, MQTT, output volume)
- Safe Mode / boot-loop protection: `sys_diag` (boot_count, reset reason, watchdog)

## Web interface and debug

- HTTP server: status/dashboard + WebSerial log stream
- `help_scripts/`: scripts to read HA state and logs over WS API (use local `main/config.h`; secrets are not committed)
