#include "voice_pipeline.h"
#include "va_control.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "esp_err.h"
#include "esp_check.h"
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>

#include "audio_capture.h"
#include "ha_client.h"
#include "tts_player.h"
#include "led_status.h"
#include "ota_update.h"
#include "mqtt_ha.h"
#include "local_music_player.h"
#include "beep_tone.h"
#include "cJSON.h"
#include "sys_diag.h"
#include "bsp_board_extra.h"
#include "oled_status.h"

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
static bool suppress_tts_audio = false;
static bool timer_local_handled = false;
static TimerHandle_t local_timer_handle = NULL;
static uint32_t local_timer_seconds = 0;
static uint32_t pending_timer_seconds = 0;
static bool pending_timer_valid = false;
static char last_stt_text[128];
static bool timer_started_from_stt = false;

static char *current_pipeline_handler = NULL;
static int warmup_chunks_skip = 0;
static bool tts_stream_active = false;

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
static void stt_text_handler(const char *text, const char *conversation_id);
static void intent_handler(const char *intent_name, const char *intent_data, const char *conversation_id);
static void conversation_response_handler(const char *response_text, const char *conversation_id);
static esp_err_t start_audio_streaming(uint32_t max_recording_ms, const char *context_tag);
static void tts_audio_handler(const uint8_t *audio_data, size_t length);
static void on_tts_complete(void);
static void restart_task(void *arg);
static void handle_local_music_play(void);
static bool response_requests_music_selection(const char *response_text);
static bool ascii_substr_case_insensitive(const char *haystack, const char *needle);
static int ascii_tolower_int(int c);
static void local_timer_callback(TimerHandle_t timer);
static void local_timer_start(uint32_t seconds);
static void local_timer_stop(void);
static bool parse_timer_seconds_from_intent(const char *intent_data, uint32_t *out_seconds);
static bool parse_duration_string_seconds(const char *text, uint32_t *out_seconds);
static bool parse_iso8601_duration_seconds(const char *text, uint32_t *out_seconds);
static bool parse_number_from_json_value(const cJSON *value, double *out);
static bool parse_timer_seconds_from_text(const char *text, uint32_t *out_seconds);
static bool response_indicates_timer_not_supported(const char *response_text);
static int parse_cro_number_word(const char *word);
static bool is_timer_keyword(const char *word);
static void led_status_set_guarded(led_status_t status);

// Helper to post commands
static void pipeline_post_cmd(pipeline_cmd_type_t type, int data) {
    if (pipeline_cmd_queue) {
        pipeline_cmd_t cmd = {.type = type, .data = data};
        xQueueSend(pipeline_cmd_queue, &cmd, 0);
    }
}

static void led_status_set_guarded(led_status_t status) {
    if (ota_update_is_running()) {
        return;
    }
    led_status_set(status);
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
    ha_client_register_stt_callback(stt_text_handler);

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
                    oled_status_set_last_event("wake");
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
                            led_status_set_guarded(LED_STATUS_LISTENING);
                            break;
                        case 1: // Light Off
                            ESP_LOGI(TAG, "Action: LIGHT OFF");
                            led_status_set_guarded(LED_STATUS_IDLE);
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
                        led_status_set_guarded(LED_STATUS_IDLE);
                        oled_status_set_va_state(OLED_VA_IDLE);
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
                    led_status_set_guarded(LED_STATUS_LISTENING);
                    oled_status_set_va_state(OLED_VA_LISTENING);
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
                    int prev_volume = bsp_extra_codec_volume_get();
                    bsp_extra_codec_volume_set(100, NULL);
                    for(int i=0; i<5; i++) {
                        beep_tone_play(1000, 500, 100);
                        vTaskDelay(pdMS_TO_TICKS(500));
                        sys_diag_wdt_feed(); // Feed during long loops
                    }
                    bsp_extra_codec_volume_set(prev_volume, NULL);
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
                    oled_status_set_va_state(OLED_VA_ERROR);
                    oled_status_set_last_event("err");
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
    timer_local_handled = false;
    suppress_tts_audio = false;
    pending_timer_valid = false;
    timer_started_from_stt = false;
    led_status_set_guarded(LED_STATUS_LISTENING);
    oled_status_set_va_state(OLED_VA_LISTENING);
    oled_status_set_last_event("wake");
    if (mqtt_ha_is_connected()) mqtt_ha_update_sensor("va_status", "SLUŠAM...");
    pipeline_post_cmd(PIPELINE_CMD_WAKE_DETECTED, 0);
}

