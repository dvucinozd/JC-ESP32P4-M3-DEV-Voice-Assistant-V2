# ESP32-P4 Voice Assistant - ESP-IDF Native Implementation

Lokalni glasovni asistent za Home Assistant baziran na ESP32-P4 platformi, implementiran u native ESP-IDF frameworku.

## 📋 Projektni Status

**Faza:** ✅ **FUNCTIONAL - Voice Assistant Working**
**Base:** JC-ESP32P4-M3-DEV mp3_player demo
**Framework:** ESP-IDF v5.5
**Target:** Home Assistant Voice Integration

**Current Status:**
- ✅ Audio capture (Microphone → PCM 16kHz)
- ✅ WiFi connectivity (ESP32-C6 via SDIO)
- ✅ Home Assistant WebSocket connection
- ✅ Speech-to-Text streaming
- ✅ Intent processing
- ✅ Text-to-Speech MP3 download
- ✅ MP3 decoding & playback (24kHz, clean audio)
- ✅ Voice Activity Detection (VAD) with auto-stop
- ✅ Codec mute/unmute management

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
I2S_MCLK:  GPIO14
I2S_BCLK:  GPIO12
I2S_LRCLK: GPIO13
I2S_DIN:   GPIO10 (Microphone)
I2S_DOUT:  GPIO9  (Speaker)

I2C_SDA:   GPIO7  (ES8311 Control)
I2C_SCL:   GPIO8
PA_EN:     GPIO11 (Amplifier Enable)
```

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
cd D:\platformio\P4\esp32-p4-voice-assistant
build.bat
```

Or manually:
```cmd
C:\Espressif\frameworks\esp-idf-v5.5\export.bat
cd D:\platformio\P4\esp32-p4-voice-assistant
idf.py build
```

### 2. Flash to Board

```cmd
flash.bat
```

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

---

## 📂 Project Structure

```
esp32-p4-voice-assistant/
├── main/
│   ├── CMakeLists.txt
│   ├── mp3_player.c          # Main application entry point
│   ├── wifi_manager.c        # WiFi connectivity (ESP32-C6 SDIO)
│   ├── ha_client.c           # Home Assistant WebSocket client
│   ├── audio_capture.c       # Microphone input (ES8311) + VAD integration
│   ├── tts_player.c          # TTS MP3 decoder & playback
│   ├── vad.c/vad.h           # Voice Activity Detection (RMS energy)
│   ├── config.h              # WiFi, HA credentials
│   └── Kconfig.projbuild
├── common_components/
│   ├── bsp_extra/            # Board Support Package (Audio drivers)
│   └── espressif__esp32_p4_function_ev_board/
├── managed_components/       # ESP-IDF managed dependencies
│   ├── chmorgan__esp-libhelix-mp3/    # MP3 decoder
│   ├── chmorgan__esp-audio-player/    # Audio player framework
│   └── espressif__esp_websocket_client/
├── build/                    # Build output (generated)
├── CMakeLists.txt
├── sdkconfig                 # ESP-IDF configuration
├── partitions.csv
├── build.bat                 # Windows build script
├── flash.bat                 # Windows flash script
└── README.md
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

### Phase 5: Advanced Features 🚧 TODO
- [ ] Wake word detection (TensorFlow Lite Micro)
- [ ] Audio preprocessing (noise reduction)
- [ ] Acoustic Echo Cancellation (AEC)
- [ ] Multi-wake word support
- [ ] Croatian language support (STT/TTS)
- [ ] Display integration (MIPI-DSI)
- [ ] Battery monitoring
- [ ] OTA updates

---

## 🛠️ Configuration

### WiFi & Home Assistant Setup

Edit `main/config.h`:

```c
// WiFi Configuration
#define WIFI_SSID "YourNetworkName"
#define WIFI_PASSWORD "YourPassword"

// Home Assistant Configuration
#define HA_HOSTNAME "homeassistant.local"  // or IP address
#define HA_PORT 8123
#define HA_USE_SSL false
#define HA_TOKEN "your_long_lived_access_token"
```

**Getting HA Access Token:**
1. Open Home Assistant → Profile (bottom left)
2. Scroll to "Long-Lived Access Tokens"
3. Click "Create Token"
4. Copy token to `config.h`

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
- Increase microphone gain in `bsp_board_extra.h` (default: 40.0 dB)
- Adjust VAD energy threshold in `vad.c` (default: 100)
- Check audio levels in monitor log

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

**Status:** ✅ **FULLY FUNCTIONAL** - Voice assistant with VAD and clean TTS audio
**Last Updated:** 2025-12-02

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
