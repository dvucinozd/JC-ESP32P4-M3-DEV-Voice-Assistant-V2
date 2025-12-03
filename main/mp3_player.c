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
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"
#include "audio_player.h"

#include "file_iterator.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "wifi_manager.h"
#include "network_manager.h"
#include "bsp/esp32_p4_function_ev_board.h"  // For bsp_sdcard_mount/unmount
#include "ha_client.h"
#include "tts_player.h"
#include "audio_capture.h"
#include "wwd.h"
#include "mqtt_ha.h"
#include "config.h"

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

static void network_event_callback(network_type_t type, bool connected)
{
    if (connected) {
        char ip_str[16];
        network_manager_get_ip(ip_str);

        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "Network Connected!");
        ESP_LOGI(TAG, "Type: %s", network_manager_type_to_string(type));
        ESP_LOGI(TAG, "IP Address: %s", ip_str);
        ESP_LOGI(TAG, "========================================");

        // Update MQTT sensor if MQTT is active
        if (mqtt_ha_is_connected()) {
            mqtt_ha_update_sensor("network_type", network_manager_type_to_string(type));
            mqtt_ha_update_sensor("ip_address", ip_str);
        }

        // Mount/unmount SD card based on network type
        if (type == NETWORK_TYPE_ETHERNET) {
            ESP_LOGI(TAG, "📀 Ethernet active - mounting SD card for local music...");
            esp_err_t sd_ret = bsp_sdcard_mount();
            if (sd_ret == ESP_OK) {
                ESP_LOGI(TAG, "✅ SD card mounted successfully - local music playback enabled");
                // Update MQTT sensor for SD card status
                if (mqtt_ha_is_connected()) {
                    mqtt_ha_update_sensor("sd_card_status", "mounted");
                }
            } else {
                ESP_LOGW(TAG, "⚠️  SD card mount failed - local music unavailable");
                if (mqtt_ha_is_connected()) {
                    mqtt_ha_update_sensor("sd_card_status", "failed");
                }
            }
        } else if (type == NETWORK_TYPE_WIFI) {
            ESP_LOGI(TAG, "📶 WiFi fallback active - SD card disabled");
            // Unmount SD card if it was mounted
            if (bsp_sdcard != NULL) {
                ESP_LOGI(TAG, "Unmounting SD card (WiFi fallback mode)...");
                bsp_sdcard_unmount();
                if (mqtt_ha_is_connected()) {
                    mqtt_ha_update_sensor("sd_card_status", "unmounted");
                }
            }
        }
    } else {
        ESP_LOGW(TAG, "Network disconnected: %s", network_manager_type_to_string(type));

        // Unmount SD card on network disconnect
        if (bsp_sdcard != NULL) {
            ESP_LOGI(TAG, "Unmounting SD card (network disconnected)...");
            bsp_sdcard_unmount();
            if (mqtt_ha_is_connected()) {
                mqtt_ha_update_sensor("sd_card_status", "disconnected");
            }
        }
    }
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
static int warmup_chunks_skip = 0;  // Skip first N chunks after wake word

// Dynamic configuration variables (adjustable via MQTT)
static uint32_t vad_threshold = 180;           // Speech energy threshold (50-300)
static uint32_t vad_silence_duration = 1800;   // Silence duration in ms (1000-3000)
static uint32_t vad_min_speech = 200;          // Min speech duration in ms (100-500)
static uint32_t vad_max_recording = 7000;      // Max recording duration in ms (5000-10000)
static float wwd_threshold = 0.5f;             // Wake word detection threshold (0.3-0.9)

// Forward declarations
static void test_audio_streaming(void);
static void wwd_audio_feed_wrapper(const int16_t *audio_data, size_t samples);
static void on_wake_word_detected(wwd_event_t event, void *user_data);