static void on_offline_cmd_detected(int id, int index) {
    pipeline_post_cmd(PIPELINE_CMD_OFFLINE_CMD, id);
}

static void vad_event_handler(audio_capture_vad_event_t event) {
    if (event == VAD_EVENT_SPEECH_START) {
        ESP_LOGI(TAG, "VAD: Speech Start");
        oled_status_set_va_state(OLED_VA_LISTENING);
        oled_status_set_last_event("vad-start");
    } else if (event == VAD_EVENT_SPEECH_END) {
        ESP_LOGI(TAG, "VAD: Speech End");
        is_pipeline_active = false;
        audio_capture_stop_wait(0);
        
        if (ha_client_is_connected()) {
            ha_client_end_audio_stream();
            led_status_set_guarded(LED_STATUS_PROCESSING);
            oled_status_set_va_state(OLED_VA_PROCESSING);
            oled_status_set_last_event("vad-end");
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

static void stt_text_handler(const char *text, const char *conversation_id) {
    (void)conversation_id;

    if (!text) {
        return;
    }

    strncpy(last_stt_text, text, sizeof(last_stt_text) - 1);
    last_stt_text[sizeof(last_stt_text) - 1] = '\0';
    oled_status_set_last_event("stt");

    pending_timer_valid = parse_timer_seconds_from_text(last_stt_text, &pending_timer_seconds);
    if (pending_timer_valid) {
        ESP_LOGI(TAG, "STT timer candidate: %u seconds", pending_timer_seconds);
        local_timer_start(pending_timer_seconds);
        timer_local_handled = true;
        timer_started_from_stt = true;
        pending_timer_valid = false;
        suppress_tts_audio = true;
        followup_vad_pending = false;
        pipeline_post_cmd(PIPELINE_CMD_CONFIRM_BEEP, 0);
        pipeline_post_cmd(PIPELINE_CMD_RESUME_WWD, 0);
    }
}

static esp_err_t start_audio_streaming(uint32_t max_recording_ms, const char *context_tag) {
    // If HA not connected, we still record for VAD, but maybe don't stream?
    // audio_capture_handler handles the check.
    
    audio_capture_enable_vad(NULL, vad_event_handler);
    
    if (ha_client_is_connected()) {
        current_pipeline_handler = ha_client_start_conversation();
        oled_status_set_last_event("run-start");
    }
    
    is_pipeline_active = true;
    oled_status_set_va_state(OLED_VA_LISTENING);
    warmup_chunks_skip = 2; 
    return audio_capture_start(audio_capture_handler);
}

static void on_tts_complete(void) {
    tts_stream_active = false;
    oled_status_set_tts_state(OLED_TTS_IDLE);
    oled_status_set_last_event("tts-done");
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
    if (suppress_tts_audio) {
        if (audio_data == NULL || length == 0) {
            suppress_tts_audio = false;
            on_tts_complete();
        }
        return;
    }

    if (local_music_player_is_initialized() && local_music_player_get_state() == MUSIC_STATE_PLAYING) {
        local_music_player_pause();
        music_paused_for_tts = true;
    }
    if (audio_data == NULL || length == 0) {
        // End of stream: signal the player to start playback, but do NOT resume WWD here.
        // Resuming happens from `on_tts_complete()` after audio playback actually finishes.
        oled_status_set_tts_state(OLED_TTS_PLAYING);
        oled_status_set_last_event("tts-play");
        (void)tts_player_feed(NULL, 0);
    } else {
        if (!tts_stream_active) {
            tts_stream_active = true;
            oled_status_set_tts_state(OLED_TTS_DOWNLOADING);
            oled_status_set_last_event("tts-start");
            oled_status_set_va_state(OLED_VA_SPEAKING);
        }
        tts_player_feed(audio_data, length);
    }
}

static void conversation_response_handler(const char *response_text, const char *conversation_id) {
    if (pending_timer_valid && response_indicates_timer_not_supported(response_text)) {
        local_timer_start(pending_timer_seconds);
        timer_local_handled = true;
        pending_timer_valid = false;
        suppress_tts_audio = true;
        followup_vad_pending = false;
        pipeline_post_cmd(PIPELINE_CMD_CONFIRM_BEEP, 0);
        pipeline_post_cmd(PIPELINE_CMD_RESUME_WWD, 0);
        return;
    }

    if (timer_local_handled) {
        timer_local_handled = false;
        suppress_tts_audio = true;
        followup_vad_pending = false;
        oled_status_set_response_preview("TIMER");
        if (mqtt_ha_is_connected()) {
            mqtt_ha_update_sensor("va_response", local_timer_seconds > 0 ? "TIMER POSTAVLJEN" : "TIMER");
            mqtt_ha_update_sensor("va_status", "SPREMAN");
        }
        pipeline_post_cmd(PIPELINE_CMD_RESUME_WWD, 0);
        return;
    }

    bool local_music_ready = local_music_player_is_initialized();
    if (local_music_ready && response_requests_music_selection(response_text)) {
        ESP_LOGI(TAG, "HA asked for music selection; playing local SD music");
        suppress_tts_audio = true;
        followup_vad_pending = false;
        oled_status_set_response_preview("GLAZBA");
        handle_local_music_play();
        if (mqtt_ha_is_connected()) {
            mqtt_ha_update_sensor("va_response", "PUSTAM GLAZBU");
            mqtt_ha_update_sensor("va_status", "GLAZBA...");
        }
        return;
    }

    if (response_text && response_text[0] && response_text[strlen(response_text)-1] == '?') {
        followup_vad_pending = true;
    } else {
        followup_vad_pending = false;
    }
    
    if (mqtt_ha_is_connected()) {
        mqtt_ha_update_sensor("va_response", response_text ? response_text : "...");
        mqtt_ha_update_sensor("va_status", "GOVORIM...");
    }
    oled_status_set_response_preview(response_text ? response_text : "");
    
    if (!response_text || strlen(response_text) == 0) {
        pipeline_post_cmd(PIPELINE_CMD_RESUME_WWD, 0);
    }
}

static void intent_handler(const char *intent_name, const char *intent_data, const char *conversation_id) {
    (void)conversation_id;

    if (!intent_name) {
        return;
    }

    ESP_LOGI(TAG, "HA intent: %s", intent_name);
    oled_status_set_last_event("intent-end");

    if (strstr(intent_name, "Timer") || strstr(intent_name, "timer")) {
        if (timer_started_from_stt &&
            strcmp(intent_name, "HassTimerCancel") != 0 &&
            strcmp(intent_name, "HassTimerStop") != 0) {
            return;
        }
        uint32_t seconds = 0;
        if (parse_timer_seconds_from_intent(intent_data, &seconds) && seconds > 0) {
            local_timer_start(seconds);
        } else if (pending_timer_valid && pending_timer_seconds > 0) {
            local_timer_start(pending_timer_seconds);
            pending_timer_valid = false;
        } else if (strcmp(intent_name, "HassTimerCancel") == 0 ||
                   strcmp(intent_name, "HassTimerStop") == 0) {
            local_timer_stop();
            timer_started_from_stt = false;
        } else {
            ESP_LOGW(TAG, "Timer intent missing duration");
        }
        timer_local_handled = true;
        suppress_tts_audio = true;
        pending_timer_valid = false;
        followup_vad_pending = false;
        pipeline_post_cmd(PIPELINE_CMD_CONFIRM_BEEP, 0);
        pipeline_post_cmd(PIPELINE_CMD_RESUME_WWD, 0);
        return;
    }

    if (strcmp(intent_name, "HassMediaNext") == 0) {
        if (local_music_player_is_initialized()) local_music_player_next();
        suppress_tts_audio = true;
        return;
    }
    if (strcmp(intent_name, "HassMediaPrevious") == 0) {
        if (local_music_player_is_initialized()) local_music_player_previous();
        suppress_tts_audio = true;
        return;
    }
    if (strcmp(intent_name, "HassMediaStop") == 0) {
        if (local_music_player_is_initialized()) local_music_player_stop();
        suppress_tts_audio = true;
        return;
    }
    if (strcmp(intent_name, "HassMediaPause") == 0) {
        if (local_music_player_is_initialized()) local_music_player_pause();
        suppress_tts_audio = true;
        return;
    }
    if (strcmp(intent_name, "HassMediaPlayPause") == 0) {
        if (local_music_player_is_initialized()) {
            music_state_t state = local_music_player_get_state();
            if (state == MUSIC_STATE_PLAYING) {
                local_music_player_pause();
            } else if (state == MUSIC_STATE_PAUSED) {
                local_music_player_resume();
            } else {
                local_music_player_play();
            }
        }
        suppress_tts_audio = true;
        return;
    }
    if (strcmp(intent_name, "HassMediaPlay") == 0 || strcmp(intent_name, "HassMediaUnpause") == 0) {
        handle_local_music_play();
        suppress_tts_audio = true;
        return;
    }

    if (strstr(intent_name, "timer") || strstr(intent_name, "Timer")) {
        ESP_LOGI(TAG, "Timer intent detected locally");
        pipeline_post_cmd(PIPELINE_CMD_CONFIRM_BEEP, 0);
    }
}

static void handle_local_music_play(void) {
    if (!local_music_player_is_initialized()) {
        ESP_LOGW(TAG, "Local music player not initialized");
        return;
    }

    music_state_t state = local_music_player_get_state();
    if (state == MUSIC_STATE_PAUSED) {
        local_music_player_resume();
    } else if (state != MUSIC_STATE_PLAYING) {
        local_music_player_play();
    }
}

static void local_timer_callback(TimerHandle_t timer) {
    (void)timer;
    local_timer_seconds = 0;
    pipeline_post_cmd(PIPELINE_CMD_TIMER_BEEP, 0);
}

static void local_timer_start(uint32_t seconds) {
    if (seconds == 0) {
        return;
    }

    local_timer_seconds = seconds;

    if (local_timer_handle == NULL) {
        local_timer_handle = xTimerCreate("va_timer", pdMS_TO_TICKS(1000), pdFALSE, NULL, local_timer_callback);
        if (local_timer_handle == NULL) {
            ESP_LOGE(TAG, "Failed to create local timer");
            return;
        }
    }

    uint64_t duration_ms = (uint64_t)seconds * 1000ULL;
    if (duration_ms > UINT32_MAX) {
        duration_ms = UINT32_MAX;
    }

    xTimerStop(local_timer_handle, 0);
    xTimerChangePeriod(local_timer_handle, pdMS_TO_TICKS((uint32_t)duration_ms), 0);
    xTimerStart(local_timer_handle, 0);
    ESP_LOGI(TAG, "Local timer set: %u seconds", seconds);
}

static void local_timer_stop(void) {
    if (local_timer_handle == NULL) {
        return;
    }
    xTimerStop(local_timer_handle, 0);
    local_timer_seconds = 0;
    ESP_LOGI(TAG, "Local timer stopped");
}

static bool parse_timer_seconds_from_intent(const char *intent_data, uint32_t *out_seconds) {
    if (!intent_data || !out_seconds) {
        return false;
    }

    cJSON *root = cJSON_Parse(intent_data);
    if (!root) {
        return false;
    }

    uint32_t total_seconds = 0;
    const cJSON *slots = cJSON_GetObjectItemCaseSensitive(root, "slots");

    if (slots && cJSON_IsObject(slots)) {
        const cJSON *slot = NULL;
        for (slot = slots->child; slot != NULL; slot = slot->next) {
            const char *name = slot->string;
            if (!name) continue;

            const cJSON *value = cJSON_GetObjectItemCaseSensitive((cJSON *)slot, "value");
            if (!value) value = slot;

            if (strcmp(name, "hours") == 0 || strcmp(name, "hour") == 0) {
                double v = 0;
                if (parse_number_from_json_value(value, &v) && v > 0) {
                    total_seconds += (uint32_t)(v * 3600.0);
                }
            } else if (strcmp(name, "minutes") == 0 || strcmp(name, "minute") == 0) {
                double v = 0;
                if (parse_number_from_json_value(value, &v) && v > 0) {
                    total_seconds += (uint32_t)(v * 60.0);
                }
            } else if (strcmp(name, "seconds") == 0 || strcmp(name, "second") == 0) {
                double v = 0;
                if (parse_number_from_json_value(value, &v) && v > 0) {
                    total_seconds += (uint32_t)(v);
                }
            } else if (strcmp(name, "duration") == 0) {
                if (cJSON_IsString(value) && value->valuestring) {
                    uint32_t parsed = 0;
                    if (parse_duration_string_seconds(value->valuestring, &parsed) && parsed > 0) {
                        total_seconds += parsed;
                    }
                } else if (cJSON_IsObject(value)) {
                    const cJSON *sec = cJSON_GetObjectItemCaseSensitive((cJSON *)value, "seconds");
                    const cJSON *min = cJSON_GetObjectItemCaseSensitive((cJSON *)value, "minutes");
                    const cJSON *hr = cJSON_GetObjectItemCaseSensitive((cJSON *)value, "hours");
                    double v = 0;
                    if (parse_number_from_json_value(hr, &v) && v > 0) total_seconds += (uint32_t)(v * 3600.0);
                    if (parse_number_from_json_value(min, &v) && v > 0) total_seconds += (uint32_t)(v * 60.0);
                    if (parse_number_from_json_value(sec, &v) && v > 0) total_seconds += (uint32_t)v;
                }
            }
        }
    }

    cJSON_Delete(root);

    if (total_seconds == 0) {
        return false;
    }

    *out_seconds = total_seconds;
    return true;
}

static bool parse_duration_string_seconds(const char *text, uint32_t *out_seconds) {
    if (!text || !out_seconds) {
        return false;
    }

    if (parse_iso8601_duration_seconds(text, out_seconds)) {
        return true;
    }

    int parts[3] = {0, 0, 0};
    int count = 0;
    const char *p = text;
    while (*p && count < 3) {
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p) break;
        parts[count++] = (int)v;
        if (*end == ':') {
            p = end + 1;
        } else {
            p = end;
            break;
        }
    }

    if (count == 3) {
        *out_seconds = (uint32_t)(parts[0] * 3600 + parts[1] * 60 + parts[2]);
        return true;
    }
    if (count == 2) {
        *out_seconds = (uint32_t)(parts[0] * 60 + parts[1]);
        return true;
    }
    if (count == 1) {
        *out_seconds = (uint32_t)parts[0];
        return true;
    }

    return false;
}

static bool parse_iso8601_duration_seconds(const char *text, uint32_t *out_seconds) {
    if (!text || !out_seconds) {
        return false;
    }

    if (strncmp(text, "PT", 2) != 0) {
        return false;
    }

    uint32_t total = 0;
    const char *p = text + 2;
    while (*p) {
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p) return false;
        if (*end == 'H') {
            total += (uint32_t)(v * 3600);
        } else if (*end == 'M') {
            total += (uint32_t)(v * 60);
        } else if (*end == 'S') {
            total += (uint32_t)v;
        } else {
            return false;
        }
        p = end + 1;
    }

    if (total == 0) {
        return false;
    }

    *out_seconds = total;
    return true;
}

static bool parse_number_from_json_value(const cJSON *value, double *out) {
    if (!value || !out) {
        return false;
    }
    if (cJSON_IsNumber(value)) {
        *out = value->valuedouble;
        return true;
    }
    if (cJSON_IsString(value) && value->valuestring) {
        char *end = NULL;
        double v = strtod(value->valuestring, &end);
        if (end != value->valuestring) {
            *out = v;
            return true;
        }
    }
    if (cJSON_IsObject(value)) {
        const cJSON *v_item = cJSON_GetObjectItemCaseSensitive((cJSON *)value, "value");
        if (v_item) {
            return parse_number_from_json_value(v_item, out);
        }
    }
    return false;
}

static bool parse_timer_seconds_from_text(const char *text, uint32_t *out_seconds) {
    if (!text || !out_seconds) {
        return false;
    }

    bool has_timer = false;
    uint32_t total_seconds = 0;
    double pending = -1.0;
    const char *p = text;

    while (*p) {
        while (*p && !isalnum((unsigned char)*p) && *p != ':' && *p != 'P' && *p != 'p') {
            p++;
        }
        if (!*p) break;

        char word[32];
        size_t len = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == ':' || *p == '-')) {
            if (len < sizeof(word) - 1) {
                word[len++] = *p;
            }
            p++;
        }
        word[len] = '\0';

        char lower[32];
        for (size_t i = 0; i < len && i < sizeof(lower) - 1; i++) {
            lower[i] = (char)ascii_tolower_int((unsigned char)word[i]);
        }
        lower[len < sizeof(lower) - 1 ? len : sizeof(lower) - 1] = '\0';

        if (is_timer_keyword(lower)) {
            has_timer = true;
            continue;
        }

        if (strcmp(lower, "pola") == 0) {
            pending = 0.5;
            continue;
        }

        if (strchr(lower, ':') || (lower[0] == 'p' && lower[1] == 't')) {
            uint32_t parsed = 0;
            if (parse_duration_string_seconds(lower, &parsed) && parsed > 0) {
                total_seconds += parsed;
                pending = -1.0;
            }
            continue;
        }

        int word_num = parse_cro_number_word(lower);
        if (word_num >= 0) {
            pending = (double)word_num;
            continue;
        }

        char *end = NULL;
        double v = strtod(lower, &end);
        if (end != lower && *end == '\0') {
            pending = v;
            continue;
        }

        if (pending > 0) {
            if (strcmp(lower, "sat") == 0 || strcmp(lower, "sata") == 0 ||
                strcmp(lower, "sati") == 0 || strcmp(lower, "satova") == 0) {
                total_seconds += (uint32_t)(pending * 3600.0);
                pending = -1.0;
            } else if (strcmp(lower, "min") == 0 || strcmp(lower, "minuta") == 0 ||
                       strcmp(lower, "minute") == 0 || strcmp(lower, "minutu") == 0 ||
                       strcmp(lower, "minut") == 0) {
                total_seconds += (uint32_t)(pending * 60.0);
                pending = -1.0;
            } else if (strcmp(lower, "sek") == 0 || strcmp(lower, "sekunda") == 0 ||
                       strcmp(lower, "sekundi") == 0 || strcmp(lower, "sekunde") == 0 ||
                       strcmp(lower, "sekundu") == 0) {
                total_seconds += (uint32_t)(pending);
                pending = -1.0;
            }
        }
    }

    if (total_seconds == 0 || !has_timer) {
        return false;
    }

    *out_seconds = total_seconds;
    return true;
}

