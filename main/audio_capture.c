/**
 * Audio Capture Implementation
 * Uses BSP board I2S functions to read from ES8311 microphone
 * With VAD (Voice Activity Detection) support
 */

#include "audio_capture.h"
#include "vad.h"
#include "config.h"
#include "bsp_board_extra.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_capture";

#define CAPTURE_BUFFER_SIZE 1024   // Samples per read (2048 bytes for 16-bit)

static TaskHandle_t capture_task_handle = NULL;
static audio_capture_callback_t capture_callback = NULL;
static audio_capture_vad_callback_t vad_callback = NULL;
static bool is_capturing = false;
static bool vad_enabled = false;
static vad_handle_t vad_handle = NULL;

/**
 * Capture task - continuously reads audio from I2S using BSP functions
 */
static void capture_task(void *arg)
{
    int16_t *buffer = (int16_t *)malloc(CAPTURE_BUFFER_SIZE * sizeof(int16_t));
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate capture buffer");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Capture task started (VAD: %s)", vad_enabled ? "enabled" : "disabled");

    int chunk_count = 0;
    bool speech_detected = false;

    while (is_capturing) {
        size_t bytes_read = 0;

        // Read audio data from I2S using BSP function
        esp_err_t ret = bsp_extra_i2s_read(buffer,
                                           CAPTURE_BUFFER_SIZE * sizeof(int16_t),
                                           &bytes_read,
                                           portMAX_DELAY);

        if (ret == ESP_OK && bytes_read > 0) {
            size_t num_samples = bytes_read / sizeof(int16_t);

            // Process through VAD if enabled
            if (vad_enabled && vad_handle != NULL) {
                vad_state_t vad_state = vad_process_frame(vad_handle, buffer, num_samples);

                // Notify VAD state changes
                if (vad_state == VAD_STATE_SPEAKING && !speech_detected) {
                    speech_detected = true;
                    if (vad_callback) {
                        vad_callback(VAD_EVENT_SPEECH_START);
                    }
                    ESP_LOGI(TAG, "Speech detected!");
                } else if (vad_state == VAD_STATE_END && speech_detected) {
                    if (vad_callback) {
                        vad_callback(VAD_EVENT_SPEECH_END);
                    }
                    ESP_LOGI(TAG, "Speech ended (duration: %lums)", vad_get_duration_ms(vad_handle));

                    // Stop immediately - don't send this chunk
                    is_capturing = false;
                    break;
                }

                // Check if VAD says to stop (redundant check, but keep for safety)
                if (vad_should_stop(vad_handle)) {
                    ESP_LOGI(TAG, "VAD auto-stop triggered");
                    is_capturing = false;
                    break;
                }
            }

            // Debug: Log audio statistics periodically
            if (chunk_count % 20 == 0) {  // Log every 20th chunk (~1.3 seconds)
                int non_zero = 0;
                int32_t peak = 0;
                for (size_t i = 0; i < num_samples; i++) {
                    if (buffer[i] != 0) non_zero++;
                    if (abs(buffer[i]) > peak) peak = abs(buffer[i]);
                }
                ESP_LOGD(TAG, "Chunk %d: samples=%d, peak=%ld, non-zero=%d",
                         chunk_count, num_samples, peak, non_zero);
            }
            chunk_count++;

            // Call callback with captured audio
            if (capture_callback) {
                capture_callback((const uint8_t *)buffer, bytes_read);
            }
        } else if (ret != ESP_OK) {
            ESP_LOGW(TAG, "I2S read failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    free(buffer);
    ESP_LOGI(TAG, "Capture task stopped (chunks: %d)", chunk_count);
    vTaskDelete(NULL);
}

esp_err_t audio_capture_init(void)
{
    ESP_LOGI(TAG, "Initializing audio capture (using BSP I2S)...");

    // BSP codec already initialized by bsp_extra_codec_init()
    // No need to create new I2S channel - just use existing one

    ESP_LOGI(TAG, "Audio capture initialized (16kHz, mono, 16-bit)");
    return ESP_OK;
}

esp_err_t audio_capture_start(audio_capture_callback_t callback)
{
    if (is_capturing) {
        ESP_LOGW(TAG, "Already capturing");
        return ESP_OK;
    }

    capture_callback = callback;
    is_capturing = true;

    // Create capture task
    BaseType_t task_ret = xTaskCreate(capture_task, "audio_capture", 4096, NULL, 5, &capture_task_handle);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create capture task");
        is_capturing = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Audio capture started");
    return ESP_OK;
}

void audio_capture_stop(void)
{
    if (!is_capturing) {
        return;
    }

    is_capturing = false;
    capture_callback = NULL;

    // Wait for task to finish
    if (capture_task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(100));  // Give task time to exit
        capture_task_handle = NULL;
    }

    ESP_LOGI(TAG, "Audio capture stopped");
}

void audio_capture_deinit(void)
{
    audio_capture_stop();

    if (vad_handle != NULL) {
        vad_deinit(vad_handle);
        vad_handle = NULL;
    }

    ESP_LOGI(TAG, "Audio capture deinitialized");
}

esp_err_t audio_capture_enable_vad(const vad_config_t *config, audio_capture_vad_callback_t callback)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "Invalid VAD config");
        return ESP_ERR_INVALID_ARG;
    }

    // Initialize VAD
    esp_err_t ret = vad_init(config, &vad_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize VAD");
        return ret;
    }

    vad_enabled = true;
    vad_callback = callback;

    ESP_LOGI(TAG, "VAD enabled");
    return ESP_OK;
}

void audio_capture_disable_vad(void)
{
    vad_enabled = false;
    vad_callback = NULL;

    if (vad_handle != NULL) {
        vad_deinit(vad_handle);
        vad_handle = NULL;
    }

    ESP_LOGI(TAG, "VAD disabled");
}

void audio_capture_reset_vad(void)
{
    if (vad_handle != NULL) {
        vad_reset(vad_handle);
        ESP_LOGD(TAG, "VAD reset");
    }
}
