# ESP32-P4 Voice Assistant - ESP-IDF Native Implementation

Lokalni glasovni asistent za Home Assistant baziran na ESP32-P4 platformi, implementiran u native ESP-IDF frameworku.

## 📋 Project Status

**Faza:** ✅ **FUNCTIONAL - Voice Assistant Working**  
**Base:** JC-ESP32P4-M3-DEV mp3_player demo  
**Framework:** ESP-IDF v5.5  
**Target:** Home Assistant Voice Integration

**Highlights (current build):**
- ✅ Wake word detection (WakeNet9 "Hi ESP") with confirmation beep and warmup skip
- ✅ Hands-free pipeline: wake -> VAD -> HA STT/intent -> TTS playback -> auto resume wake word mode
- ✅ **Voice-controlled timers** with Croatian/Cyrillic support ("namjesti timer na 10 sekundi")
- ✅ MQTT HA auto-discovery: sensors (wifi_rssi, free_memory, uptime), WWD switch, restart/test-TTS buttons, VAD/WWD tuning numbers
- ✅ WiFi via ESP32-C6 SDIO, mDNS hostname/IP fallback, HA WebSocket + TTS download
- ✅ Codec stability: set_fs reconfigure between microphone, beep tone, and TTS playback
- ✅ RGB LED status indicator (HW-478 on GPIO 45/46/47) with brightness control
- ✅ OTA firmware updates via HTTP server with Home Assistant controls
- ✅ Local music player with SD card support (play/pause/stop/next/previous/volume)

---

## 🔧 Hardware

- **Board:** JC-ESP32P4-M3-DEV (Guition)
- **MCU:** ESP32-P4NRW32 (Dual-core RISC-V @ 400MHz, 32MB PSRAM, 16MB Flash)
- **WiFi Coprocessor:** ESP32-C6-MINI-1 (WiFi 6, BLE 5)
- **Audio Codec:** ES8311 (Mono ADC/DAC)
- **Amplifier:** NS4150B (3W @ 4Ω)
- **Interface:** MIPI-DSI (optional display)

### Pin Configuration

**Audio (I2S + I2C):**
```
I2S_BCLK:  GPIO12
I2S_MCLK:  GPIO13
I2S_LRCLK: GPIO10
I2S_DIN:   GPIO48 (Microphone)
I2S_DOUT:  GPIO9  (Speaker)

I2C_SDA:   GPIO7  (ES8311 Control)
I2C_SCL:   GPIO8
PA_EN:     GPIO11 (Amplifier Enable)
```

**Napomena:** Ovaj projekt koristi pinout iz bundled BSP komponente (pogledaj
`common_components/espressif__esp32_p4_function_ev_board/include/bsp/esp32_p4_function_ev_board.h`).
`main/config.h.example` sadrži WiFi/HA/MQTT postavke; audio pinovi se ne čitaju iz tog fajla.

**SDIO (P4 ↔ C6 Communication):**
```
SDIO_D0:   GPIO39
SDIO_D1:   GPIO40
SDIO_D2:   GPIO41
SDIO_D3:   GPIO42
SDIO_CMD:  GPIO44
SDIO_CLK:  GPIO43
C6_RESET:  GPIO54 (optional)
```

**RGB LED Status Indicator (HW-478):**
```
LED_RED:   GPIO45
LED_GREEN: GPIO46
LED_BLUE:  GPIO47
```

LED Status Colors:
- 🟢 **Green:** Idle (ready for wake word)
- 🔵 **Blue Pulsing:** Listening (recording voice)
- 🟡 **Yellow Blinking:** Processing (STT/Intent)
- 🟣 **Purple Pulsing:** Connecting to WiFi/MQTT
- ⚪ **White Breathing:** OTA update in progress
- 🔴 **Red Fast Blink:** Error state

---

## 💻 Software Requirements

### ESP-IDF Installation

1. **Download ESP-IDF v5.5:**
   ```
   https://dl.espressif.com/dl/esp-idf/
   ```

