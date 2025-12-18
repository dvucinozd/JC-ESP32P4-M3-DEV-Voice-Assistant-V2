#include "voice_pipeline.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_check.h"
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "audio_capture.h"
#include "ha_client.h"
#include "tts_player.h"
#include "led_status.h"
#include "mqtt_ha.h"
#include "local_music_player.h"
#include "beep_tone.h"
#include "cJSON.h"
#include "sys_diag.h"

#define TAG "voice_pipeline"
#define FOLLOWUP_RECORDING_MS 7000

// Internal Command Queue
typedef enum {
    PIPELINE_CMD_WAKE_DETECTED,
    PIPELINE_CMD_OFFLINE_CMD, 
    PIPELINE_CMD_RESUME_WWD,
    PIPELINE_CMD_STOP_WWD,
    PIPELINE_CMD_RESTART_WWD,
    PIPELINE_CMD_START_FOLLOWUP_VAD,
    PIPELINE_CMD_ERROR_RESUME,
    PIPELINE_CMD_TIMER_BEEP,
    PIPELINE_CMD_ALARM_BEEP,
    PIPELINE_CMD_CONFIRM_BEEP,
    PIPELINE_CMD_ERROR_BEEP,
    PIPELINE_CMD_MUSIC_CONTROL 
} pipeline_cmd_type_t;

typedef struct {
    pipeline_cmd_type_t type;
    int data; 
} pipeline_cmd_t;

static QueueHandle_t pipeline_cmd_queue = NULL;
static TaskHandle_t pipeline_task_handle = NULL;

// State variables
static bool is_wwd_running = false;
static bool is_pipeline_active = false;
static bool wake_detect_pending = false;
static bool followup_vad_pending = false;
static bool music_paused_for_tts = false;

static char *current_pipeline_handler = NULL;
static int warmup_chunks_skip = 0;

// Config
static voice_pipeline_config_t current_config = {
    .wwd_threshold = 0.5f,
    .vad_speech_threshold = 180,
    .vad_silence_ms = 1800,
    .vad_min_speech_ms = 200,
    .vad_max_recording_ms = 7000,
    .agc_enabled = true,
    .agc_target_level = 4000
};

// Forward decls
static void pipeline_task(void *arg);
static void on_wake_word_detected(const int16_t *audio_data, size_t samples);
static void on_offline_cmd_detected(int id, int index);
static void vad_event_handler(audio_capture_vad_event_t event);
static void audio_capture_handler(const uint8_t *audio_data, size_t length);
static void intent_handler(const char *intent_name, const char *intent_data, const char *conversation_id);
static void conversation_response_handler(const char *response_text, const char *conversation_id);
static esp_err_t start_audio_streaming(uint32_t max_recording_ms, const char *context_tag);
static void tts_audio_handler(const uint8_t *audio_data, size_t length);
static void on_tts_complete(void);
static void restart_task(void *arg);

// Helper to post commands
static void pipeline_post_cmd(pipeline_cmd_type_t type, int data) {
    if (pipeline_cmd_queue) {
        pipeline_cmd_t cmd = {.type = type, .data = data};
        xQueueSend(pipeline_cmd_queue, &cmd, 0);
    }
}

// =============================================================================
// PUBLIC API
// =============================================================================

esp_err_t voice_pipeline_init(void) {
    ESP_LOGI(TAG, "Initializing Voice Pipeline...");

    pipeline_cmd_queue = xQueueCreate(10, sizeof(pipeline_cmd_t));
    if (!pipeline_cmd_queue) return ESP_ERR_NO_MEM;

    // Initialize Audio Capture (includes AFE/WWD/MultiNet)
    audio_capture_init();
    
    // Register callbacks
    audio_capture_register_cmd_callback(on_offline_cmd_detected);

    // Register HA callbacks
    ha_client_register_intent_callback(intent_handler);
    ha_client_register_conversation_callback(conversation_response_handler);

    // Register TTS callback
    tts_player_init();
    ha_client_register_tts_audio_callback(tts_audio_handler);
    tts_player_register_complete_callback(on_tts_complete);
    
    // Start the manager task
    xTaskCreate(pipeline_task, "voice_pipeline", 4096, NULL, 5, &pipeline_task_handle);

    return ESP_OK;
}

