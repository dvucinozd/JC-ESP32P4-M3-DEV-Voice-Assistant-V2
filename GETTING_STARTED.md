# Getting Started - ESP32-P4 Voice Assistant

Ovaj vodič će te provesti kroz kompletnu setup proceduru od nule do prvog testa.

---

## ✅ Preduvjeti

- [x] JC-ESP32P4-M3-DEV ploča
- [x] USB-C kabel (data-capable)
- [x] Zvučnik 4Ω ili 8Ω (2-3W)
- [x] ESP-IDF v5.5 instaliran u `C:\Espressif\`
- [x] Windows 10/11
- [x] Home Assistant (opciono za sada)

---

## 🚀 Step-by-Step Setup

### Korak 1: Verifikacija ESP-IDF Instalacije

Otvori **ESP-IDF PowerShell** ili Command Prompt:

```cmd
C:\Espressif\frameworks\esp-idf-v5.5\export.bat
idf.py --version
```

Očekivani output:
```
ESP-IDF v5.5.x
```

Ako nemaš ESP-IDF instaliran, download sa:
https://dl.espressif.com/dl/esp-idf/

---

### Korak 2: Hardverski Setup

#### A. Spoji Zvučnik

```
Board JST Connector → Speaker
- Pin 1 (RED)   → Speaker +
- Pin 2 (BLACK) → Speaker -
```

**⚠️ VAŽNO:** NE pokreći board bez zvučnika! Pojačalo može pregrijati.

#### B. Spoji USB

```
Board USB-C Port → PC USB Port
```

**Provjeri COM port u Device Manager:**
- Ploča bi trebala biti: **COM13** (USB/JTAG)
- Ako je drugačiji, ažuriraj `flash.bat` file

#### C. RGB LED Status Indicator (HW-478)

**Preporučeno:** Dodaj HW-478 RGB LED modul za vizualni status feedback:

```
HW-478 Module → ESP32-P4
VCC (Red wire)    → 3.3V
GND (Black wire)  → GND
R (Red LED)       → GPIO45
G (Green LED)     → GPIO46
B (Blue LED)      → GPIO47
```

**LED Status Codes:**
- 🟢 Green = Idle (čeka wake word)
- 🔵 Blue Pulsing = Listening (snima glas)
- 🟡 Yellow Blinking = Processing (STT)
- 🟣 Purple Pulsing = Connecting (WiFi/MQTT)
- ⚪ White Breathing = OTA Update
- 🔴 Red Blinking = Error

**Napomena:** LED je opcionalan - sistem će raditi i bez njega.

---

### Korak 3: Konfiguracija WiFi i Home Assistant

Kopiraj `main/config.h.example` u `main/config.h` i uredi credentials:

```c
// WiFi Configuration
#define WIFI_SSID "TvojaWiFiMreza"
#define WIFI_PASSWORD "TvojaLozinka"

// Home Assistant Configuration
#define HA_HOSTNAME "homeassistant.local"  // ili IP adresa
#define HA_PORT 8123
#define HA_USE_SSL false
#define HA_TOKEN "your_long_lived_access_token"

// MQTT Configuration
#define MQTT_BROKER_URI "mqtt://homeassistant.local:1883"
#define MQTT_USERNAME "mqtt_user"  // ili NULL
#define MQTT_PASSWORD "mqtt_password"  // ili NULL
```

**Dobijanje HA Access Token:**
1. Home Assistant → Profil (dolje lijevo)
2. "Long-Lived Access Tokens" sekcija
3. "Create Token"
4. Kopiraj u `config.h`

---

### Korak 4: Build Project

#### Opcija A: Koristi Batch Script (Najlakše)

```cmd
cd D:\AI\ESP32P4\esp32-p4-voice-assistant
build.bat
```

#### Opcija B: Ručno

```cmd
cd D:\AI\ESP32P4\esp32-p4-voice-assistant
C:\Espressif\frameworks\esp-idf-v5.5\export.bat
idf.py build
```

**Ako `idf.py build` padne na `UnicodeEncodeError` (PowerShell encoding):**
```powershell
chcp 65001
$env:PYTHONUTF8=1
$env:PYTHONIOENCODING='utf-8'
```

**Prvo build-anje će trajati 5-10 minuta** jer kompajlira ESP-IDF framework.

**Očekivani output:**
```
Project build complete. To flash, run:
 idf.py -p COM13 flash