2. **Windows Installation:**
   - Run ESP-IDF installer
   - Install to: `C:\Espressif\`
   - Select ESP-IDF v5.5.x
   - Install Python 3.11 environment

3. **Verify Installation:**
   ```cmd
   C:\Espressif\frameworks\esp-idf-v5.5\export.bat
   idf.py --version
   ```

---

## 🚀 Quick Start

### 1. Build Project

```cmd
cd D:\AI\ESP32P4\esp32-p4-voice-assistant
build.bat
```

Or manually:
```cmd
C:\Espressif\frameworks\esp-idf-v5.5\export.bat
cd D:\AI\ESP32P4\esp32-p4-voice-assistant
idf.py build
```

If you build from PowerShell and see `UnicodeEncodeError`, run:
```powershell
chcp 65001
$env:PYTHONUTF8=1
$env:PYTHONIOENCODING='utf-8'
```

### 2. Flash to Board

```cmd
flash.bat
```

`COM_PORT` env var overrides the default COM13 used in the script.

Or manually:
```cmd
idf.py -p COM3 flash monitor
```

**Note:** Replace `COM3` with your actual COM port.

### 3. Monitor Output

```cmd
idf.py -p COM3 monitor
```

Exit monitor: `Ctrl+]`

## 🌐 Web Dashboard + WebSerial

Web UI se pokreće kada uključiš `webserial_enabled` (MQTT switch). Nakon toga:

- **Dashboard:** `http://<device-ip>/`
- **WebSerial konzola:** `http://<device-ip>/webserial`

Dashboard prikazuje status (IP/uptime/heap/SD/HA/MQTT/WWD/AGC) i omogućava osnovno podešavanje:
- WWD threshold
- VAD threshold/silence/min/max
- AGC enable + target level

API endpointi (za integracije/skripte):
- `GET /api/status`
- `POST /api/config`
- `POST /api/action`

## 🛰 MQTT Home Assistant Integration

Device appears as "ESP32-P4 Voice Assistant" in Home Assistant with full auto-discovery.

**Sensors (10s updates):**
- `wifi_rssi` - WiFi signal strength
- `free_memory` - Available heap memory
- `uptime` - System uptime
- `agc_current_gain` - Current AGC gain level
- `webserial_clients` - Active WebSerial clients

**Switches:**
- `wwd_enabled` - Wake Word Detection on/off
- `webserial_enabled` - Web dashboard + WebSerial on/off
- `agc_enabled` - Auto Gain Control on/off
- `led_enabled` - RGB LED status indicator on/off

**Buttons:**
- `restart` - Reboot ESP32
- `test_tts` - Test TTS playback
- `ota_trigger` - Start OTA firmware update

**Number Controls:**
- `vad_threshold`, `vad_silence_duration`, `vad_min_speech`, `vad_max_recording` - VAD tuning
- `wwd_threshold` - Wake word detection sensitivity
- `agc_target_level` - AGC target level (dB)
- `led_brightness` - LED brightness (0-100%)

**Media Player:**
- `music_player` - Full control (play/pause/stop/next/previous/volume)
- Supports MP3 files from SD card `/music/` folder
- Auto-stops wake word detection during music playback

**Select Controls:**
- `log_level` - Runtime logging level (Error/Warn/Info/Debug/Verbose)

See `MQTT_INTEGRATION.md` for dashboard examples and topic references.

---

## 📂 Project Structure

```
esp32-p4-voice-assistant/
  main/
    CMakeLists.txt
    main.c                          # Main application entry point (renamed from mp3_player.c)
    mqtt_ha.c / mqtt_ha.h           # MQTT HA discovery + entities/controls
    beep_tone.c / beep_tone.h       # Wake confirmation tone
    wifi_manager.c / wifi_manager.h # WiFi connectivity (ESP32-C6 SDIO)
    ha_client.c / ha_client.h       # Home Assistant WebSocket/TTS + STT parser
    audio_capture.c / audio_capture.h # Microphone input (ES8311) + VAD
    wwd.c / wwd.h                   # Wake Word Detection (WakeNet9)
    tts_player.c / tts_player.h     # TTS MP3 decoder & playback
    vad.c / vad.h                   # Voice Activity Detection (RMS energy)
    led_status.c / led_status.h     # RGB LED status indicator with effects
    ota_update.c / ota_update.h     # OTA firmware update handler
    local_music_player.c / .h       # SD card music player with HA integration
    timer_manager.c / timer_manager.h # FreeRTOS timer/alarm manager with SNTP
    config.h.example                # Sample credentials (copy to config.h)
    Kconfig.projbuild
  common_components/
    bsp_extra/                      # Board Support Package (audio drivers)
    espressif__esp32_p4_function_ev_board/
  managed_components/               # ESP-IDF managed deps (esp-sr, mqtt, websocket…)
  build/                            # Build output (generated)
  build.bat / flash.bat             # Windows build/flash scripts
  build.py / flash.py               # Cross-platform build/flash utilities
  ota_server.bat                    # OTA HTTP server for firmware updates
  partitions.csv                    # Custom partition table (WakeNet model)
  sdkconfig                         # ESP-IDF configuration
  README.md
```

