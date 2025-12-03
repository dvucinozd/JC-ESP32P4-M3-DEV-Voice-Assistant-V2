# Wake Word Detection - Implementation Progress

## ✅ Completed Tasks

### 1. ESP-SR Component Integration
- ✅ Added `espressif/esp-sr: "^2.0.0"` to `main/idf_component.yml`
- ✅ Added `espressif__esp-sr` to CMakeLists.txt REQUIRES

### 2. Wake Word Detection Module (`wwd.c` / `wwd.h`)
- ✅ Created clean API for wake word detection
- ✅ Wraps ESP-SR WakeNet9l model
- ✅ Callback-based event system
- ✅ Configurable detection threshold

**Key Functions:**
```c
wwd_init(config)  // Initialize WakeNet
wwd_start()       // Start detection
wwd_feed_audio()  // Feed audio samples
wwd_stop()        // Stop detection
wwd_deinit()      // Cleanup
```

### 3. Audio Capture Dual-Mode Support
- ✅ Added `CAPTURE_MODE_WAKE_WORD` and `CAPTURE_MODE_RECORDING` enum
- ✅ Modified `capture_task()` to handle both modes:
  - **Wake Word Mode**: Lightweight - feeds audio to WWD callback only
  - **Recording Mode**: Intensive - VAD processing + STT streaming
- ✅ Added `audio_capture_start_wake_word_mode()` function
- ✅ Added `audio_capture_get_mode()` getter

**Mode Flow:**
```
IDLE → WAKE_WORD (always listening) → RECORDING (after wake word) → IDLE
```

## 🚧 In Progress

### 4. Integration in `mp3_player.c`
Need to add:
1. Include wwd.h header
2. Wake word callback function
3. Initialize WWD after codec init
4. Start WWD in wake word mode
5. On wake word detection: Start VAD + STT pipeline
6. After TTS completes: Restart WWD

**Pseudocode:**
```c
// Add wake word callback
void on_wake_word_detected(wwd_event_t event, void *user_data) {
    if (event == WWD_EVENT_DETECTED) {
        ESP_LOGI(TAG, "🎤 Wake word detected!");

        // Stop wake word detection
        audio_capture_stop();

        // Start VAD + STT pipeline
        start_voice_assistant_pipeline();
    }
}

// In app_main(), after codec init:
wwd_config_t wwd_config = wwd_get_default_config();
wwd_config.callback = on_wake_word_detected;
wwd_init(&wwd_config);
wwd_start();

// Start audio capture in wake word mode
audio_capture_start_wake_word_mode(wwd_feed_audio_wrapper);

// After TTS completes (in VAD END handler):
wwd_start();
audio_capture_start_wake_word_mode(wwd_feed_audio_wrapper);
```

## 📋 Remaining Tasks

### 5. Test & Debug
- [ ] Build with ESP-SR component (will download ~100MB dependencies)
- [ ] Test WakeNet initialization
- [ ] Test wake word detection with default keyword ("Hi ESP")
- [ ] Test full pipeline: Wake word → VAD → STT → TTS
- [ ] Test resume after TTS completes
- [ ] Tune detection threshold for false positive/negative balance

### 6. Optimization
- [ ] Monitor CPU usage in wake word mode
- [ ] Optimize memory usage
- [ ] Test in noisy environments
- [ ] Add timeout for wake word mode (prevent endless listening)

### 7. Documentation
- [ ] Update README.md with wake word feature
- [ ] Document wake word customization process
- [ ] Add troubleshooting section

## 🎯 Expected Behavior

```
System boots → Codec init → WWD init → Start wake word listening

User: "Hi ESP"
System: 🎤 Wake word detected!
        → Stop WWD
        → Start VAD
        → User speaks command
        → VAD detects END
        → Stream to HA STT
        → Intent processing
        → TTS response plays
        → Restart WWD
        → Back to wake word listening
```

## 📊 Memory Estimates

- WakeNet9l model: ~100KB Flash
- Runtime RAM: ~30KB
- Audio buffer overhead: ~4KB
- Total acceptable for ESP32-P4

## ⚠️ Known Issues

1. **ESP-SR Component Size**: First build will download ~100MB of dependencies (models, libraries)
2. **Build Time**: First build may take 5-10 minutes
3. **Wake Word Language**: Default models are English ("Hi ESP", "Alexa")
   - Custom Croatian wake word would require model training
4. **Continuous Listening**: ~10-15mA additional current draw
   - Acceptable for USB-powered, not for battery

## 🔗 References

- [ESP-SR GitHub](https://github.com/espressif/esp-sr)
- [ESP-Skainet Examples](https://github.com/espressif/esp-skainet)
- [WakeNet Documentation](https://docs.espressif.com/projects/esp-sr/en/latest/esp32/wake_word_engine/README.html)