static bool response_indicates_timer_not_supported(const char *response_text) {
    if (!response_text || response_text[0] == '\0') {
        return false;
    }

    if (ascii_substr_case_insensitive(response_text, "ne mogu postavljati timere")) {
        return true;
    }
    if (ascii_substr_case_insensitive(response_text, "ne mogu postaviti timer")) {
        return true;
    }
    if (ascii_substr_case_insensitive(response_text, "ne mogu postavljati timer")) {
        return true;
    }
    if (ascii_substr_case_insensitive(response_text, "ne mogu namjestati timer")) {
        return true;
    }
    if (ascii_substr_case_insensitive(response_text, "ne mogu namjestiti timer")) {
        return true;
    }

    return false;
}

static int parse_cro_number_word(const char *word) {
    if (!word) return -1;

    if (strcmp(word, "nula") == 0) return 0;
    if (strcmp(word, "jedan") == 0 || strcmp(word, "jedna") == 0 || strcmp(word, "jednu") == 0) return 1;
    if (strcmp(word, "dva") == 0 || strcmp(word, "dvije") == 0) return 2;
    if (strcmp(word, "tri") == 0) return 3;
    if (strcmp(word, "cetiri") == 0) return 4;
    if (strcmp(word, "pet") == 0) return 5;
    if (strcmp(word, "sest") == 0) return 6;
    if (strcmp(word, "sedam") == 0) return 7;
    if (strcmp(word, "osam") == 0) return 8;
    if (strcmp(word, "devet") == 0) return 9;
    if (strcmp(word, "deset") == 0) return 10;
    if (strcmp(word, "jedanaest") == 0) return 11;
    if (strcmp(word, "dvanaest") == 0) return 12;

    return -1;
}

