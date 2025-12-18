/**
 * Audio Capture Module
 * Handles microphone input via ES8311 codec
 * With VAD (Voice Activity Detection) support
 */

#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include "esp_err.h"
//#include "vad.h" // Removed to avoid conflict with ESP-SR
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h> // Added

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration if needed, or just use void* for legacy config
// typedef struct vad_config_t vad_config_t; 

/**
 * VAD event types
 */
typedef enum {
  VAD_EVENT_SPEECH_START, // Speech detected
  VAD_EVENT_SPEECH_END,   // Speech ended (silence detected)
} audio_capture_vad_event_t;

/**
 * Audio capture mode
 */
typedef enum {
  CAPTURE_MODE_IDLE,      // Not capturing
  CAPTURE_MODE_WAKE_WORD, // Continuous wake word detection (lightweight)
  CAPTURE_MODE_RECORDING  // Active recording with VAD for STT (intensive)
} audio_capture_mode_t;

/**
 * @brief Callback for captured audio data
 *
 * @param audio_data PCM audio buffer (16-bit, 16kHz, mono)
 * @param length Length of audio data in bytes
 */
typedef void (*audio_capture_callback_t)(const uint8_t *audio_data,
                                         size_t length);

/**
 * @brief Callback for wake word detection audio feed
 *
 * @param audio_data PCM audio samples (int16_t array)
 * @param samples Number of samples
 */
typedef void (*audio_capture_wwd_callback_t)(const int16_t *audio_data,
                                             size_t samples);

/**
 * @brief Callback for VAD events
 *
 * @param event VAD event type
 */
typedef void (*audio_capture_vad_callback_t)(audio_capture_vad_event_t event);

/**
 * @brief Callback for Offline Command detection (MultiNet)
 * 
 * @param command_id ID of the recognized command
 * @param command_index Index of the command
 */
typedef void (*audio_capture_cmd_callback_t)(int command_id, int command_index);

/**
 * @brief Initialize audio capture
 *
 * @return ESP_OK on success
 */
esp_err_t audio_capture_init(void);

/**
 * @brief Register callback for offline commands
 */
void audio_capture_register_cmd_callback(audio_capture_cmd_callback_t callback);

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
 * @brief Stop capturing audio and wait for capture task to exit
 *
 * Safe to call from any task except the capture task itself (in that case it
 * behaves like audio_capture_stop()).
 *
 * @param timeout_ms Max time to wait for task exit (0 = no wait)
 * @return ESP_OK if stopped, ESP_ERR_TIMEOUT if timed out
 */
esp_err_t audio_capture_stop_wait(uint32_t timeout_ms);

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
esp_err_t audio_capture_enable_vad(const void *config,
                                   audio_capture_vad_callback_t callback);

/**
 * @brief Disable VAD
 */
void audio_capture_disable_vad(void);

/**
 * @brief Reset VAD state
 */
void audio_capture_reset_vad(void);

/**
 * @brief Start wake word detection mode
 *
 * Starts continuous audio capture and feeds to wake word detector.
 * Lightweight mode - only feeds audio to WWD without VAD or streaming.
 *
 * @param wwd_callback Function to call with audio for wake word detection
 * @return ESP_OK on success
 */
esp_err_t
audio_capture_start_wake_word_mode(audio_capture_wwd_callback_t wwd_callback);

/**
 * @brief Get current capture mode
 *
 * @return Current audio capture mode
 */
audio_capture_mode_t audio_capture_get_mode(void);

/**
 * @brief Enable AGC (Automatic Gain Control)
 *
 * @param target_level Target RMS amplitude (default: 4000)
 * @return ESP_OK on success
 */
esp_err_t audio_capture_enable_agc(uint16_t target_level);

/**
 * @brief Disable AGC
 */
void audio_capture_disable_agc(void);

/**
 * @brief Check if AGC is enabled
 *
 * @return true if AGC is enabled
 */
bool audio_capture_is_agc_enabled(void);

/**
 * @brief Get current AGC gain
 *
 * @return Current gain multiplier (1.0 = no gain)
 */
float audio_capture_get_agc_gain(void);

/**
 * @brief Set AGC target level dynamically
 *
 * @param target_level New target RMS level
 * @return ESP_OK on success
 */
esp_err_t audio_capture_set_agc_target(uint16_t target_level);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_CAPTURE_H */
