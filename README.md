# ESP32-P4 Voice Assistant (JC-ESP32P4-M3-DEV)

Firmware za **JC-ESP32P4-M3-DEV** koji pretvara ESP32‑P4 u lokalni glasovni asistent za **Home Assistant Assist Pipeline** (WebSocket) uz **MQTT HA Discovery** za upravljanje i status.

## Značajke

- Offline wake word (WakeNet9) na 16 kHz mono audio streamu (runtime podesiv prag).
- HA Assist pipeline preko WebSocket: STT/intent/TTS eventi, streaming audio u oba smjera.
- Lokalni MP3 player sa SD kartice + integracija sa voice pipeline (pauziranje/gašenje WWD tijekom glazbe).
- Lokalni timer fallback (start iz STT teksta) radi i kad HA ne podrzava timere.
- MQTT Home Assistant Discovery: senzori, prekidači i kontrole (LED, OTA, glasnoća, glazba).
- OTA update: URL ulaz + okidač iz Home Assistanta (MQTT) + LED status tijekom OTA.
- Safe Mode (boot‑loop zaštita) + watchdog + dijagnostika reset razloga.
- Web dashboard + WebSerial (real‑time logovi).

## Hardver

- Board: `JC-ESP32P4-M3-DEV` (ESP32‑P4 + ESP32‑C6 Wi‑Fi coprocessor preko SDIO).
- Audio codec: `ES8311`.
- RGB LED modul: `HW-478` (3 pina) na `GPIO45/46/47`.

### RGB LED (HW‑478) – važno

Ako je **common spojen na GND** (common‑cathode), LED je **active‑high** i radi s default postavkom.

- Pinovi: `R=GPIO45`, `G=GPIO46`, `B=GPIO47`, `COM -> GND`
- Ako koristiš **common‑anode** (COM -> 3V3), postavi `LED_ACTIVE_LOW=1` (vidi `main/led_status.h`).

## Konfiguracija (tajne ne idu u git)

`main/config.h` je u `.gitignore` i služi samo lokalno.

1. Kopiraj predložak: `main/config.h.example -> main/config.h`
2. Upisi Wi‑Fi, HA i MQTT postavke.
3. Na prvom bootu se vrijednosti spremaju u NVS (poslije se čitaju iz NVS).

## Build / Flash (Windows)

Preduvjet: aktiviran ESP‑IDF 5.5 environment.

- Build: `python build.py`
- Flash: `python flash.py -p COM13`
- Monitor: `idf.py -p COM13 monitor`
- Ako COM port ostane zaključan: `kill_monitor.bat` ili zatvoriti sve `idf.py monitor`/`esptool` procese.

OTA binarij: `build/esp32_p4_voice_assistant.bin`

## OTA (lokalni server + HA)

1. Pokreni lokalni server: `ota_server.bat` (ili `python -m http.server 8080` iz root foldera).
2. U Home Assistantu postavi `OTA URL` na `http://<PC_IP>:8080/build/esp32_p4_voice_assistant.bin`.
3. Pritisni `Start OTA`.

Tijekom OTA LED status je `OTA` (bijelo brzo pulsiranje).

## Home Assistant (MQTT Discovery entiteti)

Entiteti se pojave automatski (MQTT Discovery).

- Senzori: `VA Status`, `VA Response`, `WiFi Signal` (RSSI dBm), `IP Address`, `Firmware Version`, `Free Memory`, `Uptime`, `Network Type`, `Music State`, `OTA Status/Progress`, `SD Card Status`, `WebSerial Clients`
- Kontrole: `Wake Word Detection`, `Auto Gain Control`, `LED Status Indicator` (switch), `LED Brightness`, `Output Volume`, `AGC Target Level`, `WWD Detection Threshold`, `VAD` (threshold/silence/min/max)
- OTA: `OTA URL` (text), `Start OTA` (button)
- Test/utility: `Test TTS`, `Restart Device`, `Play Music`/`Stop Music`, `LED Test`, `Diagnostic Dump` (button)

`VA Status` i `VA Response` su namijenjeni i za prikaz na drugim uređajima (npr. ESPHome CYD ekran) preko HA entiteta.

## LED statusi

LED statusi su implementirani u `main/led_status.c` i vezani uz VA/OTA događaje.

- `IDLE`: zeleno (dim)
- `LISTENING`: plavo pulsiranje
- `PROCESSING`: žuto blinkanje
- `SPEAKING`: cijan brzo pulsiranje (od `tts-start`, prije download/playback)
- `OTA`: bijelo brzo pulsiranje
- `ERROR`: crveno brzo blinkanje (Safe Mode / error)
- `CONNECTING`: plavo “breathing”

## Troubleshooting

- LED ode u crveno nakon ~1 minute: uređaj je ušao u Safe Mode (boot‑loop zaštita). Provjeriti serijski log; `main/sys_diag.c` sada koristi worker task (NVS se više ne dira iz timer callback‑a).
- Ne vidiš MQTT entitete u HA: provjeri broker URI/cred u `main/config.h`/NVS i da je MQTT integracija u HA aktivna.
- Dupli MQTT entiteti nakon promjena imena: reload MQTT integraciju u HA ili obriši retain discovery topic-e na brokeru.
- Prejak zvuk: koristi `Output Volume` slider u HA (vrijednost se sprema u NVS).

## Pomoćne skripte

`help_scripts/` sadrži skripte za čitanje HA stanja/logova preko WebSocket API‑ja (token se uzima iz `main/config.h` lokalno). Najčešće: `list_ha_states_from_config.py` i `check_ha_logs_from_config.py`.

## Tehničke specifikacije

Detaljan popis tehnologija/komponenti je u `TECHNICAL_SPECIFICATIONS.md`.

## Licence / Zasluge

- ESP‑IDF / ESP‑SR / Home Assistant