```

---

### Korak 5: Flash na Board

#### Opcija A: Koristi Flash Script

```cmd
cd D:\AI\ESP32P4\esp32-p4-voice-assistant
flash.bat
```

#### Opcija B: Ručno

```cmd
idf.py -p COM13 flash monitor
```

**Serial Monitor:**
- Prikazuje logove u real-time
- Exit: `Ctrl + ]`

**Očekivani output:**
```
[INFO] ESP32P4 MIPI DSI LVGL
[INFO] SD card mount successfully
[INFO] Codec initialized
[INFO] Playing audio...
```

---

### Korak 6: Prvi Test - Voice Assistant

Sistem će automatski pokrenuti:

1. **WiFi konekciju** na ESP32-C6 koprocessor
2. **MQTT Home Assistant Discovery** - pojavi se kao "ESP32-P4 Voice Assistant"
3. **Wake Word Detection** - reci "Hi ESP" za aktivaciju
4. **Voice Pipeline** - VAD → STT → Intent → TTS

**Testiraj glasovnu komandu:**
1. Reci: **"Hi ESP"** (čut ćeš "beep" potvrdu)
2. LED postane plavi (pulsing) = snima
3. Reci: **"Turn on the lights"** (ili bilo koju HA komandu)
4. LED žuti (blinking) = procesiranje
5. Čuješ TTS odgovor iz zvučnika
6. LED zeleni = čeka novu komandu

**Opciono: Testiraj Music Player:**
- Stavi MP3 datoteke na SD karticu u `/music/` folder
- U Home Assistant: Media Player kontrole (play/pause/stop/volume)
- Wake word se automatski stopira tijekom reprodukcije glazbe

---

## 🔧 Troubleshooting

### Problem 1: Build Failed - "component not found"

```
ERROR: Component 'bsp_extra' not found
```

**Fix:**
```cmd
# Provjeri postoje li folderi:
dir D:\AI\ESP32P4\esp32-p4-voice-assistant\common_components
```

Ako `common_components/` ne postoji, repo je nepotpun (re-clone / provjeri da si skinuo sve foldere).

---

### Problem 2: Flash Failed - "No serial data received"

**Uzroci:**
- USB kabel nije data-capable (samo charging)
- Pogrešan COM port
- Board nije u Download mode

**Fix:**
1. Provjeri COM port u Device Manager
2. Koristi quality USB kabel
3. Pokušaj reset button dok flasha

---

### Problem 3: No Sound from Speaker

**Checklist:**
- [ ] Zvučnik correctly spojen? (check polarity)
- [ ] PA_EN pin enabled? (GPIO11 u config.h)
- [ ] Volume OK? (check codec settings)
- [ ] SD kartica ima MP3 datoteke?

**Debug:**
```cmd
idf.py -p COM13 monitor
```

Traži u logovima:
```
[INFO] Codec initialized ← Mora biti tu!
[ERROR] ... ← Ako vidiš errors, to je problem
```

---

### Problem 4: Board se resetira

**Uzroci:**
- Slab USB power (try 5V/3A adapter)
- Overheating (dodaj hladnjak!)
- Faulty firmware

**Fix:**
```cmd
# Clean build i retry:
idf.py fullclean
idf.py build flash
```

---

## 📊 Verify Installation Checklist

Nakon uspješnog flasha, provjeri:

- [ ] **Serial monitor** prikazuje boot logs
- [ ] **ES8311 codec** je inicijaliziran (vidi u logovima)
- [ ] **WiFi connected** - vidi RSSI u logovima
- [ ] **MQTT connected** - "Connected to MQTT broker"
- [ ] **Home Assistant** vidi "ESP32-P4 Voice Assistant" uređaj
- [ ] **Wake word detection** - reci "Hi ESP" i čuješ beep
- [ ] **RGB LED** pokazuje status (ako je spojen)
- [ ] **SD card** je montiran (ako je prisutan)
- [ ] **Board ne pregrijava** (touch test - warm, not hot)

---

## 🎯 Što Dalje?

### Testiranje Funkcionalnosti ✅

Trenutno možeš testirati:
- [x] **Wake word detection** - "Hi ESP"
- [x] **Voice commands** - bilo koja HA komanda
- [x] **Music player** - MP3 playback sa SD kartice
- [x] **LED feedback** - vizualni status
- [x] **MQTT controls** - svi parametri iz HA
- [x] **OTA updates** - bežične nadogradnje

### Napredne Funkcije

Eksperimentiraj sa:
- **VAD tuning** - podesi `vad_threshold` za bolju detekciju
- **WWD tuning** - podesi `wwd_threshold` za osjetljivost wake word
- **AGC tuning** - automatska kontrola pojačanja mikrofona
- **LED brightness** - prilagodi svjetlinu LED indikatora
- **Music player** - dodaj svoje MP3 datoteke
- **OTA updates** - bezžično flashaj nove verzije firmware-a

### Dokumentacija

Više informacija u:
- `README.md` - kompletna dokumentacija
- `MQTT_INTEGRATION.md` - MQTT entiteti i dashboard
- `WAKENET_SD_CARD_SETUP.md` - WakeNet model setup (flash default; SD optional)

---

## 🆘 Need Help?

### Debug Outputs

```cmd
# Full verbose logs:
idf.py -p COM13 monitor

# Save logs to file:
idf.py -p COM13 monitor > debug.log
```

### Check Hardware

```cmd
# Verify board connection:
esptool.py --port COM13 chip_id

# Read flash:
esptool.py --port COM13 read_flash 0x0 0x1000 flash_dump.bin
```

### ESP-IDF Resources

- [ESP-IDF Documentation](https://docs.espressif.com/projects/esp-idf/en/v5.5/)
- [ESP32-P4 Examples](https://github.com/espressif/esp-idf/tree/v5.5/examples)
- [Espressif Forum](https://esp32.com/)

---

## 📝 Development Notes

**Current Status:** ✅ Fully Functional Voice Assistant

**Implementirano:**
- ✅ WiFi connectivity (ESP32-C6 SDIO)
- ✅ MQTT Home Assistant Discovery
- ✅ Wake word detection (WakeNet9 "Hi ESP")
- ✅ Voice Activity Detection (VAD)
- ✅ Home Assistant Assist Pipeline integration
- ✅ TTS playback with codec stability
- ✅ RGB LED status indicator with effects
- ✅ OTA firmware updates
- ✅ Local music player (SD card MP3)
- ✅ Auto Gain Control (AGC)
- ✅ Runtime log level control

**Napredne mogućnosti:**
- Svi parametri podesivi iz Home Assistant
- Bezžične nadogradnje firmware-a
- Lokalno reproduciranje glazbe
- Vizualni feedback preko RGB LED-a

---

**Success!** 🎉

Ako vidiš "ESP32-P4 Voice Assistant" u Home Assistant-u i LED pokazuje status, sistem je potpuno funkcionalan!

Za detaljnije informacije o svim funkcijama, pogledaj `README.md` → Development Roadmap.

**Enjoy your voice assistant!** 🎤🤖
