# ESP32-P4 Voice Assistant

## Project Overview

This project is a fully functional, local voice assistant firmware for the **ESP32-P4** (specifically targeting the **JC-ESP32P4-M3-DEV** board with an **ESP32-C6** WiFi coprocessor). It is designed to integrate seamlessly with **Home Assistant** using the Assist Pipeline.

The architecture is highly modular and robust, featuring offline capabilities, acoustic echo cancellation, and system hardening mechanisms suitable for 24/7 operation.

### Key Features

*   **Audio Processing (AFE):** Uses Espressif's ESP-SR Audio Front End for:
    *   **Wake Word Detection:** Offline detection of "Hi ESP".
    *   **Voice Activity Detection (VAD):** AI-based speech detection.
    *   **Acoustic Echo Cancellation (AEC):** Software loopback enables "Barge-in" (interrupting the assistant while it speaks/plays music).
    *   **MultiNet:** Offline command recognition (e.g., "Turn on light", "Play music").
*   **Connectivity:**
    *   **WiFi:** Managed via the ESP32-C6 coprocessor over SDIO.
    *   **Home Assistant:** Integrates via WebSocket (Assist Pipeline) for STT/TTS and MQTT for device control/status.
*   **Features:**
    *   **Local Music Player:** Plays MP3s from the SD card.
    *   **Alarms & Timers:** Local management backed by NVS.
    *   **Web Dashboard:** Built-in WebSerial interface for logs, status, and configuration.
    *   **OTA Updates:** Over-the-Air firmware updates.
*   **System Hardening:**
    *   **Task Watchdog:** Auto-reset if critical tasks hang.
    *   **Safe Mode:** Boot loop detection disables audio/heavy subsystems to allow recovery via Web/OTA.
    *   **Crash Reporting:** Reports reset reasons to Home Assistant.

## Building and Running

### Prerequisites

*   **ESP-IDF v5.5**: Ensure the environment is set up.
*   **Hardware**: ESP32-P4 board with ES8311 codec and ESP32-C6 WiFi module.

### Configuration

1.  **Credentials**: Copy `main/config.h.example` to `main/config.h` and populate your WiFi and Home Assistant credentials.
    *   *Note:* The system prioritizes settings stored in NVS. If NVS is empty (first run), it falls back to `config.h`. You can update settings later via the Web Dashboard.
2.  **Partitions**: The project uses a custom `partitions.csv` to allocate space for the large speech models (4MB model partition).

### Build Commands

**Windows (PowerShell/CMD):**

```powershell
# Build the project
.\build.bat

# Flash to device (Replace COMx with your port)
.\flash.bat
# Or manually:
idf.py -p COM3 flash monitor
```

**Manual (Cross-Platform):**

```bash
# Set target
idf.py set-target esp32p4

# Build
idf.py build

# Flash and Monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

## Architecture

The monolithic `main.c` has been refactored into a modular architecture:

*   **`main/voice_pipeline.c`**: The central "brain". Manages the state machine (Wake -> Recording -> Processing -> TTS) and coordinates other modules.
*   **`main/audio_capture.c`**: Handles audio input. Wraps the ESP-SR AFE, WakeNet, and MultiNet. Feeds cleaned audio to the pipeline.
*   **`main/ha_client.c`**: WebSocket client for the Home Assistant Assist API. Handles audio streaming and event parsing.
*   **`main/sys_diag.c`**: Watchdog timers and boot loop protection.
*   **`main/audio_ref_buffer.c`**: Circular buffer for AEC reference signal loopback.
*   **`main/webserial.c`**: HTTP server for the dashboard and logging.

### Audio Flow

1.  **Input**: I2S reads from ES8311 Microphone.
2.  **Reference**: Audio sent to the speaker is simultaneously written to `audio_ref_buffer`.
3.  **Processing**: `audio_capture` interleaves Mic and Reference data and feeds the AFE.
4.  **Detection**:
    *   **Wake Word**: AFE triggers a wake event internally.
    *   **Offline Command**: MultiNet checks for local commands.
    *   **Online Command**: If no local command/wake word, audio is streamed to HA (if connected).
5.  **Output**: TTS or Music is sent to the codec (and the reference buffer).

## Development Conventions

*   **Configuration**: Use `settings_manager` to read/write config to NVS. Avoid hardcoding credentials in new code.
*   **Inter-task Communication**: Use FreeRTOS queues or direct task notifications. Do not perform blocking operations in the audio feed/fetch tasks.
*   **Logging**: Use `ESP_LOGx` macros. The `webserial` module hooks into the log output to display it in the browser.
*   **Safety**: Critical loops should include `sys_diag_wdt_feed()` to prevent watchdog timeouts.
