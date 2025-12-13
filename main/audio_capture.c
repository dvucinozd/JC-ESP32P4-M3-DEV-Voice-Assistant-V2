/**
 * Audio Capture Implementation
 * Uses BSP board I2S functions to read from ES8311 microphone
 * With VAD (Voice Activity Detection) support
 */

#include "audio_capture.h"
#include "agc.h"
#include "bsp_board_extra.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "vad.h"

static const char *TAG = "audio_capture";

#define CAPTURE_BUFFER_SIZE 1024 // Samples per read (2048 bytes for 16-bit)

static TaskHandle_t capture_task_handle = NULL;
static audio_capture_callback_t capture_callback = NULL;
static audio_capture_vad_callback_t vad_callback = NULL;
static audio_capture_wwd_callback_t wwd_callback = NULL;
static bool is_capturing = false;
static bool vad_enabled = false;
static vad_handle_t vad_handle = NULL;
static audio_capture_mode_t capture_mode = CAPTURE_MODE_IDLE;
static SemaphoreHandle_t capture_stopped_sem = NULL;

// AGC state
static bool agc_enabled = false;
static agc_handle_t agc_handle = NULL;

/**
 * Capture task - continuously reads audio from I2S using BSP functions
 */
static void capture_task(void *arg) {
  int16_t *buffer = (int16_t *)malloc(CAPTURE_BUFFER_SIZE * sizeof(int16_t));
  if (buffer == NULL) {
    ESP_LOGE(TAG, "Failed to allocate capture buffer");
    if (capture_task_handle != NULL &&
        xTaskGetCurrentTaskHandle() == capture_task_handle) {
      capture_task_handle = NULL;
    }
    if (capture_stopped_sem) {
      xSemaphoreGive(capture_stopped_sem);
    }
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "Capture task started (VAD: %s)",
           vad_enabled ? "enabled" : "disabled");

  int chunk_count = 0;
  bool speech_detected = false;

  while (is_capturing) {
    size_t bytes_read = 0;

    // Read audio data from I2S using BSP function
    esp_err_t ret =
        bsp_extra_i2s_read(buffer, CAPTURE_BUFFER_SIZE * sizeof(int16_t),
                           &bytes_read, portMAX_DELAY);

    if (ret == ESP_OK && bytes_read > 0) {
      size_t num_samples = bytes_read / sizeof(int16_t);

      // Some codec/I2S configurations return 2-channel interleaved samples even when
      // we intend to operate in mono (the unused channel may be zeros or duplicates).
      // WakeNet/VAD expect a clean mono stream, so detect and compact when needed.
      if ((num_samples >= 8) && ((num_samples % 2) == 0)) {
        size_t pairs = num_samples / 2;
        size_t odd_near_zero = 0;
        size_t even_near_zero = 0;
        size_t dup_pairs = 0;

        for (size_t i = 0; i < pairs; i++) {
          int16_t left = buffer[i * 2];
          int16_t right = buffer[i * 2 + 1];

          if (right == 0 || right == 1 || right == -1) {
            odd_near_zero++;
          }
          if (left == 0 || left == 1 || left == -1) {
            even_near_zero++;
          }
          if (right == left || right == (int16_t)(left + 1) ||
              right == (int16_t)(left - 1)) {
            dup_pairs++;
          }
        }

        bool looks_like_zero_right = (odd_near_zero * 10 >= pairs * 9) &&
                                    (even_near_zero * 10 < pairs * 9);
        bool looks_like_dup_stereo = (dup_pairs * 10 >= pairs * 9);

        if (looks_like_zero_right || looks_like_dup_stereo) {
          for (size_t i = 0; i < pairs; i++) {
            buffer[i] = buffer[i * 2];
          }
          num_samples = pairs;
          bytes_read = num_samples * sizeof(int16_t);
        }
      }

      // MODE 1: Wake Word Detection Mode (lightweight)
      if (capture_mode == CAPTURE_MODE_WAKE_WORD) {
        // Optional: Apply AGC if enabled to improve wake word detection on low mic levels.
        if (agc_enabled && agc_handle != NULL) {
          agc_process(agc_handle, buffer, num_samples);
        }

        // Feed raw audio to wake word detector callback
        if (wwd_callback) {
          wwd_callback(buffer, num_samples);
        }

        // Light telemetry: log peak every ~1.5s to confirm audio activity in WWD mode.
        if ((chunk_count % 30) == 0) {
          int32_t peak = 0;
          int non_zero = 0;
          for (size_t i = 0; i < num_samples; i++) {
            int32_t s = buffer[i];
            if (s != 0) {
              non_zero++;
            }
            if (s < 0) s = -s;
            if (s > peak) peak = s;
          }
          ESP_LOGI(TAG, "WWD audio stats: chunk=%d samples=%d peak=%ld nz=%d",
                   chunk_count, (int)num_samples, (long)peak, non_zero);
        }
        // Skip VAD and streaming - just listen for wake word
        chunk_count++;
        continue;
      }

      // MODE 2: Recording Mode (intensive)
      // Apply AGC only during recording (for STT)
      if (agc_enabled && agc_handle != NULL) {
        agc_process(agc_handle, buffer, num_samples);
      }

      // MODE 2: Recording Mode (intensive) - Process through VAD if enabled
      if (vad_enabled && vad_handle != NULL) {
        vad_state_t vad_state =
            vad_process_frame(vad_handle, buffer, num_samples);

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
          ESP_LOGI(TAG, "Speech ended (duration: %lums)",
                   vad_get_duration_ms(vad_handle));

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
      if (chunk_count % 20 == 0) { // Log every 20th chunk (~1.3 seconds)
        int non_zero = 0;
        int32_t peak = 0;
        for (size_t i = 0; i < num_samples; i++) {
          if (buffer[i] != 0)
            non_zero++;
          if (abs(buffer[i]) > peak)
            peak = abs(buffer[i]);
        }
        ESP_LOGI(TAG, "🎤 Audio: chunk=%d, samples=%d, peak=%ld, non-zero=%d",
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
  if (capture_task_handle != NULL &&
      xTaskGetCurrentTaskHandle() == capture_task_handle) {
    capture_task_handle = NULL;
  }
  if (capture_stopped_sem) {
    xSemaphoreGive(capture_stopped_sem);
  }
  vTaskDelete(NULL);
}

static void ensure_capture_stopped_sem(void) {
  if (capture_stopped_sem == NULL) {
    capture_stopped_sem = xSemaphoreCreateBinary();
  }
  if (capture_stopped_sem) {
    // Ensure it's in the "given" state for the next start/stop cycle.
    xSemaphoreGive(capture_stopped_sem);
  }
}

esp_err_t audio_capture_init(void) {
  ESP_LOGI(TAG, "Initializing audio capture (using BSP I2S)...");

  // BSP codec already initialized by bsp_extra_codec_init()
  // No need to create new I2S channel - just use existing one

  ensure_capture_stopped_sem();
  ESP_LOGI(TAG, "Audio capture initialized (16kHz, mono, 16-bit)");
  return ESP_OK;
}

esp_err_t audio_capture_start(audio_capture_callback_t callback) {
  if (is_capturing) {
    ESP_LOGW(TAG, "Already capturing");
    return ESP_OK;
  }

  ensure_capture_stopped_sem();
  if (capture_stopped_sem) {
    // Mark "not stopped" for this run.
    xSemaphoreTake(capture_stopped_sem, 0);
  }

  // Reset codec to microphone configuration (16kHz MONO)
  // Use set_fs to properly close both playback and record channels
  extern esp_err_t bsp_extra_codec_set_fs(uint32_t rate, uint32_t bits_cfg,
                                          i2s_slot_mode_t ch);
  esp_err_t codec_ret = bsp_extra_codec_set_fs(16000, 16, I2S_SLOT_MODE_MONO);
  if (codec_ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to reset codec for recording: %s",
             esp_err_to_name(codec_ret));
  } else {
    ESP_LOGI(TAG, "Codec reset to 16kHz MONO for recording");
  }

  capture_callback = callback;
  capture_mode = CAPTURE_MODE_RECORDING; // Set to recording mode
  is_capturing = true;

  // Create capture task
  BaseType_t task_ret = xTaskCreate(capture_task, "audio_capture", 4096, NULL,
                                    5, &capture_task_handle);
  if (task_ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create capture task");
    is_capturing = false;
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Audio capture started");
  return ESP_OK;
}

void audio_capture_stop(void) {
  if (!is_capturing) {
    return;
  }

  is_capturing = false;
  capture_callback = NULL;
  wwd_callback = NULL;
  capture_mode = CAPTURE_MODE_IDLE;

  ESP_LOGI(TAG, "Audio capture stopped");
}

esp_err_t audio_capture_stop_wait(uint32_t timeout_ms) {
  if (!is_capturing) {
    return ESP_OK;
  }

  // If called from the capture task context, we can't wait for ourselves.
  if (capture_task_handle != NULL &&
      xTaskGetCurrentTaskHandle() == capture_task_handle) {
    audio_capture_stop();
    return ESP_OK;
  }

  audio_capture_stop();

  if (capture_stopped_sem == NULL) {
    return ESP_OK;
  }

  TickType_t ticks =
      (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
  if (xSemaphoreTake(capture_stopped_sem, ticks) != pdTRUE) {
    ESP_LOGW(TAG, "Timed out waiting for capture task to stop");
    return ESP_ERR_TIMEOUT;
  }

  // Task has exited; clear handle.
  capture_task_handle = NULL;
  return ESP_OK;
}

void audio_capture_deinit(void) {
  audio_capture_stop_wait(1000);

  if (vad_handle != NULL) {
    vad_deinit(vad_handle);
    vad_handle = NULL;
  }

  ESP_LOGI(TAG, "Audio capture deinitialized");
}

esp_err_t audio_capture_enable_vad(const vad_config_t *config,
                                   audio_capture_vad_callback_t callback) {
  if (config == NULL) {
    ESP_LOGE(TAG, "Invalid VAD config");
    return ESP_ERR_INVALID_ARG;
  }

  // Re-init safely if already enabled (avoid leaking previous handle)
  if (vad_handle != NULL) {
    vad_deinit(vad_handle);
    vad_handle = NULL;
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

void audio_capture_disable_vad(void) {
  vad_enabled = false;
  vad_callback = NULL;

  if (vad_handle != NULL) {
    vad_deinit(vad_handle);
    vad_handle = NULL;
  }

  ESP_LOGI(TAG, "VAD disabled");
}

void audio_capture_reset_vad(void) {
  if (vad_handle != NULL) {
    vad_reset(vad_handle);
    ESP_LOGD(TAG, "VAD reset");
  }
}

esp_err_t
audio_capture_start_wake_word_mode(audio_capture_wwd_callback_t wwd_cb) {
  if (is_capturing) {
    ESP_LOGW(TAG, "Already capturing");
    return ESP_ERR_INVALID_STATE;
  }

  if (!wwd_cb) {
    ESP_LOGE(TAG, "WWD callback required");
    return ESP_ERR_INVALID_ARG;
  }

  ESP_LOGI(TAG, "Starting wake word detection mode");

  ensure_capture_stopped_sem();
  if (capture_stopped_sem) {
    // Mark "not stopped" for this run.
    xSemaphoreTake(capture_stopped_sem, 0);
  }

  // Reset codec to microphone configuration (16kHz MONO)
  // This is critical after TTS playback which uses 24kHz STEREO
  // Use set_fs to properly close both playback and record channels
  extern esp_err_t bsp_extra_codec_set_fs(uint32_t rate, uint32_t bits_cfg,
                                          i2s_slot_mode_t ch);
  esp_err_t codec_ret = bsp_extra_codec_set_fs(16000, 16, I2S_SLOT_MODE_MONO);
  if (codec_ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to reset codec for microphone: %s",
             esp_err_to_name(codec_ret));
  } else {
    ESP_LOGI(TAG, "Codec reset to 16kHz MONO for microphone");
  }

  wwd_callback = wwd_cb;
  capture_mode = CAPTURE_MODE_WAKE_WORD;
  is_capturing = true;

  // Create capture task
  BaseType_t ret = xTaskCreate(capture_task, "audio_capture", 4096, NULL, 5,
                               &capture_task_handle);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create capture task");
    is_capturing = false;
    capture_mode = CAPTURE_MODE_IDLE;
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Audio capture started (Wake Word Mode)");
  return ESP_OK;
}

audio_capture_mode_t audio_capture_get_mode(void) { return capture_mode; }

// ═══════════════════════════════════════════════════════════════════════════
// AGC (Automatic Gain Control) Functions
// ═══════════════════════════════════════════════════════════════════════════

esp_err_t audio_capture_enable_agc(uint16_t target_level) {
  if (agc_enabled && agc_handle != NULL) {
    // Already enabled, just update target
    return agc_set_target_level(agc_handle, target_level);
  }

  ESP_LOGI(TAG, "Enabling AGC with target level: %u", target_level);

  agc_config_t config = agc_get_default_config();
  config.target_level = target_level;

  esp_err_t ret = agc_init(&config, &agc_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize AGC: %s", esp_err_to_name(ret));
    return ret;
  }

  agc_enabled = true;
  ESP_LOGI(TAG, "AGC enabled");
  return ESP_OK;
}

void audio_capture_disable_agc(void) {
  if (!agc_enabled) {
    return;
  }

  ESP_LOGI(TAG, "Disabling AGC");

  if (agc_handle != NULL) {
    agc_deinit(agc_handle);
    agc_handle = NULL;
  }

  agc_enabled = false;
}

bool audio_capture_is_agc_enabled(void) { return agc_enabled; }

float audio_capture_get_agc_gain(void) {
  if (!agc_enabled || agc_handle == NULL) {
    return 1.0f;
  }
  return agc_get_current_gain(agc_handle);
}

esp_err_t audio_capture_set_agc_target(uint16_t target_level) {
  if (!agc_enabled || agc_handle == NULL) {
    ESP_LOGW(TAG, "AGC not enabled");
    return ESP_ERR_INVALID_STATE;
  }
  return agc_set_target_level(agc_handle, target_level);
}
