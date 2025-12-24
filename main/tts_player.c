/**
 * TTS Audio Player Implementation
 */

#include "tts_player.h"
#include "audio_capture.h"
#include "audio_player.h"
#include "bsp_board_extra.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mp3dec.h"
#include <string.h>

static const char *TAG = "tts_player";

#define TTS_BUFFER_SIZE (128 * 1024) // 128KB buffer for audio chunks
#define TTS_QUEUE_SIZE 10
#define PCM_BUFFER_SIZE (MAX_NCHAN * MAX_NSAMP * 2) // Max PCM output per frame

typedef struct {
  uint8_t *data;
  size_t length;
} audio_chunk_t;

static QueueHandle_t audio_queue = NULL;
static TaskHandle_t playback_task_handle = NULL;
static bool is_playing = false;

// MP3 decoder instance
static HMP3Decoder mp3_decoder = NULL;

// Simple audio buffer for accumulating chunks
static uint8_t *tts_buffer = NULL;
static size_t tts_buffer_pos = 0;

// Playback completion callback
static tts_playback_complete_callback_t playback_complete_callback = NULL;

static void tts_queue_stop_signal(void) {
  if (audio_queue == NULL) {
    return;
  }

  audio_chunk_t stop_chunk = {.data = NULL, .length = 0};
  if (xQueueSend(audio_queue, &stop_chunk, pdMS_TO_TICKS(500)) != pdTRUE) {
    ESP_LOGE(TAG, "Failed to queue stop signal (queue full)");
    // Ensure assistant doesn't get stuck waiting for completion.
    if (playback_complete_callback) {
      playback_complete_callback();
    }
  }
}

/**
 * Decode and play MP3 audio buffer
 */
static esp_err_t play_mp3_buffer(uint8_t *mp3_data, size_t mp3_size) {
  esp_err_t overall_ret = ESP_OK;
  int16_t *pcm_buffer = NULL;

  if (mp3_decoder == NULL) {
    ESP_LOGE(TAG, "MP3 decoder not initialized");
    overall_ret = ESP_ERR_INVALID_STATE;
    goto out;
  }

  ESP_LOGI(TAG, "Decoding MP3: %d bytes", mp3_size);

  // Ensure codec is unmuted for playback
  bsp_extra_codec_mute_set(false);
  ESP_LOGI(TAG, "Codec unmuted for TTS playback");

  // Reset codec configuration flag for this playback session
  // This ensures codec is reconfigured on first MP3 frame
  static bool codec_configured_flag = false;
  codec_configured_flag = false;

  // PCM output buffer
  pcm_buffer = (int16_t *)malloc(PCM_BUFFER_SIZE);
  if (pcm_buffer == NULL) {
    ESP_LOGE(TAG, "Failed to allocate PCM buffer");
    overall_ret = ESP_ERR_NO_MEM;
    goto out;
  }

  uint8_t *read_ptr = mp3_data;
  int bytes_left = mp3_size;
  int total_samples = 0;

  // Decode MP3 frames
  while (bytes_left > 0) {
    // Find sync word
    int offset = MP3FindSyncWord(read_ptr, bytes_left);
    if (offset < 0) {
      ESP_LOGD(TAG, "No more MP3 frames found");
      break;
    }

    read_ptr += offset;
    bytes_left -= offset;

    // Decode one MP3 frame
    int err = MP3Decode(mp3_decoder, &read_ptr, &bytes_left, pcm_buffer, 0);

    if (err == ERR_MP3_NONE) {
      // Get frame info
      MP3FrameInfo frame_info;
      MP3GetLastFrameInfo(mp3_decoder, &frame_info);

      ESP_LOGD(TAG, "Decoded frame: %d Hz, %d ch, %d samples",
               frame_info.samprate, frame_info.nChans, frame_info.outputSamps);

      // Configure I2S for this sample rate on first frame
      // Always reconfigure to handle cases where beep tone changed codec
      // settings
      if (!codec_configured_flag) {
        ESP_LOGI(TAG, "Configuring codec: %d Hz, %d channels",
                 frame_info.samprate, frame_info.nChans);
        // Use set_fs to properly reconfigure both playback and record devices
        // This is critical after beep tone which uses 16kHz MONO
        bsp_extra_codec_set_fs(frame_info.samprate, 16,
                               (i2s_slot_mode_t)frame_info.nChans);
        codec_configured_flag = true;
      }

      // Write PCM data to I2S
      size_t pcm_bytes = frame_info.outputSamps * sizeof(int16_t);
      size_t bytes_written = 0;
      // timeout_ms: 0 means block indefinitely
      esp_err_t ret =
          bsp_extra_i2s_write(pcm_buffer, pcm_bytes, &bytes_written, 0);

      if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
        overall_ret = ret;
        goto out;
      }

      total_samples += frame_info.outputSamps;

    } else if (err == ERR_MP3_INDATA_UNDERFLOW) {
      ESP_LOGD(TAG, "MP3 data underflow, need more data");
      break;
    } else {
      ESP_LOGW(TAG, "MP3 decode error: %d", err);
      // Skip some bytes and try again
      if (bytes_left > 0) {
        read_ptr++;
        bytes_left--;
      }
    }
  }

  ESP_LOGI(TAG, "Playback complete: %d samples", total_samples);

out:
  if (pcm_buffer) {
    free(pcm_buffer);
  }

  // Always signal completion so the assistant can resume listening even on
  // errors
  if (playback_complete_callback) {
    playback_complete_callback();
  }

  return overall_ret;
}

/**
 * Playback task - processes audio chunks from queue
 */
