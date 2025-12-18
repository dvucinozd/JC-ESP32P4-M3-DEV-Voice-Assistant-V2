# ESP32-P4 Voice Assistant - Production Firmware

**Lokalni AI Glasovni Asistent za Home Assistant na ESP32-P4 platformi.**

Ovo je napredni, produkcijski spreman firmware koji transformira ESP32-P4 u pametni zvučnik s podrškom za offline naredbe, prekidanje govora (barge-in) i duboku integraciju s Home Assistantom.

## 📋 Status Projekta

**Faza:** 🚀 **PRODUCTION READY (Completed)**  
**Verzija:** 3.0.0 (Modular Architecture + AFE + MultiNet)  
**Framework:** ESP-IDF v5.5  

### Ključne Značajke

*   **🗣️ Napredni Audio Engine (ESP-SR AFE):**
    *   **AI Wake Word:** Offline detekcija "Hi ESP" (WakeNet).
    *   **AEC & Barge-in:** Poništavanje jeke omogućuje prekidanje asistenta dok svira glazbu ili govori.
    *   **Noise Suppression:** Uklanjanje pozadinske buke za kristalno čist prijenos glasa.
*   **⚡ Offline Naredbe (MultiNet):**
    *   Lokalno prepoznavanje naredbi bez interneta (npr. "Turn on the light", "Play music", "Stop").
*   **🏠 Home Assistant Integracija:**
    *   **Assist Pipeline:** Puni dvosmjerni razgovor preko WebSocketa.
    *   **MQTT:** Kontrola uređaja, status senzora i dijagnostika.
*   **🎵 Multimedija:**
    *   Lokalni MP3 player (SD kartica).
    *   Glasovna kontrola playera ("Next song", "Stop").
*   **⏰ Alarmi i Timeri:**
    *   Lokalni alarmi spremljeni u trajnu memoriju (NVS).
    *   Timeri koji rade i bez WiFi-a.
*   **🛡️ System Hardening:**
    *   **Task Watchdog:** Automatski reset u slučaju smrzavanja audio sustava.
    *   **Safe Mode:** Zaštita od boot loop-a (pokreće samo WiFi/Web za oporavak).
    *   **Crash Reporting:** Slanje razloga rušenja (Panic/WDT) na Home Assistant dashboard.
*   **⚙️ Web Dashboard:**
    *   Konfiguracija osjetljivosti (WWD/VAD) i pregled logova u stvarnom vremenu (WebSerial).

---

## 🔧 Hardver

- **Board:** JC-ESP32P4-M3-DEV (Guition)
- **MCU:** ESP32-P4NRW32 (Dual-core RISC-V @ 400MHz, 32MB PSRAM)
- **WiFi:** ESP32-C6-MINI-1 (SDIO veza)
- **Audio:** ES8311 Codec + NS4150B Pojačalo

---

## 🚀 Brzi Start

### 1. Konfiguracija

Kopiraj `main/config.h.example` u `main/config.h` i unesi WiFi/HA podatke.
*Napomena:* Nakon prvog flashanja, postavke se spremaju u **NVS**. Možeš ih mijenjati kasnije putem Web Dashboarda bez rekompajliranja!

### 2. Build & Flash

Zbog promjene particijske tablice (za smještaj AI modela), prvo flashanje će **obrisati sve podatke**.

```cmd
# Windows (koristi build.bat helper)
.\build.bat
.\flash.bat

# Ili ručno
idf.py build
idf.py -p COMx flash monitor
```

### 3. Prvo Korištenje

1.  Pričekaj da LED postane **ZELENA** (Spreman).
2.  Reci **"Hi ESP"**. LED postaje **PLAVA** (Slušam).
3.  Reci naredbu (npr. *"Turn on the light"* ili *"Tell me a joke"*).

---

## 🗣️ Glasovne Naredbe

Sustav koristi hibridni pristup: prvo provjerava lokalne naredbe, a zatim šalje audio na Home Assistant.

### 🌐 Offline (Trenutno, Lokalno)
Rade i bez WiFi-a:
*   "Turn on the light" (Simulacija: LED Blue)
*   "Turn off the light" (Simulacija: LED Green)
*   "Play music" (Pokreće MP3 s SD kartice)
*   "Stop music" / "Stop"
*   "Next song" / "Previous song"

### ☁️ Online (Home Assistant)
Bilo što što tvoj HA Assist pipeline podržava:
*   "What time is it?"
*   "Turn on the kitchen lights"
*   "Set a timer for 5 minutes"

---

## 📂 Nova Arhitektura

Projekt je kompletno refaktoriran u modularni dizajn:

*   **`main.c`**: Samo inicijalizacija sustava.
*   **`voice_pipeline.c`**: Centralni "mozak" (State Machine). Upravlja tokom događaja (Wake -> Record -> Process -> Action).
*   **`audio_capture.c`**: Wrapper oko **ESP-SR AFE**. Upravlja mikrofonom, AEC-om, WakeNet-om i MultiNet-om.
*   **`ha_client.c`**: WebSocket klijent za streaming zvuka i primanje TTS-a.
*   **`sys_diag.c`**: Watchdog timer i zaštita od boot loop-a.
*   **`settings_manager.c`**: Upravljanje NVS konfiguracijom.
*   **`audio_ref_buffer.c`**: Ring buffer za AEC loopback (Barge-in).

---

## 🛡️ Sigurnost i Oporavak

### Safe Mode
Ako se uređaj resetira 3 puta zaredom unutar 1 minute (npr. zbog greške u audio driveru), ulazi u **Safe Mode**.
*   **Indikacija:** LED bljeska CRVENO.
*   **Funkcije:** Audio je isključen. Rade samo WiFi, WebSerial i OTA.
*   **Oporavak:** Spoji se na Web Dashboard i napravi OTA update ili resetiraj postavke.

### Web Dashboard
Dostupan na `http://<IP-ADRESA>/`.
*   **Status:** IP, Uptime, Heap, WWD status.
*   **WebSerial:** Live logovi u pregledniku (`/webserial`).
*   **Config:** Promjena osjetljivosti (Threshold) i AGC-a u letu.

---

## 📦 Particije

Koristi se custom `partitions.csv` kako bi se osiguralo **4MB** prostora za AI modele (WakeNet/MultiNet).

| Name | Type | SubType | Size | Usage |
| :--- | :--- | :--- | :--- | :--- |
| nvs | data | nvs | 24K | Postavke, Alarmi |
| ota_0/1 | app | ota | 3M | Firmware |
| model | data | spiffs | **4M** | AI Modeli (srmodels.bin) |
| storage | data | spiffs | 2M | Web UI assets (opcionalno) |

---

## 📝 Zasluge

Razvijeno kao edukacijski projekt za demonstraciju snage ESP32-P4 čipa.
Koristi Espressif **ESP-IDF**, **ESP-SR** (AI Audio), **ESP-ADF** (Driveri) i **LVGL**.

**Autor:** Daniel (uz pomoć Gemini AI Agenta)  
**Datum:** Prosinac 2025.