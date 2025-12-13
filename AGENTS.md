AGENT NOTES
===========

Context
-------
- Board: JC-ESP32P4-M3-DEV (ESP32-P4, ES8311 codec, SD card).
- Wake word: WakeNet9, model `wn9_hiesp`, 16 kHz mono, threshold 0.50.
- HA Assist pipeline via WebSocket (`ws://kucni.local:9000/api/websocket`), MQTT broker `mqtt://192.168.0.163:1883`.
- OTA binary: `build/esp32_p4_voice_assistant.bin`.
- OTA URL (local server): `http://<PC_IP>:8080/build/esp32_p4_voice_assistant.bin` (`ota_server.bat` prints the URL).

Recent fixes (Dec 2025)
-----------------------
- Wake word capture: mic path unmuted by removing unsupported `set_in_mute`, gain set before `esp_codec_dev_open`. Stereo interleaved is compacted to mono; WWD stats log peak/nz periodically.
- HA streaming: audio send timeout raised to 500 ms to reduce ws lock contention; audio chunks dropped (warn only) if HA socket is not ready; WWD resume delayed after TTS to avoid re-trigger/slowdown.
- Guard delays: 300 ms before WWD start + 200 ms after start; 600 ms delay after TTS before resuming WWD.

Build/Flash
-----------
- Build: `python build.py` (requires ESP-IDF 5.5 env). If shell lacks IDF activation, run the standard `export.bat`/`install.ps1` first.
- Flash: `python flash.py -p COMXX` (default COM13). If COM port is busy, close all monitors/esptool or replug USB (sometimes Windows requires a reboot to release the port).
- OTA: run `python -m http.server 8080` from repo root (or `ota_server.bat`); set URL to `http://<PC_IP>:8080/build/esp32_p4_voice_assistant.bin`, then trigger OTA via MQTT (`esp32p4/ota_url_input/set`, `esp32p4/ota_trigger/set = ON`) or HA button.

Runtime Tips
------------
- If WWD stats show `peak=0`, check codec open errors in log and ensure `bsp_extra_codec_set_fs` succeeds; verify SD model path only if CONFIG_MODEL_IN_SDCARD is enabled.
- I2S warnings “dma frame num is adjusted…” are benign (alignment to 256 frames).
- WebSocket disconnects: capture handler now ignores sends while disconnected; pipeline continues. If HA link is flaky, consider increasing `HA_SEND_AUDIO_TIMEOUT_MS` further (main/ha_client.c).
- TTS “slow” or re-trigger: ensured guard delays before/after WWD resume. If still slow, lengthen the delays in `AUDIO_CMD_RESUME_WWD` or `tts_playback_complete_handler`.

Files touched in latest changes
-------------------------------
- `common_components/bsp_extra/src/bsp_board_extra.c` – codec open flow, mic gain pre-open, no mute call.
- `main/audio_capture.c` – warn on zero-byte reads.
- `main/ha_client.c` – audio send timeout 500 ms.
- `main/main.c` – guard delays around WWD resume; drop audio send errors to warn; skip streaming if HA not connected.
- `ota_server.bat` – prints OTA URL pointing to `/build/esp32_p4_voice_assistant.bin`.

Known quirks
------------
- Windows ACL: previously `.git` had DENY on write; ensure permissions allow creating `.git/index.lock` before git add/commit.
- COM port locking: if `Access is denied` persists, replug board or reboot to release COMxx.