esp_err_t voice_pipeline_start(void) {
    ESP_LOGI(TAG, "Starting Voice Pipeline (Wake Word Mode)");
    pipeline_post_cmd(PIPELINE_CMD_RESUME_WWD, 0);
    return ESP_OK;
}

esp_err_t voice_pipeline_stop(void) {
    ESP_LOGI(TAG, "Stopping Voice Pipeline");
    pipeline_post_cmd(PIPELINE_CMD_STOP_WWD, 0);
    return ESP_OK;
}

void voice_pipeline_trigger_wake(void) {
    on_wake_word_detected(NULL, 0);
}

void voice_pipeline_on_music_state_change(bool is_playing) {
    if (is_playing) {
        pipeline_post_cmd(PIPELINE_CMD_STOP_WWD, 0);
    } else {
        pipeline_post_cmd(PIPELINE_CMD_RESUME_WWD, 0);
    }
}

esp_err_t voice_pipeline_update_config(const voice_pipeline_config_t *config) {
    if (!config) return ESP_ERR_INVALID_ARG;
    bool wwd_changed = (fabs(config->wwd_threshold - current_config.wwd_threshold) > 0.01f);
    current_config = *config;
    if (wwd_changed) {
        pipeline_post_cmd(PIPELINE_CMD_RESTART_WWD, 0);
    }
    return ESP_OK;
}

void voice_pipeline_get_config(voice_pipeline_config_t *config) {
    if (config) *config = current_config;
}

bool voice_pipeline_is_running(void) {
    return is_wwd_running;
}

bool voice_pipeline_is_active(void) {
    return is_pipeline_active;
}

void voice_pipeline_test_tts(const char *text) {
    if (text && ha_client_is_connected()) {
        ha_client_request_tts(text);
    }
}

void voice_pipeline_trigger_restart(void) {
    xTaskCreate(restart_task, "restart", 2048, NULL, 3, NULL);
}

void voice_pipeline_trigger_alarm(int alarm_id) {
    pipeline_post_cmd(PIPELINE_CMD_ALARM_BEEP, alarm_id);
}

// =============================================================================
// INTERNAL LOGIC
// =============================================================================