static bool is_timer_keyword(const char *word) {
    if (!word) return false;
    if (strcmp(word, "timer") == 0 || strcmp(word, "tajmer") == 0) return true;
    if (strcmp(word, "odbrojavanje") == 0 || strcmp(word, "odbroj") == 0) return true;
    return false;
}
static bool response_requests_music_selection(const char *response_text) {
    if (!response_text || response_text[0] == '\0') {
        return false;
    }

    if (ascii_substr_case_insensitive(response_text, "koju glazbu")) {
        return true;
    }
    if (ascii_substr_case_insensitive(response_text, "koju pjesmu")) {
        return true;
    }
    if (ascii_substr_case_insensitive(response_text, "sto zelis slusati")) {
        return true;
    }
    if (ascii_substr_case_insensitive(response_text, "sto zelite slusati")) {
        return true;
    }

    return false;
}

static bool ascii_substr_case_insensitive(const char *haystack, const char *needle) {
    if (!haystack || !needle) {
        return false;
    }

    size_t nlen = strlen(needle);
    if (nlen == 0) {
        return true;
    }

    for (const char *p = haystack; *p; p++) {
        if (ascii_tolower_int((unsigned char)*p) == ascii_tolower_int((unsigned char)needle[0])) {
            size_t i = 1;
            for (; i < nlen; i++) {
                char hc = p[i];
                if (hc == '\0') {
                    break;
                }
                if (ascii_tolower_int((unsigned char)hc) != ascii_tolower_int((unsigned char)needle[i])) {
                    break;
                }
            }
            if (i == nlen) {
                return true;
            }
        }
    }

    return false;
}

