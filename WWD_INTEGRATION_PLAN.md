# Wake Word Detection Integration Plan

## Overview
Integration ESP-SR WakeNet9l sa postojećim voice assistant pipeline-om.

## Architecture Change

### Current Flow (VAD-triggered)
```
User speaks → VAD detects → Start recording → STT → Intent → TTS
```

### New Flow (Wake Word-triggered)
```
Always listening → Wake word detected → VAD Start → STT → Intent → TTS → Resume listening
```

## Implementation Steps

### 1. Modify `audio_capture.c`
Add two modes:
- **Continuous Listening Mode**: Feed audio to WWD, lightweight processing
- **Recording Mode**: Feed audio to VAD + STT streaming (existing)

### 2. Integrate in `mp3_player.c`
- Initialize WWD after codec init
- Register wake word callback
- On wake word detection: Start VAD + STT pipeline
- After TTS completes: Restart WWD

### 3. State Machine

```
┌─────────────────┐
│  IDLE/LISTENING │ ← Continuous wake word detection
└────────┬────────┘
         │ Wake word detected!
         v
┌─────────────────┐
│   RECORDING     │ ← VAD + STT streaming
└────────┬────────┘
         │ Speech end (VAD)
         v
┌─────────────────┐
│   PROCESSING    │ ← Intent recognition
└────────┬────────┘
         │ Intent matched
         v
┌─────────────────┐
│   SPEAKING      │ ← TTS playback
└────────┬────────┘
         │ TTS complete
         v
┌─────────────────┐
│  IDLE/LISTENING │ ← Resume wake word detection
└─────────────────┘
```

## Code Changes

### `mp3_player.c`
```c
// Add wake word callback
void on_wake_word_detected(wwd_event_t event, void *user_data) {
    if (event == WWD_EVENT_DETECTED) {
        ESP_LOGI(TAG, "🎤 Wake word detected - starting pipeline");
        // Start VAD + STT
        start_voice_pipeline();
    }
}

// In app_main(), after codec init:
wwd_config_t wwd_config = wwd_get_default_config();
wwd_config.callback = on_wake_word_detected;
wwd_config.user_data = NULL;
wwd_init(&wwd_config);
wwd_start();

// After TTS completes (in VAD END handler):
wwd_start();  // Resume listening
```

### `audio_capture.c`
```c
// Add mode enum
typedef enum {
    CAPTURE_MODE_WWD,      // Continuous listening for wake word
    CAPTURE_MODE_RECORDING // Active recording for STT
} capture_mode_t;

// In capture task:
if (mode == CAPTURE_MODE_WWD) {
    // Feed to WWD (lightweight)
    wwd_feed_audio(buffer, bytes_read);
} else if (mode == CAPTURE_MODE_RECORDING) {
    // Feed to VAD + stream to HA (existing)
    // ... existing code ...
}
```

## Configuration

### Memory Considerations
- WakeNet9l model size: ~100KB Flash
- Runtime RAM: ~30KB
- Total footprint acceptable for ESP32-P4 (16MB Flash, 32MB PSRAM)

### Power Considerations
- Continuous wake word detection: ~10-15 mA additional
- Acceptable for USB-powered device
- TODO: Add deep sleep mode for battery operation

## Testing Plan

1. ✅ Build with ESP-SR component
2. ✅ Test WakeNet initialization
3. Test wake word detection with default keyword ("Hi ESP")
4. Test integration with VAD pipeline
5. Test TTS → Wake word resume flow
6. Tune detection threshold for false positive/negative balance
7. Test in noisy environments

## Next Steps

1. Implement audio capture dual-mode
2. Add wake word callback in mp3_player.c
3. Test end-to-end flow
4. Optimize CPU usage
5. Document wake word customization process
