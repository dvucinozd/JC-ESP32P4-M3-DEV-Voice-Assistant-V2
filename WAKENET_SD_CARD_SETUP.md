# WakeNet9 SD Card Setup Guide

This guide explains how to set up WakeNet9 wake word detection models on an SD card for the ESP32-P4 Voice Assistant.

## Overview

WakeNet9 models are stored on the SD card to save flash space and allow easy model updates without reflashing the device.

## Model Structure

Each WakeNet9 model consists of 3 files in its own directory:
```
/sdcard/srmodels/
├── wn9_hiesp/          (example: "Hi ESP" wake word)
│   ├── _MODEL_INFO_    (model metadata)
│   ├── wn9_data        (model weights)
│   └── wn9_index       (model index)
```

## Available Models

The ESP-SR component includes many pre-trained WakeNet9 models for different wake words:

### English Wake Words
- `wn9_alexa` - "Alexa"
- `wn9_hiesp` - "Hi ESP"  ⭐ **Recommended**
- `wn9_computer_tts` - "Computer"
- `wn9_mycroft_tts` - "Mycroft"
- `wn9_jarvis_tts` - "Jarvis"

### Chinese Wake Words
- `wn9_hilexin` - "Hi Lexin"
- `wn9_nihaoxiaozhi` - "Ni Hao Xiao Zhi"
- `wn9_xiaoaitongxue` - "Xiao Ai Tong Xue"
- And many more...

All models are located in: `managed_components/espressif__esp-sr/model/wakenet_model/`

## SD Card Setup Instructions

### Step 1: Prepare SD Card
1. Format SD card as FAT32
2. Create directory structure:
   ```
   /sdcard/srmodels/
   ```

### Step 2: Copy Model Files
Choose a wake word model and copy it to the SD card:

**Example - Using "Hi ESP" wake word:**
```bash
# From project directory
mkdir -p /path/to/sdcard/srmodels/wn9_hiesp
cp managed_components/espressif__esp-sr/model/wakenet_model/wn9_hiesp/* /path/to/sdcard/srmodels/wn9_hiesp/
```

**Windows PowerShell:**
```powershell
# Assuming SD card is mounted as E:
New-Item -Path "E:\srmodels\wn9_hiesp" -ItemType Directory -Force
Copy-Item -Path "managed_components\espressif__esp-sr\model\wakenet_model\wn9_hiesp\*" -Destination "E:\srmodels\wn9_hiesp\"
```

### Step 3: Verify Files
Ensure the SD card has this structure:
```
E:\ (or /sdcard/)
└── srmodels/
    └── wn9_hiesp/
        ├── _MODEL_INFO_
        ├── wn9_data
        └── wn9_index
```

### Step 4: Insert SD Card
Insert the SD card into the ESP32-P4 board's SD card slot.

## Code Configuration

The wake word detection code in `main/wwd.c` is already configured to load models from `/sdcard/srmodels/`:

```c
// Loads models from SD card
srmodel_list_t *models = esp_srmodel_init("/sdcard/srmodels");

// Finds WakeNet9 model
char *model_name = esp_srmodel_filter(models, "wn9", NULL);
```

## Enable Wake Word Detection

To enable wake word detection in `main/mp3_player.c`, uncomment the WWD initialization:

```c
void app_main(void) {
    // ... other initialization ...

    // Enable Wake Word Detection
    ESP_LOGI(TAG, "Initializing Wake Word Detection...");
    wwd_config_t wwd_config = wwd_get_default_config();
    wwd_config.callback = on_wake_word_detected;
    wwd_config.user_data = NULL;
    ret = wwd_init(&wwd_config);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Starting wake word detection...");
        wwd_start();
        audio_capture_start_wake_word_mode(wwd_audio_feed_wrapper);
    } else {
        ESP_LOGE(TAG, "Failed to initialize wake word detection");
    }

    // ... rest of init ...
}
```

## Testing

1. Flash the firmware to ESP32-P4
2. Power on the device
3. Monitor serial output for wake word detection logs:
   ```
   I (xxx) wwd: Initializing Wake Word Detection...
   I (xxx) wwd: Loading models from SD card...
   I (xxx) wwd: Found WakeNet9 model: wn9_hiesp
   I (xxx) wwd: Wake Word Detection initialized successfully
   I (xxx) wwd: Starting wake word detection...
   ```

4. Say the wake word ("Hi ESP")
5. You should see:
   ```
   I (xxx) wwd: 🎤 Wake word detected!
   ```

## Troubleshooting

### "Failed to load models from SD card"
- Verify SD card is properly formatted (FAT32)
- Check directory structure is correct: `/sdcard/srmodels/wn9_xxx/`
- Ensure all 3 files are present (_MODEL_INFO_, wn9_data, wn9_index)
- Try different SD card if mount fails

### "WakeNet9 model not found on SD card"
- Verify model directory name starts with "wn9_"
- Check `_MODEL_INFO_` file exists and is readable
- Ensure files were copied completely (check file sizes)

### Wake word not detected
- Speak clearly and at normal volume
- Try adjusting detection threshold in wwd_config (default: 0.5)
- Check microphone is working (test with VAD-based activation first)
- Try different wake word model

### Low detection rate
- Increase threshold: `wwd_config.detection_threshold = 0.7f;`
- Use DET_MODE_95 (aggressive mode) - automatically set for threshold >= 0.95
- Reduce background noise
- Position microphone closer to sound source

## Model Information Format

The `_MODEL_INFO_` file contains model metadata:
```
wakeNet9_v1h24_Hi,ESP_3_0.63_0.635
```

Format: `model_type_version_wakeword_window_threshold1_threshold2`

## Adding Multiple Wake Words

To support multiple wake words simultaneously:
1. Copy multiple model directories to SD card:
   ```
   /sdcard/srmodels/
   ├── wn9_hiesp/
   ├── wn9_alexa/
   └── wn9_computer_tts/
   ```

2. The system will automatically detect all available models
3. Any detected wake word will trigger the callback

## Performance Notes

- WakeNet9 requires ~16kHz mono audio input
- Detection runs in real-time with minimal CPU overhead
- PSRAM is used for model storage during runtime
- SD card is only accessed during initialization

## References

- [ESP-SR Documentation](https://github.com/espressif/esp-sr)
- [ESP-Skainet Examples](https://github.com/espressif/esp-skainet)
- [WakeNet Wake Word Engine Documentation](https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/wake_word_engine/README.html)
- [Model Selection and Loading](https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/flash_model/README.html)

## Model Locations

All available models can be found at:
```
D:\AI\ESP32P4\JC-ESP32P4-M3-DEV-Voice-Assistant_NEW\managed_components\espressif__esp-sr\model\wakenet_model\
```
