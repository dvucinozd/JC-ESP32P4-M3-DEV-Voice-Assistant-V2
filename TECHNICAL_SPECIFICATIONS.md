# Tehničke specifikacije (projekt)

Ovaj dokument opisuje korištene tehnologije, protokole, komponente i arhitekturu firmware-a za **JC-ESP32P4-M3-DEV Voice Assistant**.

## Hardver

- Target board: `JC-ESP32P4-M3-DEV`
- MCU: `ESP32-P4` (RISC‑V, PSRAM)
- Wi‑Fi coprocessor: `ESP32‑C6` (ESP‑Hosted / WiFi Remote preko SDIO)
- Audio codec: `ES8311`
- RGB status LED (opcionalno): `HW‑478` (GPIO45/46/47)
- SD kartica (opcionalno): lokalna glazba; WakeNet modeli u flashu (SD mode je opcionalan i ovisi o konfiguraciji)

## Firmware platforma

- Framework: **ESP‑IDF** (projekt je konfiguriran za **ESP‑IDF 5.5.x**; vidi `sdkconfig.defaults`)
- RTOS: **FreeRTOS** (taskovi + event loop + timeri)
- Build: CMake (`CMakeLists.txt`, `main/CMakeLists.txt`)

## Jezici i alati

- C (glavnina firmware-a)
- Python skripte (build/flash i HA debug u `help_scripts/`)
- Windows batch helperi (`build.bat`, `flash.bat`, `ota_server.bat`, `kill_monitor.bat`)

## Komponente i biblioteke (ESP Component Manager)

Definirano u `main/idf_component.yml`:

- `espressif/esp-sr` – WakeNet9 + AFE (AEC/NS/AGC/VAD ovisno o konfiguraciji)
- `espressif/esp_wifi_remote` – WiFi preko ESP32‑C6 (ESP‑Hosted)
- `espressif/esp_websocket_client` – WebSocket klijent (Home Assistant Assist WS API)
- `espressif/mdns` – mDNS (npr. resolve hostname-a)
- `espressif/button` – button helperi
- `espressif/led_strip` – LED strip driver (opcionalno / buduća upotreba)

Ostale ovisnosti (vidi `main/CMakeLists.txt` `REQUIRES` / `PRIV_REQUIRES`):

- `mqtt` – ESP‑IDF MQTT client (MQTT Discovery + kontrolne teme)
- `json` / `cJSON` – parsiranje/generiranje JSON-a (MQTT discovery payloadi, HA eventi)
- `esp_http_server` – web dashboard + WebSerial
- `esp_http_client`, `app_update`, `esp_https_ota` – OTA update
- `nvs_flash` – NVS postavke i dijagnostika
- MP3/audio stack (managed components):
  - `chmorgan__esp-libhelix-mp3`
  - `chmorgan__esp-audio-player`
  - `chmorgan__esp-file-iterator`

## Protokoli i integracije

### Home Assistant (Assist Pipeline)

- Transport: WebSocket (`/api/websocket`) preko `esp_websocket_client`
- Tok: streaming audio prema HA + parsing eventa (npr. `tts-start`, `intent-end`)
- Objave u HA (za vanjske prikaze tipa ESPHome CYD): `va_status` i `va_response` (MQTT senzori)

### MQTT (Home Assistant Discovery)

- Discovery prefix: `homeassistant/.../config` (retain)
- State/command prefix: `esp32p4/<entity>/{state|set}`
- Tipovi entiteta: sensor, switch, number, text, button

## Audio arhitektura

- Ulaz: mikrofon preko I2S/codec-a, 16 kHz mono (WakeNet9 zahtjev)
- Obrada: AFE (AEC/NS/AGC/VAD) i wake word detekcija (WakeNet9)
- Izlaz: TTS playback preko codec-a; lokalna glazba (MP3) preko audio playera
- Upravljanje glasnoćom: runtime promjena + spremanje u NVS (HA slider `Output Volume`)

## LED status i PWM

- Driver: LEDC PWM (RGB kanali)
- Modovi: `IDLE`, `LISTENING`, `PROCESSING`, `SPEAKING`, `OTA`, `ERROR`, `CONNECTING`
- Konfiguracija polariteta:
  - `LED_ACTIVE_LOW=0` za common‑GND (common‑cathode, active‑high)
  - `LED_ACTIVE_LOW=1` za common‑3V3 (common‑anode, active‑low)

## OTA update

- URL se postavlja kroz HA entitet (MQTT `text`) i start kroz HA button (MQTT `button`)
- Download/flash: `esp_http_client` + `app_update`/`esp_https_ota` (omogućen i HTTP)
- LED status tijekom OTA: `LED_STATUS_OTA`

## Postavke i persistent storage

- NVS: `settings_manager` (Wi‑Fi, HA, MQTT, output volume)
- Safe Mode / boot-loop zaštita: `sys_diag` (boot_count, reset reason, watchdog)

## Web sučelje i debug

- HTTP server: status/dashboard + WebSerial log stream
- `help_scripts/`: skripte za čitanje HA state-a i logova preko WS API-ja (koriste lokalni `main/config.h`; tajne se ne commitaju)