---

## 🔨 Development Roadmap

### Phase 1: Audio Foundation ✅ COMPLETED
- [x] ES8311 codec initialization
- [x] I2S audio playback (speaker)
- [x] Microphone input capture (16kHz PCM)
- [x] Audio streaming to HA
- [x] TTS MP3 decoding & playback

### Phase 2: WiFi Connectivity ✅ COMPLETED
- [x] ESP32-Hosted SDIO driver integration
- [x] ESP32-C6 coprocessor communication
- [x] WiFi station mode
- [x] mDNS service discovery (kucni.local)
- [x] Home Assistant connection

### Phase 3: Home Assistant Integration ✅ COMPLETED
- [x] WebSocket connection to HA
- [x] Assist Pipeline API integration
- [x] Binary audio streaming (with handler ID)
- [x] STT (Speech-to-Text) streaming
- [x] TTS (Text-to-Speech) MP3 download
- [x] Intent handling
- [x] Full conversation loop

### Phase 4: Voice Activity Detection ✅ COMPLETED
- [x] RMS energy-based VAD implementation
- [x] Auto-stop recording after 2s silence
- [x] Maximum recording duration (8s)
- [x] Dynamic energy threshold (100)
- [x] Configurable silence/speech detection

### Phase 5: Wake Word Detection ✅ COMPLETED
- [x] ESP-SR WakeNet9 integration
- [x] Flash partition for model storage (512KB)
- [x] "Hi ESP" wake word model
- [x] Dual-mode audio capture (WWD + Recording)
- [x] Automatic pipeline activation on wake word
- [x] MONO audio configuration fix

### Phase 6: MQTT & Remote Controls ✅ COMPLETED
- [x] MQTT HA auto-discovery (sensors, switch, buttons)
- [x] MQTT number controls for VAD/WWD tuning
- [x] Wake confirmation beep and TTS-complete resume handling

### Phase 7: LED Status Indicator ✅ COMPLETED
- [x] RGB LED driver with PWM control (LEDC)
- [x] Visual status feedback (idle/listening/processing/error)
- [x] Pulsing and blinking effects
- [x] MQTT brightness control (0-100%)
- [x] MQTT on/off switch
- [x] Proper task stack sizing (4096 bytes)

### Phase 8: OTA Updates ✅ COMPLETED
- [x] HTTP-based OTA update handler
- [x] MQTT trigger button and URL text input
- [x] OTA server scripts (Windows batch + `python -m http.server` option)
- [x] White breathing LED during OTA
- [x] Automatic reboot after successful update

### Phase 9: Local Music Player ✅ COMPLETED
- [x] SD card MP3 file iterator
- [x] Home Assistant media player entity
- [x] Play/pause/stop/next/previous controls
- [x] Volume control (0-100%)
- [x] Track position and duration reporting
- [x] Auto-stop wake word during music playback
- [x] Manual stop flag to prevent auto-resume

### Phase 10: Voice-Controlled Timers ✅ COMPLETED
- [x] FreeRTOS-based timer manager with SNTP synchronization
- [x] Croatian STT parser (text + numeric format support)
- [x] Cyrillic language support (тајмер, секунди, минут)
- [x] Text-based numbers 1-60 ("osam", "deset", "dvadeset")
- [x] Automatic wake word detection resume after timer completion
- [x] Audio feedback (confirmation beeps + timer finished beeps @ 90% volume)
- [x] TTS skip for successful timer commands
- [x] Intent callback architecture for HA conversation integration

### Phase 11: Advanced Features 🚧 TODO
- [ ] Voice-controlled alarms with time parsing
- [ ] Audio preprocessing (noise reduction)
- [ ] Acoustic Echo Cancellation (AEC)
- [ ] Multi-wake word support
- [ ] Display integration (MIPI-DSI)
- [ ] Battery monitoring
- [ ] Bluetooth speaker output

---

## ⏱️ Voice-Controlled Timers

Set countdown timers using natural Croatian voice commands - the device parses your speech locally and manages timers without relying on Home Assistant's built-in timer functionality.

### Supported Commands

**Croatian (Latin):**
- "Namjesti timer na 10 sekundi" → 10-second timer
- "Namjesti timer na pet minuta" → 5-minute timer
- "Namjesti tajmer na dvadeset sekundi" → 20-second timer
- "Namjesti timer na 1 minutu" → 1-minute timer

