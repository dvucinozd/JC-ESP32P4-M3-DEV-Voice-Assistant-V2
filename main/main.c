#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_check.h"

// Hardware / Drivers
#include "bsp/esp32_p4_function_ev_board.h"
#include "bsp_board_extra.h"
#include "driver/sdspi_host.h"

// Modules
#include "config.h"
#include "settings_manager.h" 
#include "wifi_manager.h"
#include "network_manager.h"
#include "mqtt_ha.h"
#include "ha_client.h" 
#include "ota_update.h"
#include "led_status.h"
#include "voice_pipeline.h"
#include "va_control.h"
#include "webserial.h"
#include "local_music_player.h"
#include "audio_capture.h"
#include "alarm_manager.h" 
#include "sys_diag.h" // Phase 9

#define TAG "main"

static bool sd_init_done = false;
static TaskHandle_t post_connect_task_handle = NULL;
static char ota_url_value[256] = {0};
static TaskHandle_t music_control_task_handle = NULL;
static bool audio_hw_ready = false;
static TaskHandle_t metrics_task_handle = NULL;

static const char *ota_state_to_string(ota_state_t state);
static const char *music_state_to_string(music_state_t state);
static void mqtt_update_music_state(music_state_t state, int current_track, int total_tracks);
static void mqtt_publish_telemetry(void);
static void mqtt_metrics_task(void *arg);
static bool get_wifi_rssi(int *out_rssi);
static void ota_progress_handler(ota_state_t state, int progress, const char *message);

typedef enum {
    MUSIC_CMD_PLAY = 0,
    MUSIC_CMD_STOP = 1,
} music_cmd_t;

static void music_control_task(void *arg) {
    music_cmd_t cmd = (music_cmd_t)(uintptr_t)arg;

    if (cmd == MUSIC_CMD_PLAY) {
        ESP_LOGI(TAG, "Music play requested (stopping voice pipeline first)");
        voice_pipeline_stop();

        // Wait briefly for WWD to stop and release I2S/codec.
        for (int i = 0; i < 30; i++) {
            if (!voice_pipeline_is_running()) break;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        vTaskDelay(pdMS_TO_TICKS(150));

        if (local_music_player_is_initialized()) {
            (void)local_music_player_play();
        }
    } else if (cmd == MUSIC_CMD_STOP) {
        ESP_LOGI(TAG, "Music stop requested");
        if (local_music_player_is_initialized()) {
            (void)local_music_player_stop();
        }
        vTaskDelay(pdMS_TO_TICKS(150));
        voice_pipeline_start();
    }

    music_control_task_handle = NULL;
    vTaskDelete(NULL);
}

static const char *ota_state_to_string(ota_state_t state) {
    switch (state) {
        case OTA_STATE_IDLE:
            return "IDLE";
        case OTA_STATE_DOWNLOADING:
            return "DOWNLOADING";
        case OTA_STATE_VERIFYING:
            return "VERIFYING";
        case OTA_STATE_SUCCESS:
            return "SUCCESS";
        case OTA_STATE_FAILED:
            return "FAILED";
        default:
            return "UNKNOWN";
    }
}

static const char *music_state_to_string(music_state_t state) {
    switch (state) {
        case MUSIC_STATE_PLAYING:
            return "PLAYING";
        case MUSIC_STATE_PAUSED:
            return "PAUSED";
        case MUSIC_STATE_STOPPED:
            return "STOPPED";
        case MUSIC_STATE_IDLE:
        default:
            return "IDLE";
    }
}

static bool get_wifi_rssi(int *out_rssi) {
    if (!out_rssi) return false;

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return false;
    }
    *out_rssi = ap_info.rssi;
    return true;
}

static void mqtt_update_music_state(music_state_t state, int current_track, int total_tracks) {
    if (!mqtt_ha_is_connected()) {
        return;
    }

    mqtt_ha_update_sensor("music_state", music_state_to_string(state));

    char buf[32];
    snprintf(buf, sizeof(buf), "%d", total_tracks);
    mqtt_ha_update_sensor("total_tracks", buf);

    if (current_track >= 0) {
        char name[64];
        if (local_music_player_get_track_name(name, sizeof(name)) == ESP_OK) {
            mqtt_ha_update_sensor("current_track", name);
        } else {
            snprintf(buf, sizeof(buf), "%d", current_track + 1);
            mqtt_ha_update_sensor("current_track", buf);
        }
    } else {
        mqtt_ha_update_sensor("current_track", "None");
    }
}

