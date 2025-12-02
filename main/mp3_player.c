#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_spiffs.h"
#include "esp_vfs_fat.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"
#include "audio_player.h"

#include "file_iterator.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "wifi_manager.h"
#include "ha_client.h"
#include "tts_player.h"
#include "audio_capture.h"

#define TAG             "mp3_player"
#define MUSIC_DIR       "/sdcard/music"
#define BUTTON_IO_NUM   35
#define BUTTON_ACTIVE_LEVEL   0

file_iterator_instance_t *_file_iterator;
static audio_player_cb_t audio_idle_callback = NULL;
static QueueHandle_t event_queue;
static SemaphoreHandle_t semph_event;
int music_cnt = 0;
int cnt = 0;

static void audio_player_callback(audio_player_cb_ctx_t *ctx)
{
    ESP_LOGI(TAG,"audio_player_callback %d",ctx->audio_event);
    if(ctx->audio_event == AUDIO_PLAYER_CALLBACK_EVENT_SHUTDOWN || ctx->audio_event == AUDIO_PLAYER_CALLBACK_EVENT_IDLE)
        xSemaphoreGive(semph_event);
        // xQueueSend(event_queue, &(ctx->audio_event), 0);
}

static void mp3_player_task(void *arg)
{
    audio_player_callback_event_t event;
    while(true)
    {
        bsp_extra_player_play_index(_file_iterator,cnt);
        cnt++;
        if(cnt > music_cnt)
            cnt = 0;
        xSemaphoreTake(semph_event, portMAX_DELAY);
    }

    bsp_extra_player_del();
    vTaskDelete(NULL);
}

static void conversation_response_handler(const char *response_text, const char *conversation_id)
{
    ESP_LOGI(TAG, "HA Response [%s]: %s",
             conversation_id ? conversation_id : "none",
             response_text);
}

static void tts_audio_handler(const uint8_t *audio_data, size_t length)
{
    ESP_LOGI(TAG, "Received TTS audio: %d bytes", length);

    // Feed audio to TTS player
    esp_err_t ret = tts_player_feed(audio_data, length);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to feed TTS audio: %s", esp_err_to_name(ret));
    }
}

// Audio streaming test variables
static char *pipeline_handler = NULL;
static int audio_chunks_sent = 0;
static bool pipeline_active = false;

static void vad_event_handler(audio_capture_vad_event_t event)
{
    switch (event) {
        case VAD_EVENT_SPEECH_START:
            ESP_LOGI(TAG, "🎤 Speech detected - recording...");
            break;

        case VAD_EVENT_SPEECH_END:
            ESP_LOGI(TAG, "🔇 Silence detected - VAD auto-stop triggered");
            ESP_LOGI(TAG, "Total audio chunks sent: %d", audio_chunks_sent);

            // Mark pipeline as inactive to stop sending audio chunks
            pipeline_active = false;

            // Stop audio capture immediately to free I2S for TTS playback
            audio_capture_stop();
            ESP_LOGI(TAG, "Audio capture stopped - I2S freed for TTS");

            // End audio stream to HA
            ha_client_end_audio_stream();

            // Cleanup pipeline handler
            if (pipeline_handler != NULL) {
                free(pipeline_handler);
                pipeline_handler = NULL;
            }
            audio_chunks_sent = 0;
            break;
    }
}

static void audio_capture_handler(const uint8_t *audio_data, size_t length)
{
    // Check if pipeline is still active
    if (!pipeline_active || pipeline_handler == NULL) {
        return;  // Pipeline closed or not started
    }

    // Validate audio data
    if (audio_data == NULL || length == 0) {
        return;
    }

    // Stream audio to HA
    esp_err_t ret = ha_client_stream_audio(audio_data, length, pipeline_handler);
    if (ret == ESP_OK) {
        audio_chunks_sent++;
    } else {
        ESP_LOGW(TAG, "Failed to stream audio chunk - pipeline may be closed");
    }
}