**Croatian (Cyrillic):**
- "Намести тајмер на 10 секунди" → 10-second timer
- "Намести тајмер на пет минута" → 5-minute timer

### Supported Number Formats

**Text-based (1-60):**
- jedan/jedna/jednu, dva/dvije, tri, četiri, pet, šest, sedam, osam, devet, deset
- jedanaest through devetnaest (11-19)
- dvadeset, trideset, četrdeset, pedeset, šezdeset (20, 30, 40, 50, 60)

**Numeric:**
- Any number (e.g., "10 sekundi", "5 minuta", "30 sekundi")

### Audio Feedback

- **2 quick beeps (1200Hz @ 90%):** Timer started successfully
- **3 beeps (1000Hz @ 90%):** Timer finished
- Wake word detection automatically resumes after timer completion

### How It Works

1. Say "Hi ESP" to wake the device
2. Say your timer command (e.g., "namjesti timer na 10 sekundi")
3. Hear 2 confirmation beeps → timer is running
4. After countdown: 3 beeps → timer finished
5. Device automatically returns to wake word mode

**Note:** The device parses Croatian timer commands directly from the speech-to-text transcript, bypassing Home Assistant's native timer system. This provides faster response times and works even when HA doesn't recognize timer intents.

---

## 🛠️ Configuration

### WiFi, Home Assistant & MQTT Setup

Copy `main/config.h.example` to `main/config.h`, then edit your credentials (file is gitignored):

```c
// WiFi Configuration
#define WIFI_SSID "YourNetworkName"
#define WIFI_PASSWORD "YourPassword"

// Home Assistant Configuration
#define HA_HOSTNAME "homeassistant.local"  // or IP address
#define HA_PORT 8123
#define HA_USE_SSL false
#define HA_TOKEN "your_long_lived_access_token"

// MQTT Home Assistant Discovery
#define MQTT_BROKER_URI "mqtt://homeassistant.local:1883"
#define MQTT_USERNAME "mqtt_user"  // or NULL for anonymous
#define MQTT_PASSWORD "mqtt_password"  // or NULL
#define MQTT_CLIENT_ID "esp32p4_voice_assistant"
```

**Getting HA Access Token:**
1. Open Home Assistant → Profile (bottom left)
2. Scroll to "Long-Lived Access Tokens"
3. Click "Create Token"
4. Copy token to `config.h`

### OTA Updates

**Method 1: Using OTA Server Script (Recommended)**

1. Build firmware: `build.bat`
2. Start OTA server on your PC:
   ```cmd
   ota_server.bat
   ```
   Or cross-platform (from repo root):
   ```bash
   python -m http.server 8080
   ```
3. In Home Assistant:
   - Set `ota_url` text entity to: `http://YOUR_PC_IP:8080/build/esp32_p4_voice_assistant.bin`
   - Press `ota_trigger` button
4. Watch LED turn white (breathing) during update
5. Device reboots automatically after successful update

**Method 2: Custom HTTP Server**

Host the binary file (`build/esp32_p4_voice_assistant.bin`) on any HTTP server and provide the URL to the device via MQTT.

**OTA Server Features:**
- Simple Python HTTP server on port 8080
- Serves firmware from `build/` folder
- Cross-platform (Windows batch or `python -m http.server`)
- No external dependencies

### ESP-IDF menuconfig

```cmd
idf.py menuconfig
```

**Key Settings:**

- **Component config → ESP32P4-specific**
  - CPU Frequency: 400 MHz
  - Enable PSRAM: Yes
  - PSRAM Speed: 80 MHz

- **Component config → FreeRTOS**
  - Tick rate (Hz): 1000

- **Component config → LWIP**
  - Max number of open sockets: 16

- **Partition Table**
  - Custom partition CSV file: `partitions.csv`

---

## 📡 ESP32-C6 Coprocessor Setup

ESP32-P4 nema integrirani WiFi - koristi C6 koprocesor preko SDIO-a.

### Provjera C6 Firmware-a

```cmd
esptool.py --port COM4 read_mac
```

### Flash ESP-Hosted-NG (ako potrebno)

1. **Download ESP-Hosted:**
   ```
   https://github.com/espressif/esp-hosted
   ```

2. **Build C6 firmware:**
   ```cmd
   cd esp-hosted/esp_hosted_ng/esp/esp_driver
   idf.py set-target esp32c6
   idf.py build
   ```

