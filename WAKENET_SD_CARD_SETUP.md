# WakeNet9 Model Setup (Flash by default, SD optional)

This project uses **ESP-SR WakeNet9** for wake word detection (default model: `wn9_hiesp` = "Hi ESP").

## Default (recommended): Flash `model` partition

By default, WakeNet models are stored in the **flash** `model` partition (`partitions.csv`) and are flashed alongside the firmware (build produces `build/srmodels/srmodels.bin`).

Benefits:
- Works in **WiFi and Ethernet** modes
- No SD card required for wake word detection
- Avoids SDIO pin conflicts

## Optional: SD card models (Ethernet-only / no ESP32-hosted WiFi)

ESP-SR supports loading models from an SD card via `CONFIG_MODEL_IN_SDCARD`.

Important limitation in this project:
- When using **ESP32-hosted WiFi (SDIO)**, the SD card interface/pins conflict, so SD card models are **not compatible** with that WiFi mode.
- SD-based model loading is practical only for **Ethernet mode** (or when hosted WiFi is disabled).

## SD model directory structure

Each WakeNet9 model is a folder with 3 files:

```
/sdcard/srmodels/
  wn9_hiesp/
    _MODEL_INFO_
    wn9_data
    wn9_index
```

## Copying a model to SD

Models are located in the repo under:
`managed_components/espressif__esp-sr/model/wakenet_model/`

Example (copy "Hi ESP"):

PowerShell (SD card mounted as `E:`):
```powershell
New-Item -Path "E:\\srmodels\\wn9_hiesp" -ItemType Directory -Force
Copy-Item -Path "managed_components\\espressif__esp-sr\\model\\wakenet_model\\wn9_hiesp\\*" -Destination "E:\\srmodels\\wn9_hiesp\\"
```

## Enabling SD model mode in firmware

1) Enable SD-card model loading:
- `idf.py menuconfig` -> `Component config` -> `ESP Speech Recognition` -> enable `MODEL_IN_SDCARD` (`CONFIG_MODEL_IN_SDCARD`)

2) In SD model mode, `main/wwd.c` loads models from `/sdcard/srmodels` (compile-time).

3) Ensure SD card is mounted **before** WakeNet init:
- In this project SD mounting is typically tied to **Ethernet connectivity**; in WiFi(SDIO) mode SD is disabled.

## Troubleshooting

### "No models found" / "Failed to load models"
- Verify SD card is FAT32 and mounted as `/sdcard`
- Verify structure: `/sdcard/srmodels/wn9_xxx/{_MODEL_INFO_,wn9_data,wn9_index}`
- Ensure you are not using ESP32-hosted WiFi (SDIO); use Ethernet-only for SD model loading

## Notes

- WakeNet expects 16 kHz mono audio input
- SD card is only accessed during model discovery/initialization
