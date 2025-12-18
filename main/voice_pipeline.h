#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Events that the pipeline can report
typedef enum {
    VOICE_EVENT_WAKE_DETECTED,
    VOICE_EVENT_LISTENING_START,
    VOICE_EVENT_LISTENING_END,
    VOICE_EVENT_PROCESSING_START,
    VOICE_EVENT_PROCESSING_END,
    VOICE_EVENT_TTS_START,
    VOICE_EVENT_TTS_END,
    VOICE_EVENT_ERROR
} voice_pipeline_event_t;

// Configuration structure
typedef struct {
    float wwd_threshold;
    uint32_t vad_speech_threshold;
    uint32_t vad_silence_ms;
    uint32_t vad_min_speech_ms;
    uint32_t vad_max_recording_ms;
    bool agc_enabled;
    uint16_t agc_target_level;
} voice_pipeline_config_t;

// Initialize the voice pipeline
esp_err_t voice_pipeline_init(void);

// Start the pipeline (Wake Word Detection)
esp_err_t voice_pipeline_start(void);

// Stop the pipeline completely
esp_err_t voice_pipeline_stop(void);

// Manually trigger "Wake" (e.g. from button)
void voice_pipeline_trigger_wake(void);

// Control Audio Playback (Music) interactions
void voice_pipeline_on_music_state_change(bool is_playing);

// Update configuration at runtime
esp_err_t voice_pipeline_update_config(const voice_pipeline_config_t *config);
void voice_pipeline_get_config(voice_pipeline_config_t *config);

// Status Getters
bool voice_pipeline_is_running(void); // WWD running?
bool voice_pipeline_is_active(void);  // Processing/Speaking?

// Test commands
void voice_pipeline_test_tts(const char *text);
void voice_pipeline_trigger_restart(void);
void voice_pipeline_trigger_alarm(int alarm_id);

#ifdef __cplusplus
}
#endif