static int ascii_tolower_int(int c) {
    if (c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }
    return c;
}

static void restart_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

// =============================================================================
// VA CONTROL IMPLEMENTATION
// =============================================================================

float va_control_get_wwd_threshold(void) {
    return current_config.wwd_threshold;
}

uint32_t va_control_get_vad_threshold(void) {
    return current_config.vad_speech_threshold;
}

uint32_t va_control_get_vad_silence_duration_ms(void) {
    return current_config.vad_silence_ms;
}

uint32_t va_control_get_vad_min_speech_ms(void) {
    return current_config.vad_min_speech_ms;
}

uint32_t va_control_get_vad_max_recording_ms(void) {
    return current_config.vad_max_recording_ms;
}

bool va_control_get_agc_enabled(void) {
    return current_config.agc_enabled;
}

uint16_t va_control_get_agc_target_level(void) {
    return current_config.agc_target_level;
}

bool va_control_get_pipeline_active(void) {
    return is_pipeline_active;
}

bool va_control_get_wwd_running(void) {
    return is_wwd_running;
}

esp_err_t va_control_set_wwd_threshold(float threshold) {
    voice_pipeline_config_t cfg = current_config;
    cfg.wwd_threshold = threshold;
    return voice_pipeline_update_config(&cfg);
}

