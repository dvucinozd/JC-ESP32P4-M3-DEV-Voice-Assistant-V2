/**
 * Voice Activity Detection (VAD)
 *
 * Simple energy-based VAD for detecting speech vs silence.
 * Automatically stops recording after detecting silence.
 */

#ifndef VAD_H
#define VAD_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * VAD states
 */
typedef enum {
    VAD_STATE_IDLE,         // Not processing
    VAD_STATE_LISTENING,    // Waiting for speech
    VAD_STATE_SPEAKING,     // Speech detected
    VAD_STATE_SILENCE,      // Silence after speech
    VAD_STATE_END           // Recording complete
} vad_state_t;

/**
 * VAD configuration
 */
typedef struct {
    uint32_t sample_rate;           // Audio sample rate (Hz)
    uint32_t speech_threshold;      // Energy threshold for speech detection
    uint32_t silence_duration_ms;   // Silence duration to end recording (ms)
    uint32_t min_speech_duration_ms; // Minimum speech duration (ms)
    uint32_t max_recording_ms;      // Maximum recording time (ms)
} vad_config_t;

/**
 * VAD instance handle
 */
typedef struct vad_instance* vad_handle_t;

/**
 * @brief Initialize VAD
 *
 * @param config VAD configuration
 * @param[out] handle VAD handle
 * @return ESP_OK on success
 */
esp_err_t vad_init(const vad_config_t *config, vad_handle_t *handle);

/**
 * @brief Process audio frame through VAD
 *
 * @param handle VAD handle
 * @param audio_data PCM audio buffer (16-bit samples)
 * @param num_samples Number of samples in buffer
 * @return Current VAD state
 */
vad_state_t vad_process_frame(vad_handle_t handle, const int16_t *audio_data, size_t num_samples);

/**
 * @brief Check if recording should end
 *
 * @param handle VAD handle
 * @return true if recording should stop
 */
bool vad_should_stop(vad_handle_t handle);

/**
 * @brief Reset VAD state
 *
 * @param handle VAD handle
 */
void vad_reset(vad_handle_t handle);

/**
 * @brief Get current VAD state
 *
 * @param handle VAD handle
 * @return Current state
 */
vad_state_t vad_get_state(vad_handle_t handle);

/**
 * @brief Get recording duration in milliseconds
 *
 * @param handle VAD handle
 * @return Duration in ms
 */
uint32_t vad_get_duration_ms(vad_handle_t handle);

/**
 * @brief Deinitialize VAD
 *
 * @param handle VAD handle
 */
void vad_deinit(vad_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif /* VAD_H */