static void playback_task(void *arg) {
  audio_chunk_t chunk;

  while (1) {
    // Wait for audio chunk
    if (xQueueReceive(audio_queue, &chunk, portMAX_DELAY) == pdTRUE) {
      if (chunk.data == NULL || chunk.length == 0) {
        // Stop signal
        ESP_LOGI(TAG, "Stop signal received");
        is_playing = false;

        // Play accumulated buffer if we have data
        if (tts_buffer_pos > 0) {
          ESP_LOGI(TAG, "Playing TTS audio: %d bytes MP3", tts_buffer_pos);

          // Stop audio capture to free I2S channel for playback
          (void)audio_capture_stop_wait(1000);
          ESP_LOGI(TAG, "Audio capture stopped - I2S freed for TTS playback");

          // Small delay to ensure I2S is fully released
          vTaskDelay(pdMS_TO_TICKS(50));

          // Decode and play MP3 buffer
          esp_err_t ret = play_mp3_buffer(tts_buffer, tts_buffer_pos);
          if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to play MP3: %s", esp_err_to_name(ret));
          }

          tts_buffer_pos = 0;
        } else {
          // No audio received (download failed / empty stream) - still unblock
          // resume path
          if (playback_complete_callback) {
            playback_complete_callback();
          }
        }
        continue;
      }

      // Accumulate audio data
      if (tts_buffer_pos + chunk.length < TTS_BUFFER_SIZE) {
        memcpy(tts_buffer + tts_buffer_pos, chunk.data, chunk.length);
        tts_buffer_pos += chunk.length;
        ESP_LOGD(TAG, "Buffered audio chunk: %d bytes (total: %d)",
                 chunk.length, tts_buffer_pos);
      } else {
        ESP_LOGW(TAG, "TTS buffer full (%d/%d), dropping %d bytes",
                 tts_buffer_pos, TTS_BUFFER_SIZE, chunk.length);
      }

      // Free chunk data
      free(chunk.data);
    }
  }
}

esp_err_t tts_player_init(void) {
  ESP_LOGI(TAG, "Initializing TTS player...");

  // Initialize MP3 decoder
  mp3_decoder = MP3InitDecoder();
  if (mp3_decoder == NULL) {
    ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
    return ESP_ERR_NO_MEM;
  }

  // Allocate audio buffer
  tts_buffer = (uint8_t *)malloc(TTS_BUFFER_SIZE);
  if (tts_buffer == NULL) {
    ESP_LOGE(TAG, "Failed to allocate TTS buffer");
    MP3FreeDecoder(mp3_decoder);
    return ESP_ERR_NO_MEM;
  }
  tts_buffer_pos = 0;

  // Create audio queue
  audio_queue = xQueueCreate(TTS_QUEUE_SIZE, sizeof(audio_chunk_t));
  if (audio_queue == NULL) {
    ESP_LOGE(TAG, "Failed to create audio queue");
    free(tts_buffer);
    MP3FreeDecoder(mp3_decoder);
    return ESP_ERR_NO_MEM;
  }

  // Create playback task
  BaseType_t ret = xTaskCreate(playback_task, "tts_playback", 8192, NULL, 5,
                               &playback_task_handle);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create playback task");
    vQueueDelete(audio_queue);
    free(tts_buffer);
    MP3FreeDecoder(mp3_decoder);
    return ESP_FAIL;
  }

  is_playing = false;
  ESP_LOGI(TAG, "TTS player initialized");
  return ESP_OK;
}

esp_err_t tts_player_feed(const uint8_t *audio_data, size_t length) {
  if (audio_queue == NULL) {
    ESP_LOGW(TAG, "TTS player not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  if (audio_data == NULL || length == 0) {
    // Empty chunk = end of audio stream
    tts_queue_stop_signal();
    return ESP_OK;
  }

  // Allocate chunk and copy data
  uint8_t *chunk_data = (uint8_t *)malloc(length);
  if (chunk_data == NULL) {
    ESP_LOGE(TAG, "Failed to allocate chunk memory");
    return ESP_ERR_NO_MEM;
  }

  memcpy(chunk_data, audio_data, length);

  audio_chunk_t chunk = {.data = chunk_data, .length = length};

  if (xQueueSend(audio_queue, &chunk, pdMS_TO_TICKS(100)) != pdTRUE) {
    ESP_LOGW(TAG, "Audio queue full, dropping chunk");
    free(chunk_data);
    return ESP_FAIL;
  }

  if (!is_playing) {
    is_playing = true;
    ESP_LOGI(TAG, "Started TTS playback");
  }

  return ESP_OK;
}

void tts_player_stop(void) {
  if (audio_queue != NULL) {
    // Send stop signal
    tts_queue_stop_signal();
  }

  tts_buffer_pos = 0;
  is_playing = false;
  ESP_LOGI(TAG, "TTS playback stopped");
}

void tts_player_deinit(void) {
  tts_player_stop();

  if (playback_task_handle != NULL) {
    vTaskDelete(playback_task_handle);
    playback_task_handle = NULL;
  }

  if (audio_queue != NULL) {
    vQueueDelete(audio_queue);
    audio_queue = NULL;
  }

  if (tts_buffer != NULL) {
    free(tts_buffer);
    tts_buffer = NULL;
  }

  if (mp3_decoder != NULL) {
    MP3FreeDecoder(mp3_decoder);
    mp3_decoder = NULL;
  }

  playback_complete_callback = NULL;

  ESP_LOGI(TAG, "TTS player deinitialized");
}

void tts_player_register_complete_callback(
    tts_playback_complete_callback_t callback) {
  playback_complete_callback = callback;
  ESP_LOGI(TAG, "TTS playback completion callback registered");
}