static void pipeline_task(void *arg) {
    sys_diag_wdt_add(); 
    pipeline_cmd_t cmd;

    while (1) {
        // Wait with timeout to allow WDT feeding
        if (xQueueReceive(pipeline_cmd_queue, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) {
            sys_diag_wdt_feed();
            
            switch (cmd.type) {
                case PIPELINE_CMD_WAKE_DETECTED:
                    if (!ha_client_is_connected()) {
                        ESP_LOGW(TAG, "Wake word detected but HA disconnected");
                        pipeline_post_cmd(PIPELINE_CMD_ERROR_BEEP, 0);
                        pipeline_post_cmd(PIPELINE_CMD_RESUME_WWD, 0);
                        break;
                    }
                    audio_capture_stop_wait(100);
                    is_wwd_running = false;
                    vTaskDelay(pdMS_TO_TICKS(50));

                    ESP_LOGI(TAG, "Playing wake confirmation");
                    beep_tone_play(800, 120, 40);
                    vTaskDelay(pdMS_TO_TICKS(50));

                    start_audio_streaming(current_config.vad_max_recording_ms, "wake_word");
                    wake_detect_pending = false;
                    break;

                case PIPELINE_CMD_OFFLINE_CMD:
                    ESP_LOGI(TAG, "⚡ Executing Offline Command ID: %d", cmd.data);
                    beep_tone_play(1000, 100, 80); 
                    
                    audio_capture_stop_wait(100);
                    if (ha_client_is_connected()) ha_client_end_audio_stream();
                    
                    // Actions
                    switch (cmd.data) {
                        case 0: // Light On
                            ESP_LOGI(TAG, "Action: LIGHT ON");
                            led_status_set(LED_STATUS_LISTENING);
                            break;
                        case 1: // Light Off
                            ESP_LOGI(TAG, "Action: LIGHT OFF");
                            led_status_set(LED_STATUS_IDLE);
                            break;
                        case 2: // Music Play
                            ESP_LOGI(TAG, "Action: MUSIC PLAY");
                            if (local_music_player_is_initialized()) local_music_player_play();
                            break;
                        case 3: // Music Stop
                            ESP_LOGI(TAG, "Action: MUSIC STOP");
                            if (local_music_player_is_initialized()) local_music_player_stop();
                            break;
                        case 4: // Next
                            if (local_music_player_is_initialized()) local_music_player_next();
                            break;
                        case 5: // Prev
                            if (local_music_player_is_initialized()) local_music_player_previous();
                            break;
                    }
                    
                    pipeline_post_cmd(PIPELINE_CMD_RESUME_WWD, 0);
                    break;

                case PIPELINE_CMD_RESUME_WWD:
                    if (local_music_player_is_initialized() && 
                    (local_music_player_get_state() == MUSIC_STATE_PLAYING || 
                        local_music_player_get_state() == MUSIC_STATE_PAUSED)) {
                        break;
                    }

                    audio_capture_stop_wait(500);
                    vTaskDelay(pdMS_TO_TICKS(100)); 

                    if (audio_capture_start_wake_word_mode(on_wake_word_detected) == ESP_OK) {
                        is_wwd_running = true;
                        led_status_set(LED_STATUS_IDLE);
                        if (mqtt_ha_is_connected()) mqtt_ha_update_sensor("va_status", "SPREMAN");
                        ESP_LOGI(TAG, "WWD Resumed");
                    }
                    break;

                case PIPELINE_CMD_STOP_WWD:
                    audio_capture_stop_wait(500);
                    is_wwd_running = false;
                    break;

                case PIPELINE_CMD_RESTART_WWD:
                    pipeline_post_cmd(PIPELINE_CMD_STOP_WWD, 0);
                    pipeline_post_cmd(PIPELINE_CMD_RESUME_WWD, 0);
                    break;

                case PIPELINE_CMD_START_FOLLOWUP_VAD:
                    audio_capture_stop_wait(500);
                    led_status_set(LED_STATUS_LISTENING);
                    if (mqtt_ha_is_connected()) mqtt_ha_update_sensor("va_status", "SLUSAM...");
                    
                    if (start_audio_streaming(FOLLOWUP_RECORDING_MS, "follow-up") != ESP_OK) {
                        followup_vad_pending = false;
                        pipeline_post_cmd(PIPELINE_CMD_RESUME_WWD, 0);
                    }
                    break;

                case PIPELINE_CMD_TIMER_BEEP:
                case PIPELINE_CMD_ALARM_BEEP:
                    ESP_LOGI(TAG, "Playing Alarm/Timer Sound!");
                    audio_capture_stop_wait(500);
                    for(int i=0; i<5; i++) {
                        beep_tone_play(1000, 500, 100);
                        vTaskDelay(pdMS_TO_TICKS(500));
                        sys_diag_wdt_feed(); // Feed during long loops
                    }
                    pipeline_post_cmd(PIPELINE_CMD_RESUME_WWD, 0);
                    break;
                
                case PIPELINE_CMD_ERROR_RESUME:
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    pipeline_post_cmd(PIPELINE_CMD_RESUME_WWD, 0);
                    break;
                
                case PIPELINE_CMD_CONFIRM_BEEP:
                    beep_tone_play(1200, 100, 90);
                    vTaskDelay(pdMS_TO_TICKS(120));
                    beep_tone_play(1200, 100, 90);
                    break;

                case PIPELINE_CMD_ERROR_BEEP:
                    beep_tone_play(400, 300, 60);
                    break;

                default: break;
            }
        } else {
            // Idle loop - feed dog
            sys_diag_wdt_feed();
        }
    }
    sys_diag_wdt_remove();
    vTaskDelete(NULL);
}

// =============================================================================
// EVENT HANDLERS
// =============================================================================

static void on_wake_word_detected(const int16_t *audio_data, size_t samples) {
    if (wake_detect_pending) return;
    wake_detect_pending = true;
    led_status_set(LED_STATUS_LISTENING);
    if (mqtt_ha_is_connected()) mqtt_ha_update_sensor("va_status", "SLUŠAM...");
    pipeline_post_cmd(PIPELINE_CMD_WAKE_DETECTED, 0);
}

static void on_offline_cmd_detected(int id, int index) {
    pipeline_post_cmd(PIPELINE_CMD_OFFLINE_CMD, id);
}

static void vad_event_handler(audio_capture_vad_event_t event) {
    if (event == VAD_EVENT_SPEECH_START) {
        ESP_LOGI(TAG, "VAD: Speech Start");
    } else if (event == VAD_EVENT_SPEECH_END) {
        ESP_LOGI(TAG, "VAD: Speech End");
        is_pipeline_active = false;
        audio_capture_stop_wait(0);
        
        if (ha_client_is_connected()) {
            ha_client_end_audio_stream();
            led_status_set(LED_STATUS_PROCESSING);
            if (mqtt_ha_is_connected()) mqtt_ha_update_sensor("va_status", "OBRAĐUJEM...");
        } else {
            ESP_LOGW(TAG, "HA not connected at speech end");
            pipeline_post_cmd(PIPELINE_CMD_ERROR_BEEP, 0);
            pipeline_post_cmd(PIPELINE_CMD_RESUME_WWD, 0);
        }

        if (current_pipeline_handler) {
            free(current_pipeline_handler);
            current_pipeline_handler = NULL;
        }
    }
}

static void audio_capture_handler(const uint8_t *audio_data, size_t length) {
    if (!is_pipeline_active || !current_pipeline_handler) return;

    if (ha_client_is_audio_ready()) {
        if (warmup_chunks_skip > 0) {
            warmup_chunks_skip--;
            return;
        }
        ha_client_stream_audio(audio_data, length, current_pipeline_handler);
    }
}

static esp_err_t start_audio_streaming(uint32_t max_recording_ms, const char *context_tag) {
    // If HA not connected, we still record for VAD, but maybe don't stream?
    // audio_capture_handler handles the check.
    
    audio_capture_enable_vad(NULL, vad_event_handler);
    
    if (ha_client_is_connected()) {
        current_pipeline_handler = ha_client_start_conversation();
    }
    
    is_pipeline_active = true;
    warmup_chunks_skip = 2; 
    return audio_capture_start(audio_capture_handler);
}

static void on_tts_complete(void) {
    if (music_paused_for_tts) {
        local_music_player_resume();
        music_paused_for_tts = false;
    }
    // Only resume WWD if we are not waiting for a follow-up
    if (!followup_vad_pending) {
        pipeline_post_cmd(PIPELINE_CMD_RESUME_WWD, 0);
    } else {
        pipeline_post_cmd(PIPELINE_CMD_START_FOLLOWUP_VAD, 0);
    }
}

static void tts_audio_handler(const uint8_t *audio_data, size_t length) {
    if (local_music_player_is_initialized() && local_music_player_get_state() == MUSIC_STATE_PLAYING) {
        local_music_player_pause();
        music_paused_for_tts = true;
    }
    if (audio_data == NULL || length == 0) {
        // End of stream: signal the player to start playback, but do NOT resume WWD here.
        // Resuming happens from `on_tts_complete()` after audio playback actually finishes.
        (void)tts_player_feed(NULL, 0);
    } else {
        tts_player_feed(audio_data, length);
    }
}

static void conversation_response_handler(const char *response_text, const char *conversation_id) {
    if (response_text && response_text[strlen(response_text)-1] == '?') {
        followup_vad_pending = true;
    } else {
        followup_vad_pending = false;
    }
    
    if (mqtt_ha_is_connected()) {
        mqtt_ha_update_sensor("va_response", response_text ? response_text : "...");
        mqtt_ha_update_sensor("va_status", "GOVORIM...");
    }
    
    if (!response_text || strlen(response_text) == 0) {
        pipeline_post_cmd(PIPELINE_CMD_RESUME_WWD, 0);
    }
}

static void intent_handler(const char *intent_name, const char *intent_data, const char *conversation_id) {
    if (strstr(intent_name, "timer") || strstr(intent_name, "Timer")) {
         ESP_LOGI(TAG, "Timer intent detected locally");
         pipeline_post_cmd(PIPELINE_CMD_CONFIRM_BEEP, 0);
    }
}

static void restart_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}