esp_err_t va_control_set_vad_threshold(uint32_t threshold) {
    voice_pipeline_config_t cfg = current_config;
    cfg.vad_speech_threshold = threshold;
    return voice_pipeline_update_config(&cfg);
}

esp_err_t va_control_set_vad_silence_duration_ms(uint32_t ms) {
    voice_pipeline_config_t cfg = current_config;
    cfg.vad_silence_ms = ms;
    return voice_pipeline_update_config(&cfg);
}

esp_err_t va_control_set_vad_min_speech_ms(uint32_t ms) {
    voice_pipeline_config_t cfg = current_config;
    cfg.vad_min_speech_ms = ms;
    return voice_pipeline_update_config(&cfg);
}

esp_err_t va_control_set_vad_max_recording_ms(uint32_t ms) {
    voice_pipeline_config_t cfg = current_config;
    cfg.vad_max_recording_ms = ms;
    return voice_pipeline_update_config(&cfg);
}

esp_err_t va_control_set_agc_enabled(bool enabled) {
    voice_pipeline_config_t cfg = current_config;
    cfg.agc_enabled = enabled;
    return voice_pipeline_update_config(&cfg);
}

esp_err_t va_control_set_agc_target_level(uint16_t target_level) {
    voice_pipeline_config_t cfg = current_config;
    cfg.agc_target_level = target_level;
    return voice_pipeline_update_config(&cfg);
}

void va_control_action_restart(void) {
    voice_pipeline_trigger_restart();
}

void va_control_action_wwd_resume(void) {
    pipeline_post_cmd(PIPELINE_CMD_RESUME_WWD, 0);
}

void va_control_action_wwd_stop(void) {
    pipeline_post_cmd(PIPELINE_CMD_STOP_WWD, 0);
}

void va_control_action_test_tts(const char *text) {
    voice_pipeline_test_tts(text);
}
