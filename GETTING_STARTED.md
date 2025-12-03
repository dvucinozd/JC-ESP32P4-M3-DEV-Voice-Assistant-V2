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

#### C. Opciono: LED Indikatori

Ako želiš dodati LED-ove za status:
```
GPIO15 → [220Ω resistor] → LED (Listening)
GPIO16 → [220Ω resistor] → LED (Processing)
```

---

### Korak 3: Konfiguracija WiFi Credentialsa

Uredi `main/config.h`:

```c
#define WIFI_SSID "TvojaWiFiMreza"
#define WIFI_PASSWORD "TvojaLozinka"
```

**⚠️ NAPOMENA:** Trenutno WiFi NIJE implementiran (ESP32-Hosted driver missing).
Prvo ćemo testirati samo audio funkcionalnost.

---

### Korak 4: Build Project

#### Opcija A: Koristi Batch Script (Najlakše)

```cmd
D:\AI\ESP32P4\JC-ESP32P4-M3-DEV-Voice-Assistant_NEW\build.bat
```

#### Opcija B: Ručno

```cmd
cd D:\AI\ESP32P4\JC-ESP32P4-M3-DEV-Voice-Assistant_NEW
C:\Espressif\frameworks\esp-idf-v5.5\export.bat
idf.py build
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
D:\AI\ESP32P4\JC-ESP32P4-M3-DEV-Voice-Assistant_NEW\flash.bat
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

### Korak 6: Prvi Test - MP3 Player

Trenutna verzija projekta (base: mp3_player demo) će:

1. **Montirati SD karticu** (ako je prisutna)
2. **Inicijalizirati ES8311 audio codec**
3. **Reproducirati MP3 datoteke** iz `/music/` foldera

**Testiraj zvuk:**
- Stavi MP3 datoteke na SD karticu u `/music/` folder
- Resetiraj board
- Trebao bi čuti muziku iz zvučnika!

Ako nemaš SD karticu - ne brini, to je samo za test. Nastavit ćemo s Voice Assistant implementacijom.

---

## 🔧 Troubleshooting

### Problem 1: Build Failed - "component not found"

```
ERROR: Component 'bsp_extra' not found
```

**Fix:**
```cmd
# Provjeri postoje li folderi:
dir D:\AI\ESP32P4\JC-ESP32P4-M3-DEV-Voice-Assistant_NEW\common_components
```

Ako ne postoje, kopiraj iz original demo:
```cmd
xcopy /E /I "D:\platformio\P4\JC-ESP32P4-M3-DEV\1-Demo\IDF-DEMO\NoDisplay\common_components" "D:\AI\ESP32P4\JC-ESP32P4-M3-DEV-Voice-Assistant_NEW\common_components"
```

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
- [ ] **SD card** je montiran (ako je prisutan)
- [ ] **Audio playback** radi (čuješ MP3)
- [ ] **Board ne pregrijava** (touch test - warm, not hot)

---

## 🎯 Što Dalje?

### Faza 1: Audio Verifikacija ✅ (Currently)

Trenutno testiraj:
- [x] Speaker output
- [ ] Microphone input (dodati capture test)
- [ ] Audio loopback (mic → speaker)

### Faza 2: WiFi Setup

Implementirati:
- ESP32-Hosted driver za ESP32-C6
- WiFi connection
- mDNS discovery

### Faza 3: Voice Assistant

Dodati:
- Wake word detection (TFLite model)
- VAD (Voice Activity Detection)
- WebSocket connection to Home Assistant
- STT/TTS integration

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

**Current Status:** Phase 1 - Audio Foundation

**Working:**
- ✅ Project structure
- ✅ Build system (ESP-IDF)
- ✅ ES8311 codec drivers (from demo)
- ✅ Basic audio playback

**TODO:**
- ⏳ Microphone capture
- ⏳ WiFi connectivity
- ⏳ Wake word detection
- ⏳ Home Assistant integration

---

**Success!** 🎉

Ako čuješ zvuk iz zvučnika, osnovni setup je uspješan!

Sljedeći korak: Implementacija microphone capture i Voice Assistant logike.

Prati progress u `README.md` → Development Roadmap sekciji.