static void mqtt_publish_telemetry(void) {
    if (!mqtt_ha_is_connected()) {
        return;
    }

    char buf[64];

    snprintf(buf, sizeof(buf), "%u", (unsigned int)esp_get_free_heap_size());
    mqtt_ha_update_sensor("free_memory", buf);

    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)(esp_timer_get_time() / 1000000ULL));
    mqtt_ha_update_sensor("uptime", buf);

    network_type_t active = network_manager_get_active_type();
    mqtt_ha_update_sensor("network_type", network_manager_type_to_string(active));

    int rssi = 0;
    if (get_wifi_rssi(&rssi)) {
        snprintf(buf, sizeof(buf), "%d", rssi);
        mqtt_ha_update_sensor("wifi_rssi", buf);
        mqtt_ha_update_sensor("wifi_signal", buf);
    }

    float agc_gain = audio_capture_get_agc_gain();
    snprintf(buf, sizeof(buf), "%.2f", (double)agc_gain);
    mqtt_ha_update_sensor("agc_current_gain", buf);

    snprintf(buf, sizeof(buf), "%d", webserial_get_client_count());
    mqtt_ha_update_sensor("webserial_clients", buf);

    mqtt_update_music_state(local_music_player_get_state(),
                            local_music_player_get_current_track(),
                            local_music_player_get_total_tracks());

    mqtt_ha_update_sensor("ota_status", ota_state_to_string(ota_update_get_state()));
    snprintf(buf, sizeof(buf), "%d", ota_update_get_progress());
    mqtt_ha_update_sensor("ota_progress", buf);

    mqtt_ha_update_sensor("sd_card_status", bsp_sdcard ? "MOUNTED" : "NOT_MOUNTED");

    const char *fw = ota_update_get_current_version();
    if (!fw || fw[0] == '\0') {
        fw = FIRMWARE_VERSION;
    }
    mqtt_ha_update_sensor("firmware_version", fw);

    if (ota_url_value[0] != '\0') {
        mqtt_ha_update_sensor("ota_update_url", ota_url_value);
    }

    float wwd = va_control_get_wwd_threshold();
    mqtt_ha_update_number("wwd_detection_threshold", wwd);

    mqtt_ha_update_switch("auto_gain_control", va_control_get_agc_enabled());
    mqtt_ha_update_number("agc_target_level", (float)va_control_get_agc_target_level());

    mqtt_ha_update_switch("led_status_indicator", led_status_is_enabled());
    mqtt_ha_update_switch("wwd_enabled", voice_pipeline_is_running());
}