// MQTT entity callback handlers
static void mqtt_wwd_switch_callback(const char *entity_id, const char *payload)
{
    ESP_LOGI(TAG, "MQTT: WWD switch = %s", payload);

    if (strcmp(payload, "ON") == 0) {
        wwd_start();
        audio_capture_start_wake_word_mode(wwd_audio_feed_wrapper);
        mqtt_ha_update_switch("wwd_enabled", true);
        ESP_LOGI(TAG, "Wake Word Detection enabled via MQTT");
    } else {
        wwd_stop();
        audio_capture_stop();
        mqtt_ha_update_switch("wwd_enabled", false);
        ESP_LOGI(TAG, "Wake Word Detection disabled via MQTT");
    }
}

static void mqtt_restart_callback(const char *entity_id, const char *payload)
{
    ESP_LOGI(TAG, "MQTT: Restart button pressed!");
    ESP_LOGI(TAG, "Restarting in 2 seconds...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

static void mqtt_test_tts_callback(const char *entity_id, const char *payload)
{
    ESP_LOGI(TAG, "MQTT: Test TTS button pressed!");

    if (ha_client_is_connected()) {
        ESP_LOGI(TAG, "Requesting test TTS audio...");
        // Use a valid intent/question that HA will understand
        esp_err_t ret = ha_client_request_tts("Koliko je sati?");
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Test TTS request sent successfully");
        } else {
            ESP_LOGE(TAG, "Failed to send test TTS request");
        }
    } else {
        ESP_LOGW(TAG, "Cannot test TTS - not connected to Home Assistant");
    }
}

static void mqtt_vad_threshold_callback(const char *entity_id, const char *payload)
{
    float value = atof(payload);
    vad_threshold = (uint32_t)value;
    ESP_LOGI(TAG, "MQTT: VAD threshold updated to %lu", vad_threshold);
    mqtt_ha_update_number("vad_threshold", (float)vad_threshold);
}

static void mqtt_vad_silence_callback(const char *entity_id, const char *payload)
{
    float value = atof(payload);
    vad_silence_duration = (uint32_t)value;
    ESP_LOGI(TAG, "MQTT: VAD silence duration updated to %lums", vad_silence_duration);
    mqtt_ha_update_number("vad_silence_duration", (float)vad_silence_duration);
}

static void mqtt_vad_min_speech_callback(const char *entity_id, const char *payload)
{
    float value = atof(payload);
    vad_min_speech = (uint32_t)value;
    ESP_LOGI(TAG, "MQTT: VAD min speech updated to %lums", vad_min_speech);
    mqtt_ha_update_number("vad_min_speech", (float)vad_min_speech);
}

static void mqtt_vad_max_recording_callback(const char *entity_id, const char *payload)
{
    float value = atof(payload);
    vad_max_recording = (uint32_t)value;
    ESP_LOGI(TAG, "MQTT: VAD max recording updated to %lums", vad_max_recording);
    mqtt_ha_update_number("vad_max_recording", (float)vad_max_recording);
}

static void mqtt_wwd_threshold_callback(const char *entity_id, const char *payload)
{
    wwd_threshold = atof(payload);
    ESP_LOGI(TAG, "MQTT: WWD threshold updated to %.2f", wwd_threshold);

    // Apply to WakeNet immediately if running
    if (wwd_is_running()) {
        wwd_stop();
        audio_capture_stop();
        vTaskDelay(pdMS_TO_TICKS(100));

        wwd_config_t wwd_config = wwd_get_default_config();
        wwd_config.callback = on_wake_word_detected;
        wwd_config.user_data = NULL;
        wwd_config.detection_threshold = wwd_threshold;

        esp_err_t ret = wwd_init(&wwd_config);
        if (ret == ESP_OK) {
            wwd_start();
            audio_capture_start_wake_word_mode(wwd_audio_feed_wrapper);
            ESP_LOGI(TAG, "WWD restarted with new threshold");
        } else {
            ESP_LOGE(TAG, "Failed to restart WWD with new threshold");
        }
    }

    mqtt_ha_update_number("wwd_threshold", wwd_threshold);
}

// MQTT status update task
static void mqtt_status_update_task(void *arg)
{
    while (1) {
        if (mqtt_ha_is_connected()) {
            // Update WiFi RSSI
            // Note: Would need wifi_manager API to get RSSI
            mqtt_ha_update_sensor("wifi_rssi", "-45");

            // Update free memory
            char mem_str[32];
            uint32_t free_mem = esp_get_free_heap_size() / 1024; // KB
            snprintf(mem_str, sizeof(mem_str), "%lu", (unsigned long)free_mem);
            mqtt_ha_update_sensor("free_memory", mem_str);

            // Update uptime
            char uptime_str[32];
            uint32_t uptime_sec = esp_log_timestamp() / 1000;
            snprintf(uptime_str, sizeof(uptime_str), "%lu", (unsigned long)uptime_sec);
            mqtt_ha_update_sensor("uptime", uptime_str);

            // Update WWD state
            mqtt_ha_update_switch("wwd_enabled", wwd_is_running());

            // Update network status
            char ip_str[16];
            if (network_manager_get_ip(ip_str) == ESP_OK) {
                mqtt_ha_update_sensor("ip_address", ip_str);
            }
            mqtt_ha_update_sensor("network_type", network_manager_type_to_string(network_manager_get_active_type()));

            // Update SD card status
            if (bsp_sdcard != NULL) {
                mqtt_ha_update_sensor("sd_card_status", "mounted");
            } else {
                mqtt_ha_update_sensor("sd_card_status", "not_mounted");
            }

            // Update configuration numbers (only on first iteration or when changed)
            static bool config_published = false;
            if (!config_published) {
                mqtt_ha_update_number("vad_threshold", (float)vad_threshold);
                mqtt_ha_update_number("vad_silence_duration", (float)vad_silence_duration);
                mqtt_ha_update_number("vad_min_speech", (float)vad_min_speech);
                mqtt_ha_update_number("vad_max_recording", (float)vad_max_recording);
                mqtt_ha_update_number("wwd_threshold", wwd_threshold);
                config_published = true;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10000)); // Update every 10 seconds
    }
}

// Wake word detection callback
static void on_wake_word_detected(wwd_event_t event, void *user_data)
{
    if (event == WWD_EVENT_DETECTED) {
        ESP_LOGI(TAG, "🎤 Wake word detected!");

        // Stop wake word detection
        wwd_stop();

        // Stop audio capture (from wake word mode)
        audio_capture_stop();
        ESP_LOGI(TAG, "Wake word mode stopped");

        // Small delay to ensure capture task fully exits
        vTaskDelay(pdMS_TO_TICKS(100));

        // Play beep tone as audio feedback (800Hz, 120ms, 40% volume)
        ESP_LOGI(TAG, "🔊 Playing wake word confirmation beep...");
        extern esp_err_t beep_tone_play(uint16_t frequency, uint16_t duration, uint8_t volume);
        esp_err_t beep_ret = beep_tone_play(800, 120, 40);
        if (beep_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to play beep tone: %s", esp_err_to_name(beep_ret));
        }

        // Small delay after beep before starting recording
        vTaskDelay(pdMS_TO_TICKS(50));

        ESP_LOGI(TAG, "Starting voice pipeline...");

        // Start voice assistant pipeline
        test_audio_streaming();
    }
}

// Wrapper function to feed audio to wake word detector
static void wwd_audio_feed_wrapper(const int16_t *audio_data, size_t samples)
{
    wwd_feed_audio(audio_data, samples);
}

static void tts_playback_complete_handler(void)
{
    ESP_LOGI(TAG, "🔄 TTS playback complete - resuming wake word detection...");

    // Resume wake word detection
    wwd_start();
    audio_capture_start_wake_word_mode(wwd_audio_feed_wrapper);

    ESP_LOGI(TAG, "✅ Wake word detection resumed - ready for next command");
}

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

            // WWD will be resumed AFTER TTS playback completes (via callback)
            ESP_LOGI(TAG, "Waiting for TTS playback to complete before resuming WWD...");
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

    // Skip warmup chunks to avoid wake word remnants
    if (warmup_chunks_skip > 0) {
        warmup_chunks_skip--;
        ESP_LOGD(TAG, "Skipping warmup chunk (%d remaining)", warmup_chunks_skip);
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

    // Configure VAD with dynamic settings (adjustable via MQTT)
    vad_config_t vad_config = {
        .sample_rate = 16000,
        .speech_threshold = vad_threshold,
        .silence_duration_ms = vad_silence_duration,
        .min_speech_duration_ms = vad_min_speech,
        .max_recording_ms = vad_max_recording
    };

    ESP_LOGI(TAG, "📊 VAD Config: threshold=%lu, silence=%lums, min_speech=%lums, max=%lums",
             vad_config.speech_threshold,
             vad_config.silence_duration_ms,
             vad_config.min_speech_duration_ms,
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
    warmup_chunks_skip = 10;  // Skip first 10 chunks (~640ms) to clear wake word remnants

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

    // Note: SD card not mounted - using WakeNet models from flash instead
    // The ESP32-P4 Function EV Board's SD card slot uses SDIO pins that are
    // already occupied by the ESP32-C6 WiFi module. WakeNet9 models are loaded
    // directly from the esp-sr managed component directory in flash memory.
    ESP_LOGI(TAG, "WakeNet models will be loaded from flash (managed_components)");

    // Initialize Wake Word Detection
    ESP_LOGI(TAG, "Initializing Wake Word Detection...");
    wwd_config_t wwd_config = wwd_get_default_config();
    wwd_config.callback = on_wake_word_detected;
    wwd_config.user_data = NULL;
    wwd_config.detection_threshold = wwd_threshold;  // Dynamic threshold (adjustable via MQTT)
    ESP_LOGI(TAG, "WWD threshold: %.2f, VAD threshold: %lu", wwd_threshold, vad_threshold);
    esp_err_t wwd_ret = wwd_init(&wwd_config);

    if (wwd_ret != ESP_OK) {
        ESP_LOGE(TAG, "Wake Word Detection initialization failed!");
        ESP_LOGW(TAG, "Falling back to VAD-based activation");
    } else {
        ESP_LOGI(TAG, "Wake Word Detection initialized successfully!");
    }

    // Initialize Network Manager (Ethernet priority + WiFi fallback)
    ESP_LOGI(TAG, "Initializing Network Manager...");
    network_manager_register_callback(network_event_callback);
    ret = network_manager_init();
    if(ret == ESP_OK && network_manager_is_connected()) {
        ESP_LOGI(TAG, "Network connected successfully!");
        ESP_LOGI(TAG, "Active network: %s", network_manager_type_to_string(network_manager_get_active_type()));

        // Initialize Home Assistant client
        ESP_LOGI(TAG, "Connecting to Home Assistant...");
        ret = ha_client_init();
        if(ret == ESP_OK) {
            ESP_LOGI(TAG, "Home Assistant connected successfully!");

            // Register callbacks
            ha_client_register_conversation_callback(conversation_response_handler);
            ha_client_register_tts_audio_callback(tts_audio_handler);
            tts_player_register_complete_callback(tts_playback_complete_handler);

            // Initialize MQTT Home Assistant Discovery
            ESP_LOGI(TAG, "Initializing MQTT Home Assistant Discovery...");
            mqtt_ha_config_t mqtt_config = {
                .broker_uri = MQTT_BROKER_URI,
                .username = MQTT_USERNAME,
                .password = MQTT_PASSWORD,
                .client_id = MQTT_CLIENT_ID
            };

            ret = mqtt_ha_init(&mqtt_config);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "MQTT client initialized");

                // Start MQTT client
                ret = mqtt_ha_start();
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "MQTT client started");

                    // Wait for MQTT connection
                    vTaskDelay(pdMS_TO_TICKS(2000));

                    // Register entities with Home Assistant
                    ESP_LOGI(TAG, "Registering Home Assistant entities...");

                    // Sensors
                    mqtt_ha_register_sensor("network_type", "Network Type", NULL, NULL);
                    mqtt_ha_register_sensor("ip_address", "IP Address", NULL, NULL);
                    mqtt_ha_register_sensor("sd_card_status", "SD Card Status", NULL, NULL);
                    mqtt_ha_register_sensor("wifi_rssi", "WiFi Signal", "dBm", "signal_strength");
                    mqtt_ha_register_sensor("free_memory", "Free Memory", "KB", NULL);
                    mqtt_ha_register_sensor("uptime", "Uptime", "s", "duration");

                    // Switches
                    mqtt_ha_register_switch("wwd_enabled", "Wake Word Detection", mqtt_wwd_switch_callback);

                    // Buttons
                    mqtt_ha_register_button("restart", "Restart Device", mqtt_restart_callback);
                    mqtt_ha_register_button("test_tts", "Test TTS", mqtt_test_tts_callback);

                    // Number controls for VAD tuning
                    mqtt_ha_register_number("vad_threshold", "VAD Speech Threshold",
                                           50, 300, 10, NULL, mqtt_vad_threshold_callback);
                    mqtt_ha_register_number("vad_silence_duration", "VAD Silence Duration",
                                           1000, 3000, 100, "ms", mqtt_vad_silence_callback);
                    mqtt_ha_register_number("vad_min_speech", "VAD Min Speech Duration",
                                           100, 500, 50, "ms", mqtt_vad_min_speech_callback);
                    mqtt_ha_register_number("vad_max_recording", "VAD Max Recording Duration",
                                           5000, 10000, 500, "ms", mqtt_vad_max_recording_callback);

                    // Number control for WWD tuning
                    mqtt_ha_register_number("wwd_threshold", "WWD Detection Threshold",
                                           0.3, 0.9, 0.05, NULL, mqtt_wwd_threshold_callback);

                    ESP_LOGI(TAG, "Home Assistant entities registered (3 sensors, 1 switch, 2 buttons, 5 numbers)");

                    // Start MQTT status update task
                    xTaskCreate(mqtt_status_update_task, "mqtt_status", 4096, NULL, 3, NULL);
                    ESP_LOGI(TAG, "MQTT status update task started");
                } else {
                    ESP_LOGW(TAG, "Failed to start MQTT client");
                }
            } else {
                ESP_LOGW(TAG, "Failed to initialize MQTT client");
            }

            // Start wake word detection if initialized successfully
            if (wwd_ret == ESP_OK && wwd_is_running() == false) {
                ESP_LOGI(TAG, "========================================");
                ESP_LOGI(TAG, "🎙️  Voice Assistant Ready!");
                ESP_LOGI(TAG, "Wake Word Detection enabled");
                ESP_LOGI(TAG, "Say the wake word to activate!");
                ESP_LOGI(TAG, "Wake word: 'Hi ESP' (or your chosen model)");
                ESP_LOGI(TAG, "========================================");

                // Start wake word detection
                wwd_start();
                audio_capture_start_wake_word_mode(wwd_audio_feed_wrapper);
            } else {
                // Fallback to VAD-based activation
                ESP_LOGI(TAG, "========================================");
                ESP_LOGI(TAG, "🎙️  Voice Assistant Ready!");
                ESP_LOGI(TAG, "Using VAD-based activation");
                ESP_LOGI(TAG, "System will start recording in 5 seconds...");
                ESP_LOGI(TAG, "Just start speaking - VAD will detect automatically!");
                ESP_LOGI(TAG, "========================================");
                vTaskDelay(pdMS_TO_TICKS(5000));
                test_audio_streaming();
            }
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
