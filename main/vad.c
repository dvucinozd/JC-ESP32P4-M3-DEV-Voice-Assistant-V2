/**
 * Voice Activity Detection Implementation
 *
 * Energy-based VAD using RMS (Root Mean Square) energy calculation.
 */

#include "vad.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *TAG = "vad";

/**
 * VAD instance structure
 */
struct vad_instance {
    vad_config_t config;
    vad_state_t state;

    // Timing
    uint32_t total_frames;
    uint32_t speech_frames;
    uint32_t silence_frames;
    uint32_t frames_per_ms;

    // Energy tracking
    uint32_t current_energy;
    uint32_t energy_threshold;
};

/**
 * Calculate RMS energy of audio frame
 */
static uint32_t calculate_rms_energy(const int16_t *audio_data, size_t num_samples)
{
    if (audio_data == NULL || num_samples == 0) {
        return 0;
    }

    uint64_t sum_squares = 0;

    for (size_t i = 0; i < num_samples; i++) {
        int32_t sample = audio_data[i];
        sum_squares += sample * sample;
    }

    uint32_t mean_square = (uint32_t)(sum_squares / num_samples);
    uint32_t rms = (uint32_t)sqrt(mean_square);

    return rms;
}

esp_err_t vad_init(const vad_config_t *config, vad_handle_t *handle)
{
    if (config == NULL || handle == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    struct vad_instance *vad = (struct vad_instance *)malloc(sizeof(struct vad_instance));
    if (vad == NULL) {
        ESP_LOGE(TAG, "Failed to allocate VAD instance");
        return ESP_ERR_NO_MEM;
    }

    memcpy(&vad->config, config, sizeof(vad_config_t));
    vad->state = VAD_STATE_IDLE;
    vad->total_frames = 0;
    vad->speech_frames = 0;
    vad->silence_frames = 0;
    vad->current_energy = 0;
    vad->energy_threshold = config->speech_threshold;

    // Calculate frames per millisecond (assuming frame size is provided via num_samples)
    // This will be updated dynamically in process_frame based on actual frame size
    vad->frames_per_ms = config->sample_rate / 1000;

    *handle = vad;

    ESP_LOGI(TAG, "VAD initialized: threshold=%lu, silence=%lums, max_rec=%lums",
             config->speech_threshold,
             config->silence_duration_ms,
             config->max_recording_ms);

    return ESP_OK;
}

vad_state_t vad_process_frame(vad_handle_t handle, const int16_t *audio_data, size_t num_samples)
{
    if (handle == NULL || audio_data == NULL || num_samples == 0) {
        return VAD_STATE_IDLE;
    }

    struct vad_instance *vad = (struct vad_instance *)handle;

    // Calculate energy of current frame
    vad->current_energy = calculate_rms_energy(audio_data, num_samples);

    bool is_speech = (vad->current_energy > vad->energy_threshold);

    vad->total_frames++;

    // Log energy every 20 frames (~500ms at 16kHz with 512 samples/frame)
    if (vad->total_frames % 20 == 0) {
        ESP_LOGI(TAG, "Energy: %lu (threshold: %lu) - %s",
                 vad->current_energy, vad->energy_threshold,
                 is_speech ? "SPEECH" : "silence");
    }

    // Update frames_per_ms based on actual frame size (first frame)
    if (vad->total_frames == 1) {
        vad->frames_per_ms = vad->config.sample_rate / num_samples / 1000;
        if (vad->frames_per_ms == 0) {
            vad->frames_per_ms = 1; // Avoid division by zero
        }
    }

    // State machine
    switch (vad->state) {
        case VAD_STATE_IDLE:
            // Start listening
            vad->state = VAD_STATE_LISTENING;
            vad->silence_frames = 0;
            vad->speech_frames = 0;
            ESP_LOGD(TAG, "State: LISTENING");
            // Fall through

        case VAD_STATE_LISTENING:
            if (is_speech) {
                vad->speech_frames++;

                // Check if minimum speech duration reached
                uint32_t speech_ms = vad->speech_frames / vad->frames_per_ms;
                if (speech_ms >= vad->config.min_speech_duration_ms) {
                    vad->state = VAD_STATE_SPEAKING;
                    ESP_LOGI(TAG, "State: SPEAKING (energy=%lu)", vad->current_energy);
                }
            }
            break;

        case VAD_STATE_SPEAKING:
            if (is_speech) {
                vad->speech_frames++;
                vad->silence_frames = 0;
            } else {
                vad->silence_frames++;

                // Check if silence duration exceeded
                uint32_t silence_ms = vad->silence_frames / vad->frames_per_ms;
                if (silence_ms >= vad->config.silence_duration_ms) {
                    vad->state = VAD_STATE_SILENCE;
                    ESP_LOGI(TAG, "State: SILENCE detected (%lums)", silence_ms);
                    vad->state = VAD_STATE_END;
                    ESP_LOGI(TAG, "State: END - Recording complete");
                }
            }
            break;

        case VAD_STATE_SILENCE:
        case VAD_STATE_END:
            // Already ended
            break;
    }

    // Check max recording time
    uint32_t total_ms = vad->total_frames / vad->frames_per_ms;
    if (total_ms >= vad->config.max_recording_ms) {
        vad->state = VAD_STATE_END;
        ESP_LOGI(TAG, "State: END - Max recording time reached (%lums)", total_ms);
    }

    ESP_LOGD(TAG, "Frame: energy=%lu, threshold=%lu, state=%d",
             vad->current_energy, vad->energy_threshold, vad->state);

    return vad->state;
}

bool vad_should_stop(vad_handle_t handle)
{
    if (handle == NULL) {
        return true;
    }

    struct vad_instance *vad = (struct vad_instance *)handle;
    return (vad->state == VAD_STATE_END);
}

void vad_reset(vad_handle_t handle)
{
    if (handle == NULL) {
        return;
    }

    struct vad_instance *vad = (struct vad_instance *)handle;

    vad->state = VAD_STATE_IDLE;
    vad->total_frames = 0;
    vad->speech_frames = 0;
    vad->silence_frames = 0;
    vad->current_energy = 0;

    ESP_LOGD(TAG, "VAD reset");
}

vad_state_t vad_get_state(vad_handle_t handle)
{
    if (handle == NULL) {
        return VAD_STATE_IDLE;
    }

    struct vad_instance *vad = (struct vad_instance *)handle;
    return vad->state;
}

uint32_t vad_get_duration_ms(vad_handle_t handle)
{
    if (handle == NULL) {
        return 0;
    }

    struct vad_instance *vad = (struct vad_instance *)handle;

    if (vad->frames_per_ms == 0) {
        return 0;
    }

    return vad->total_frames / vad->frames_per_ms;
}

void vad_deinit(vad_handle_t handle)
{
    if (handle != NULL) {
        free(handle);
        ESP_LOGD(TAG, "VAD deinitialized");
    }
}