static void mqtt_metrics_task(void *arg) {
    (void)arg;
    while (1) {
        mqtt_publish_telemetry();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void ota_progress_handler(ota_state_t state, int progress, const char *message) {
    (void)message;
    if (!mqtt_ha_is_connected()) {
        return;
    }

    mqtt_ha_update_sensor("ota_status", ota_state_to_string(state));
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", progress);
    mqtt_ha_update_sensor("ota_progress", buf);
}

static void post_connect_task(void *arg) {
    network_type_t type = (network_type_t)(uintptr_t)arg;

    char ip_str[16];
    if (network_manager_get_ip(ip_str) != ESP_OK) {
        strncpy(ip_str, "0.0.0.0", sizeof(ip_str));
        ip_str[sizeof(ip_str) - 1] = '\0';
    }

    ESP_LOGI(TAG, "Network Connected: %s (IP: %s)", network_manager_type_to_string(type), ip_str);

    if (mqtt_ha_is_connected()) {
        mqtt_ha_update_sensor("ip_address", ip_str);
    }

    // Start web dashboard once network is up.
    webserial_init();

    // SD/music init can be slow; keep it out of the network event loop task.
    if (!sys_diag_is_safe_mode() && !sd_init_done) {
        if (bsp_sdcard_mount() == ESP_OK) {
            ESP_LOGI(TAG, "SD Card mounted");
            local_music_player_init();
            local_music_player_register_callback(NULL); // Or proper callback
            sd_init_done = true;
        }
    }

    post_connect_task_handle = NULL;
    vTaskDelete(NULL);
}

// MQTT Callbacks (Keep implementation same)
static void mqtt_wwd_switch_callback(const char *entity_id, const char *payload) {
    if (strcmp(payload, "ON") == 0) {
        voice_pipeline_start();
        mqtt_ha_update_switch("wwd_enabled", true);
    } else {
        voice_pipeline_stop();
        mqtt_ha_update_switch("wwd_enabled", false);
    }
}

static void mqtt_restart_callback(const char *entity_id, const char *payload) {
    ESP_LOGI(TAG, "Restart requested via MQTT");
    voice_pipeline_trigger_restart();
}

static void mqtt_test_tts_callback(const char *entity_id, const char *payload) {
    voice_pipeline_test_tts("Ovo je test govora.");
}

static void mqtt_music_play_callback(const char *entity_id, const char *payload) {
    (void)entity_id;
    (void)payload;
    if (music_control_task_handle == NULL) {
        xTaskCreate(music_control_task, "music_ctl", 4096, (void *)(uintptr_t)MUSIC_CMD_PLAY, 5,
                    &music_control_task_handle);
    }
}

static void mqtt_music_stop_callback(const char *entity_id, const char *payload) {
    (void)entity_id;
    (void)payload;
    if (music_control_task_handle == NULL) {
        xTaskCreate(music_control_task, "music_ctl", 4096, (void *)(uintptr_t)MUSIC_CMD_STOP, 5,
                    &music_control_task_handle);
    }
}

static void mqtt_led_test_callback(const char *entity_id, const char *payload) {
    (void)entity_id;
    (void)payload;
    led_status_test_pattern();
}

static void mqtt_led_brightness_callback(const char *entity_id, const char *payload) {
    (void)entity_id;
    if (!payload) return;

    char *end = NULL;
    float v = strtof(payload, &end);
    if (end == payload) {
        ESP_LOGW(TAG, "Invalid LED brightness payload: %s", payload);
        return;
    }

    if (v < 0.0f) v = 0.0f;
    if (v > 100.0f) v = 100.0f;

    uint8_t b = (uint8_t)lroundf(v);
    led_status_set_brightness(b);
    (void)mqtt_ha_update_number("led_brightness", (float)b);
}

static void mqtt_output_volume_callback(const char *entity_id, const char *payload) {
    (void)entity_id;
    if (!payload) return;

    char *end = NULL;
    float v = strtof(payload, &end);
    if (end == payload) {
        ESP_LOGW(TAG, "Invalid output volume payload: %s", payload);
        return;
    }

    if (v < 0.0f) v = 0.0f;
    if (v > 100.0f) v = 100.0f;
    int vol = (int)lroundf(v);

    // Apply immediately (affects TTS, music, beeps)
    if (!audio_hw_ready) {
        ESP_LOGW(TAG, "Output volume set requested but audio hardware is not ready");
        return;
    }
    bsp_extra_codec_volume_set(vol, NULL);
    (void)mqtt_ha_update_number("output_volume", (float)vol);

    // Persist to NVS
    app_settings_t s;
    if (settings_manager_load(&s) == ESP_OK) {
        s.output_volume = vol;
        (void)settings_manager_save(&s);
    }
}

static void mqtt_wwd_threshold_callback(const char *entity_id, const char *payload) {
    (void)entity_id;
    if (!payload) return;

    char *end = NULL;
    float v = strtof(payload, &end);
    if (end == payload) {
        ESP_LOGW(TAG, "Invalid WWD threshold payload: %s", payload);
        return;
    }

    if (v < 0.5f) v = 0.5f;
    if (v > 0.95f) v = 0.95f;

    va_control_set_wwd_threshold(v);
    mqtt_ha_update_number("wwd_detection_threshold", v);
}

static void mqtt_agc_enabled_callback(const char *entity_id, const char *payload) {
    (void)entity_id;
    if (!payload) return;

    bool enable = (strcmp(payload, "ON") == 0);
    va_control_set_agc_enabled(enable);
    mqtt_ha_update_switch("auto_gain_control", enable);
}

static void mqtt_agc_target_callback(const char *entity_id, const char *payload) {
    (void)entity_id;
    if (!payload) return;

    char *end = NULL;
    float v = strtof(payload, &end);
    if (end == payload) {
        ESP_LOGW(TAG, "Invalid AGC target payload: %s", payload);
        return;
    }

    if (v < 0.0f) v = 0.0f;
    if (v > 10000.0f) v = 10000.0f;

    uint16_t target = (uint16_t)lroundf(v);
    va_control_set_agc_target_level(target);
    mqtt_ha_update_number("agc_target_level", (float)target);
}

static void mqtt_led_indicator_callback(const char *entity_id, const char *payload) {
    (void)entity_id;
    if (!payload) return;

    bool enable = (strcmp(payload, "ON") == 0);
    led_status_enable(enable);
    mqtt_ha_update_switch("led_status_indicator", enable);
}

static void mqtt_ota_url_callback(const char *entity_id, const char *payload) {
    (void)entity_id;
    if (!payload) return;

    strncpy(ota_url_value, payload, sizeof(ota_url_value) - 1);
    ota_url_value[sizeof(ota_url_value) - 1] = '\0';

    ESP_LOGI(TAG, "OTA URL set via MQTT: %s", ota_url_value);
    (void)mqtt_ha_update_text("ota_url_input", ota_url_value);

    // Save to settings
    app_settings_t s;
    if (settings_manager_load(&s) == ESP_OK) {
        strncpy(s.ota_url, ota_url_value, sizeof(s.ota_url)-1);
        s.ota_url[sizeof(s.ota_url)-1] = '\0';
        settings_manager_save(&s);
    }
}

static void mqtt_ota_trigger_callback(const char *entity_id, const char *payload) {
    (void)entity_id;
    (void)payload;

    if (ota_url_value[0] == '\0') {
        ESP_LOGW(TAG, "OTA trigger pressed but OTA URL is empty");
        return;
    }

    ESP_LOGI(TAG, "Starting OTA from MQTT URL: %s", ota_url_value);
    ota_update_start(ota_url_value);
}

// VAD / Voice Pipeline Callbacks
static void mqtt_vad_threshold_callback(const char *entity_id, const char *payload) {
    (void)entity_id;
    if (!payload) return;
    char *end = NULL;
    float v = strtof(payload, &end);
    if (end != payload) {
        uint32_t val = (uint32_t)v;
        va_control_set_vad_threshold(val);
        mqtt_ha_update_number("vad_threshold", (float)val);
    }
}

static void mqtt_vad_silence_callback(const char *entity_id, const char *payload) {
    (void)entity_id;
    if (!payload) return;
    char *end = NULL;
    float v = strtof(payload, &end);
    if (end != payload) {
        uint32_t val = (uint32_t)v;
        va_control_set_vad_silence_duration_ms(val);
        mqtt_ha_update_number("vad_silence_ms", (float)val);
    }
}

static void mqtt_vad_min_speech_callback(const char *entity_id, const char *payload) {
    (void)entity_id;
    if (!payload) return;
    char *end = NULL;
    float v = strtof(payload, &end);
    if (end != payload) {
        uint32_t val = (uint32_t)v;
        va_control_set_vad_min_speech_ms(val);
        mqtt_ha_update_number("vad_min_speech_ms", (float)val);
    }
}

static void mqtt_vad_max_recording_callback(const char *entity_id, const char *payload) {
    (void)entity_id;
    if (!payload) return;
    char *end = NULL;
    float v = strtof(payload, &end);
    if (end != payload) {
        uint32_t val = (uint32_t)v;
        va_control_set_vad_max_recording_ms(val);
        mqtt_ha_update_number("vad_max_recording_ms", (float)val);
    }
}

static void mqtt_setup_task(void *arg) {
    ESP_LOGI(TAG, "Waiting for MQTT connection...");
    while (!mqtt_ha_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGI(TAG, "Registering HA Entities...");
    
    mqtt_ha_register_switch("wwd_enabled", "Wake Word Detection", mqtt_wwd_switch_callback);
    mqtt_ha_register_switch("auto_gain_control", "Auto Gain Control", mqtt_agc_enabled_callback);
    mqtt_ha_register_switch("led_status_indicator", "LED Status Indicator", mqtt_led_indicator_callback);
    mqtt_ha_register_button("restart", "Restart Device", mqtt_restart_callback);
    mqtt_ha_register_button("test_tts", "Test TTS", mqtt_test_tts_callback);

    mqtt_ha_register_sensor("va_status", "VA Status", NULL, NULL);
    mqtt_ha_register_sensor("va_response", "VA Response", NULL, NULL);
    mqtt_ha_register_sensor("wifi_rssi", "WiFi Signal", "dBm", "signal_strength");
    mqtt_ha_register_sensor("wifi_signal", "WiFi Signal", "dBm", "signal_strength");
    mqtt_ha_register_sensor("ip_address", "IP Address", NULL, NULL);
    mqtt_ha_register_sensor("free_memory", "Free Memory", "bytes", "data_size");
    mqtt_ha_register_sensor("uptime", "Uptime", "s", NULL);
    mqtt_ha_register_sensor("firmware_version", "Firmware Version", NULL, NULL);
    mqtt_ha_register_sensor("network_type", "Network Type", NULL, NULL);
    mqtt_ha_register_sensor("webserial_clients", "WebSerial Clients", NULL, NULL);
    mqtt_ha_register_sensor("agc_current_gain", "AGC Current Gain", NULL, NULL);
    mqtt_ha_register_sensor("music_state", "Music State", NULL, NULL);
    mqtt_ha_register_sensor("current_track", "Current Track", NULL, NULL);
    mqtt_ha_register_sensor("total_tracks", "Total Tracks", NULL, NULL);
    mqtt_ha_register_sensor("sd_card_status", "SD Card Status", NULL, NULL);
    mqtt_ha_register_sensor("ota_status", "OTA Status", NULL, NULL);
    mqtt_ha_register_sensor("ota_progress", "OTA Progress", "%", NULL);
    mqtt_ha_register_sensor("ota_update_url", "OTA Update URL", NULL, NULL);

    mqtt_ha_register_number("led_brightness", "LED Brightness", 0, 100, 1, "%",
                            mqtt_led_brightness_callback);
    mqtt_ha_register_number("output_volume", "Output Volume", 0, 100, 1, "%",
                            mqtt_output_volume_callback);
    mqtt_ha_register_number("agc_target_level", "AGC Target Level", 0, 10000, 50, "",
                            mqtt_agc_target_callback);
    mqtt_ha_register_number("wwd_detection_threshold", "WWD Detection Threshold", 0.5f, 0.95f, 0.01f, "",
                            mqtt_wwd_threshold_callback);

    mqtt_ha_register_text("ota_url_input", "OTA URL", mqtt_ota_url_callback);
    mqtt_ha_register_button("ota_trigger", "Start OTA", mqtt_ota_trigger_callback);

    mqtt_ha_register_button("music_play", "Play Music", mqtt_music_play_callback);
    mqtt_ha_register_button("music_stop", "Stop Music", mqtt_music_stop_callback);
    mqtt_ha_register_button("led_test", "LED Test", mqtt_led_test_callback);

    // VAD Configuration Entities
    mqtt_ha_register_number("vad_threshold", "VAD Threshold", 0, 1000, 10, "", mqtt_vad_threshold_callback);
    mqtt_ha_register_number("vad_silence_ms", "VAD Silence (ms)", 100, 5000, 100, "ms", mqtt_vad_silence_callback);
    mqtt_ha_register_number("vad_min_speech_ms", "VAD Min Speech (ms)", 100, 2000, 50, "ms", mqtt_vad_min_speech_callback);
    mqtt_ha_register_number("vad_max_recording_ms", "VAD Max Rec (ms)", 1000, 15000, 500, "ms", mqtt_vad_max_recording_callback);
    
    // Initial State
    mqtt_ha_update_switch("wwd_enabled", true);
    mqtt_ha_update_switch("auto_gain_control", va_control_get_agc_enabled());
    mqtt_ha_update_switch("led_status_indicator", led_status_is_enabled());

    // Publish current IP once MQTT is up (covers cases where network connected earlier).
    char ip_str[16];
    if (network_manager_get_ip(ip_str) == ESP_OK) {
        mqtt_ha_update_sensor("ip_address", ip_str);
    }

    // Publish initial LED brightness and OTA URL state.
    mqtt_ha_update_number("led_brightness", (float)led_status_get_brightness());
    mqtt_ha_update_number("output_volume", (float)bsp_extra_codec_volume_get());
    mqtt_ha_update_number("agc_target_level", (float)va_control_get_agc_target_level());
    mqtt_ha_update_number("wwd_detection_threshold", va_control_get_wwd_threshold());

    // Publish initial VAD settings
    mqtt_ha_update_number("vad_threshold", (float)va_control_get_vad_threshold());
    mqtt_ha_update_number("vad_silence_ms", (float)va_control_get_vad_silence_duration_ms());
    mqtt_ha_update_number("vad_min_speech_ms", (float)va_control_get_vad_min_speech_ms());
    mqtt_ha_update_number("vad_max_recording_ms", (float)va_control_get_vad_max_recording_ms());

    if (ota_url_value[0] != '\0') {
        mqtt_ha_update_text("ota_url_input", ota_url_value);
        mqtt_ha_update_sensor("ota_update_url", ota_url_value);
    }

    mqtt_publish_telemetry();
    if (metrics_task_handle == NULL) {
        xTaskCreate(mqtt_metrics_task, "mqtt_metrics", 4096, NULL, 4, &metrics_task_handle);
    }
    
    // Report System Status (Crash info)
    sys_diag_report_status();

    vTaskDelete(NULL);
}

static void network_event_callback(network_type_t type, bool connected) {
    if (connected) {
        if (post_connect_task_handle == NULL) {
            xTaskCreate(post_connect_task, "net_post", 4096, (void *)(uintptr_t)type, 5,
                        &post_connect_task_handle);
        }
    }
}

static void music_state_callback(music_state_t state, int current_track, int total_tracks) {
    bool is_playing = (state == MUSIC_STATE_PLAYING || state == MUSIC_STATE_PAUSED);
    voice_pipeline_on_music_state_change(is_playing);
    mqtt_update_music_state(state, current_track, total_tracks);
}

void app_main(void) {
    // 1. NVS Init
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. System Diagnostics (Boot Loop Protection)
    bool safe_mode = (sys_diag_init() != ESP_OK);
    
    if (safe_mode) {
        ESP_LOGE(TAG, "STARTING IN SAFE MODE (Audio disabled)");
        // Init minimal LED
        led_status_init();
        led_status_set(LED_STATUS_ERROR); // Red Blink
    } else {
        ESP_LOGI(TAG, "Starting ESP32-P4 Voice Assistant (Normal Mode)");
        // 3. Hardware Init (Codec, LED)
        ESP_LOGI(TAG, "Initializing Hardware...");
        ESP_ERROR_CHECK(bsp_extra_codec_init());
        bsp_extra_codec_volume_set(60, NULL);
        bsp_extra_player_init();
        audio_hw_ready = true;
        
        led_status_init();
        led_status_set(LED_STATUS_BOOTING);
    }

    // OTA module (available in both normal and safe mode)
    ota_update_init();
    ota_update_register_callback(ota_progress_handler);

    // 4. Watchdog Init (30 seconds timeout)
    sys_diag_wdt_init(30);

    // 5. Load Settings
    app_settings_t settings;
    if (settings_manager_load(&settings) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load settings!");
    }

    // Apply persisted output volume (default 60)
    if (!safe_mode) {
        bsp_extra_codec_volume_set(settings.output_volume, NULL);
    }

    // Load persisted OTA URL
    if (settings.ota_url[0] != '\0') {
        strncpy(ota_url_value, settings.ota_url, sizeof(ota_url_value)-1);
        ota_url_value[sizeof(ota_url_value)-1] = '\0';
        ESP_LOGI(TAG, "Loaded OTA URL from settings: %s", ota_url_value);
    }

    // 6. Network Init
    network_manager_register_callback(network_event_callback);
    network_manager_init();

    mqtt_ha_config_t mqtt_conf = {
        .broker_uri = settings.mqtt_broker_uri,
        .username = settings.mqtt_username,
        .password = settings.mqtt_password,
        .client_id = settings.mqtt_client_id
    };
    mqtt_ha_init(&mqtt_conf);
    mqtt_ha_start();

    // 7. Core Systems (Skip in Safe Mode)
    if (!safe_mode) {
        ha_client_config_t ha_conf = {
            .hostname = settings.ha_hostname,
            .port = settings.ha_port,
            .access_token = settings.ha_token,
            .use_ssl = settings.ha_use_ssl
        };
        ha_client_init(&ha_conf);

        ESP_LOGI(TAG, "Initializing Voice Pipeline...");
        ESP_ERROR_CHECK(voice_pipeline_init());
        
        alarm_manager_init();
        local_music_player_register_callback(music_state_callback);

        ESP_LOGI(TAG, "System Ready. Waiting for Wake Word...");
        led_status_set(LED_STATUS_IDLE);
        voice_pipeline_start();
    } else {
        // Safe Mode Loop
        ESP_LOGW(TAG, "Safe Mode: Use Web/OTA to fix issues.");
    }

    xTaskCreate(mqtt_setup_task, "mqtt_setup", 4096, NULL, 5, NULL);

    // Main Loop - Keep main task alive to feed watchdog
    while (1) {
        sys_diag_wdt_feed();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
