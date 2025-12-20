AGENT NOTES
===========

Context
-------
- Board: JC-ESP32P4-M3-DEV (ESP32-P4, ES8311 codec, SD card).
- Wake word: WakeNet9, model `wn9_hiesp`, 16 kHz mono, threshold 0.50-0.95 (runtime adjustable).
- HA Assist pipeline via WebSocket (`ws://<HA_IP>:9000/api/websocket`), MQTT broker `mqtt://<HA_IP>:1883`.
- OTA binary: `build/esp32_p4_voice_assistant.bin`.
- OTA URL (local server): `http://<PC_IP>:8080/build/esp32_p4_voice_assistant.bin` (`ota_server.bat` prints the URL).

Recent fixes (Dec 20, 2025)
---------------------------
### OTA + WebSerial reliability
- OTA now validates HTTP status, supports unknown `Content-Length`, logs errors clearly, and uses PSRAM stack fallback if needed.
- WebSerial HTTP header limit raised to 8192 to avoid `431 Request Header Fields Too Large` on large requests.
- OTA start failures now log error names for faster diagnosis.

### MQTT + HA entities
- `sw_version` uses `esp_app_get_description()->version` (include `esp_app_desc.h`).
- WebSerial metric renamed to `webserial_requests` (it counts log requests, not active clients).
- Legacy MQTT discovery cleanup clears old/duplicated entities on connect; update `legacy_discovery_topics` when renaming/removing entity IDs.
- Added `diagnostic_dump` button (MQTT) to emit `sys_diag_report_status`.

### Audio behavior
- Timer/alarm beep now plays at max volume and then restores the previous output volume.
- Guard against empty response text before indexing in voice pipeline response handling.

Recent fixes (Dec 17, 2025)
---------------------------
### LED Status Improvements
- **SPEAKING LED**: Cyan fast pulsing (300ms period) triggered on `tts-start` event, not `tts-end`.
- **OTA LED**: White fast pulsing (300ms period) during firmware updates.
- LED status log now includes task name for debugging: `LED status: X -> Y [task_name]`.
- Fixed LED timing: LED is set to SPEAKING *before* TTS download starts, stays cyan during playback.

### WWD Threshold Runtime Control
- Added `wwd_set_threshold(float threshold)` and `wwd_get_threshold()` to `wwd.c/wwd.h`.
- MQTT control uses runtime function instead of full WWD reinit.
- Fixed MQTT threshold range from 0.3-0.9 to valid 0.5-0.95.

### CYD Display Integration
- Fixed `va_response` MQTT sensor not being published.
- Speech text now extracted from `intent-end` event (`response.speech.plain.speech`).
- Both `va_status` and `va_response` sensors work for CYD display.

Previous fixes (Dec 2025)
-------------------------
- Wake word capture: mic path unmuted by removing unsupported `set_in_mute`, gain set before `esp_codec_dev_open`. Stereo interleaved is compacted to mono; WWD stats log peak/nz periodically.
- HA streaming: audio send timeout raised to 500 ms to reduce ws lock contention; audio chunks dropped (warn only) if HA socket is not ready; WWD resume delayed after TTS to avoid re-trigger/slowdown.
- Guard delays: 300 ms before WWD start + 200 ms after start; 600 ms delay after TTS before resuming WWD.

Build/Flash
-----------
- Versioning: before every build after code changes, bump the firmware name/version (start from `P4-HA-VA-0.0.1`).
- Build: `python build.py` (requires ESP-IDF 5.5 env). If shell lacks IDF activation, run the standard `export.bat`/`install.ps1` first.
- Flash: `python flash.py -p COMXX` (default COM13). If COM port is busy, close all monitors/esptool or replug USB (sometimes Windows requires a reboot to release the port).
- OTA: run `python -m http.server 8080` from repo root (or `ota_server.bat`); set URL to `http://<PC_IP>:8080/build/esp32_p4_voice_assistant.bin`, then trigger OTA via MQTT (`esp32p4/ota_url_input/set`, `esp32p4/ota_trigger/set = ON`) or HA button.
- If build fails with "Cannot find component list file", run `idf.py reconfigure` (after `export.bat`) then rebuild.

Runtime Tips
------------
- If WWD stats show `peak=0`, check codec open errors in log and ensure `bsp_extra_codec_set_fs` succeeds; verify SD model path only if CONFIG_MODEL_IN_SDCARD is enabled.
- I2S warnings "dma frame num is adjusted…" are benign (alignment to 256 frames).
- WebSocket disconnects: capture handler now ignores sends while disconnected; pipeline continues. If HA link is flaky, consider increasing `HA_SEND_AUDIO_TIMEOUT_MS` further (main/ha_client.c).
- TTS "slow" or re-trigger: ensured guard delays before/after WWD resume. If still slow, lengthen the delays in `AUDIO_CMD_RESUME_WWD` or `tts_playback_complete_handler`.

LED Status Reference
--------------------
| Status | Color | Effect | Period |
|--------|-------|--------|--------|
| IDLE | Green (dim) | Solid | - |
| LISTENING | Blue | Pulsing | 1000ms |
| PROCESSING | Yellow | Blinking | 500ms |
| SPEAKING | Cyan | Fast pulsing | 300ms |
| OTA | White | Fast pulsing | 300ms |
| ERROR | Red | Fast blinking | 200ms |
| CONNECTING | Blue | Breathing | 2000ms |

Files touched in latest changes
-------------------------------
- `main/led_status.c` – SPEAKING/OTA fast pulsing (300ms), debug task name in log.
- `main/ha_client.c` – LED set in `tts-start`, speech extraction from `intent-end` for CYD.
- `main/ota_update.c` – LED_STATUS_OTA on update start, restore to IDLE on failure.
- `main/wwd.c` / `main/wwd.h` – `wwd_set_threshold()` and `wwd_get_threshold()` for runtime control.
- `main/main.c` – MQTT threshold callback uses runtime function, range 0.5-0.95.

Known quirks
------------
- Windows ACL: previously `.git` had DENY on write; ensure permissions allow creating `.git/index.lock` before git add/commit.
- COM port locking: if `Access is denied` persists, replug board or reboot to release COMxx.
- `text` field in `tts-end` event is NULL in some HA versions; speech text must be extracted from `intent-end` instead.