3. **Flash C6:**
   - Stavi GPIO9 (C6) na GND
   - Resetiraj board
   - Flash: `idf.py -p COM4 flash`
   - Ukloni GPIO9 jumper

---

## 🐛 Troubleshooting

### Build Errors

**Error: `component 'bsp_extra' not found`**
```
Ensure common_components/ folder exists and contains bsp_extra/
```

**Error: `CONFIG_SPIRAM not defined`**
```
Run: idf.py menuconfig
Enable: Component config → ESP32P4-specific → Support for external PSRAM
```

### Runtime Issues

**No sound from speaker:**
- Check speaker connection (JST connector)
- Verify PA_EN pin (GPIO11)
- Check I2S pins in main code
- **FIXED:** Codec mute issue - ensure `bsp_extra_codec_mute_set(false)` is called before TTS playback

**Choppy TTS audio:**
- **FIXED:** Increased TTS buffer from 32KB to 128KB
- **FIXED:** I2S channel re-initialization optimized (use `bsp_extra_codec_open_playback()`)

**WiFi not connecting:**
- Flash ESP-Hosted firmware to C6
- Verify SDIO pins
- Check C6 reset pin

**Board not detected:**
- Install USB drivers (CP210x or CH340)
- Check Device Manager for COM port
- Try different USB cable

**VAD not detecting speech:**
- Increase microphone gain in `common_components/bsp_extra/include/bsp_board_extra.h` (default: 55.0)
- Adjust VAD energy threshold in `vad.c` (default: 100)
- Check audio levels in monitor log

**Mic/WWD looks dead (`WWD audio stats ... peak=0 nz=0`):**
- Ensure you're running a recent firmware where `bsp_extra_codec_set_fs()` opens playback first and record last (prevents ES8311 ADC path from being disabled).
- Verify audio pinout matches the BSP header (`I2S_DIN` is `GPIO48` for this board profile).
- Check for `Failed to open record codec` / `Failed to open playback codec` errors before the stats logs.

---

## 📚 Reference Documentation

- [ESP32-P4 TRM](https://www.espressif.com/sites/default/files/documentation/esp32-p4_technical_reference_manual_en.pdf)
- [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/)
- [ES8311 Datasheet](https://dl.espressif.com/dl/schematics/Audio_ES8311.pdf)
- [ESP-Hosted Documentation](https://docs.espressif.com/projects/esp-hosted/en/latest/)
- [Home Assistant Assist API](https://developers.home-assistant.io/docs/voice/intent-recognition/)

---

## 🤝 Contributing

Projekt je trenutno u initial development fazi. Contributions are welcome nakon stabilizacije osnovne funkcionalnosti.

---

## 📝 License

MIT License - Open source za educational i development svrhe.

---

## 🙏 Credits

- Base project: JC-ESP32P4-M3-DEV mp3_player demo
- Framework: Espressif ESP-IDF v5.5
- Target platform: Home Assistant
- Documentation: Based on "ESP32-P4 za Voice Assistant AI.txt"

---

**Status:** ✅ **FULLY FUNCTIONAL** - Complete voice assistant with timers, music player, OTA, and LED feedback
**Last Updated:** 2025-12-13

---

## 🎯 How It Works

1. **Audio Capture:** ESP32-P4 captures audio from ES8311 microphone (16kHz, 16-bit, mono)
2. **Voice Activity Detection:** RMS energy-based VAD detects speech start/end automatically
3. **WiFi Upload:** Audio streamed to Home Assistant via WebSocket (binary frames with handler ID)
4. **Speech Recognition:** HA Assist Pipeline processes speech-to-text
5. **Intent Processing:** HA executes intent (e.g., "turn on lights")
6. **TTS Generation:** HA generates response using Google Translate TTS (~24kHz MP3)
7. **Download & Decode:** ESP32 downloads MP3 via HTTP, decodes using libhelix-mp3
8. **Playback:** Decoded PCM audio played through ES8311 speaker (codec unmuted automatically)

**Full conversation latency:** ~4-6 seconds (network dependent)

### Key Features

- **Automatic Speech Detection:** VAD automatically starts/stops recording based on speech detection
- **Clean Audio Playback:** 128KB TTS buffer eliminates choppy audio
- **Optimized I2S Management:** Separate playback channel initialization prevents audio glitches
- **Codec Mute Control:** Automatic unmute before TTS playback ensures audio output