static void test_audio_streaming(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Starting Audio Streaming Test with VAD");
    ESP_LOGI(TAG, "========================================");

    // Configure VAD with very sensitive settings for testing
    vad_config_t vad_config = {
        .sample_rate = 16000,
        .speech_threshold = 100,         // Very sensitive (was 300)
        .silence_duration_ms = 2000,     // 2s silence to end
        .min_speech_duration_ms = 100,   // 100ms minimum
        .max_recording_ms = 8000         // 8s max (will auto-stop)
    };

    ESP_LOGI(TAG, "📊 VAD Config: threshold=%lu, silence=%lums, max=%lums",
             vad_config.speech_threshold,
             vad_config.silence_duration_ms,
             vad_config.max_recording_ms);

    // Enable VAD with event callback
    esp_err_t ret = audio_capture_enable_vad(&vad_config, vad_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable VAD");
        return;
    }

    // Start Assist Pipeline
    pipeline_handler = ha_client_start_conversation();
    if (pipeline_handler == NULL) {
        ESP_LOGE(TAG, "Failed to start pipeline");
        return;
    }

    ESP_LOGI(TAG, "Pipeline started: %s", pipeline_handler);
    ESP_LOGI(TAG, "🎙️  Start speaking now! (VAD will auto-stop after silence)");

    // Reset counters and mark pipeline as active
    audio_chunks_sent = 0;
    pipeline_active = true;

    // Reset VAD state
    audio_capture_reset_vad();

    // Start capturing and streaming audio
    ret = audio_capture_start(audio_capture_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start audio capture");
        free(pipeline_handler);
        pipeline_handler = NULL;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "===== ESP32-P4 Voice Assistant Starting =====");

    // Initialize NVS (required for WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    // Initialize audio codec
    ESP_LOGI(TAG, "Initializing ES8311 audio codec...");
    ESP_ERROR_CHECK(bsp_extra_codec_init());
    bsp_extra_codec_volume_set(40,NULL);
    bsp_extra_player_init();
    ESP_LOGI(TAG, "ES8311 codec initialized successfully");

    // Initialize TTS player
    ESP_LOGI(TAG, "Initializing TTS player...");
    ret = tts_player_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TTS player initialization failed");
    } else {
        ESP_LOGI(TAG, "TTS player initialized successfully");
    }

    // Initialize audio capture
    ESP_LOGI(TAG, "Initializing audio capture...");
    ret = audio_capture_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Audio capture initialization failed");
    } else {
        ESP_LOGI(TAG, "Audio capture initialized successfully");
    }

    // Initialize WiFi via ESP32-C6 (SDIO)
    ESP_LOGI(TAG, "Initializing WiFi (ESP32-C6 via SDIO)...");
    ret = wifi_init_sta();
    if(ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi connected successfully!");

        // Initialize Home Assistant client
        ESP_LOGI(TAG, "Connecting to Home Assistant...");
        ret = ha_client_init();
        if(ret == ESP_OK) {
            ESP_LOGI(TAG, "Home Assistant connected successfully!");

            // Register callbacks
            ha_client_register_conversation_callback(conversation_response_handler);
            ha_client_register_tts_audio_callback(tts_audio_handler);

            // Wait a bit, then start audio streaming test
            ESP_LOGI(TAG, "Will start audio streaming test in 5 seconds...");
            ESP_LOGI(TAG, "Please speak into the microphone!");
            vTaskDelay(pdMS_TO_TICKS(5000));
            test_audio_streaming();
        } else {
            ESP_LOGW(TAG, "Home Assistant connection failed");
        }
    } else {
        ESP_LOGW(TAG, "WiFi connection failed, continuing without network");
    }

    // SD card MP3 playback disabled - focusing on Voice Assistant functionality
    // TODO: Fix file_iterator crash issue before re-enabling
    ESP_LOGI(TAG, "MP3 playback disabled (Voice Assistant mode)");
    ESP_LOGI(TAG, "Audio codec is ready for Voice Assistant development");
    ESP_LOGI(TAG, "System idle - ready to process voice commands...");

    // Keep the system running for HA communication
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

}
