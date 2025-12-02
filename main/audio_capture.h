/**
 * Audio Capture Module
 * Handles microphone input via ES8311 codec
 * With VAD (Voice Activity Detection) support
 */

#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include "esp_err.h"
#include "vad.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * VAD event types
 */
typedef enum {
    VAD_EVENT_SPEECH_START,  // Speech detected
    VAD_EVENT_SPEECH_END,    // Speech ended (silence detected)
} audio_capture_vad_event_t;

/**
 * @brief Callback for captured audio data
 *
 * @param audio_data PCM audio buffer (16-bit, 16kHz, mono)
 * @param length Length of audio data in bytes
 */
typedef void (*audio_capture_callback_t)(const uint8_t *audio_data, size_t length);

/**
 * @brief Callback for VAD events
 *
 * @param event VAD event type
 */
typedef void (*audio_capture_vad_callback_t)(audio_capture_vad_event_t event);

/**
 * @brief Initialize audio capture
 *
 * @return ESP_OK on success
 */
esp_err_t audio_capture_init(void);

/**
 * @brief Start capturing audio
 *
 * @param callback Function to call with captured audio chunks
 * @return ESP_OK on success
 */
esp_err_t audio_capture_start(audio_capture_callback_t callback);

/**
 * @brief Stop capturing audio
 */
void audio_capture_stop(void);

/**
 * @brief Deinitialize audio capture
 */
void audio_capture_deinit(void);

/**
 * @brief Enable VAD (Voice Activity Detection)
 *
 * @param config VAD configuration
 * @param callback Function to call on VAD events (optional)
 * @return ESP_OK on success
 */
esp_err_t audio_capture_enable_vad(const vad_config_t *config, audio_capture_vad_callback_t callback);

/**
 * @brief Disable VAD
 */
void audio_capture_disable_vad(void);

/**
 * @brief Reset VAD state
 */
void audio_capture_reset_vad(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_CAPTURE_H */
