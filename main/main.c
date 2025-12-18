#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
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
#include "webserial.h"
#include "local_music_player.h"
#include "alarm_manager.h" 
#include "sys_diag.h" // Phase 9

#define TAG "main"

static bool sd_init_done = false;
static TaskHandle_t post_connect_task_handle = NULL;
static char ota_url_value[256] = {0};
static TaskHandle_t music_control_task_handle = NULL;
static bool audio_hw_ready = false;

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

static void mqtt_ota_url_callback(const char *entity_id, const char *payload) {
    (void)entity_id;
    if (!payload) return;

    strncpy(ota_url_value, payload, sizeof(ota_url_value) - 1);
    ota_url_value[sizeof(ota_url_value) - 1] = '\0';

    ESP_LOGI(TAG, "OTA URL set via MQTT: %s", ota_url_value);
    (void)mqtt_ha_update_text("ota_url_input", ota_url_value);
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

static void mqtt_setup_task(void *arg) {
    ESP_LOGI(TAG, "Waiting for MQTT connection...");
    while (!mqtt_ha_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGI(TAG, "Registering HA Entities...");
    
    mqtt_ha_register_switch("wwd_enabled", "Wake Word Detection", mqtt_wwd_switch_callback);
    mqtt_ha_register_button("restart", "Restart Device", mqtt_restart_callback);
    mqtt_ha_register_button("test_tts", "Test TTS", mqtt_test_tts_callback);

    mqtt_ha_register_sensor("va_status", "VA Status", NULL, NULL);
    mqtt_ha_register_sensor("va_response", "VA Response", NULL, NULL);
    mqtt_ha_register_sensor("wifi_rssi", "WiFi Signal", "dBm", "signal_strength");
    mqtt_ha_register_sensor("ip_address", "IP Address", NULL, NULL);

    mqtt_ha_register_number("led_brightness", "LED Brightness", 0, 100, 1, "%",
                            mqtt_led_brightness_callback);
    mqtt_ha_register_number("output_volume", "Output Volume", 0, 100, 1, "%",
                            mqtt_output_volume_callback);

    mqtt_ha_register_text("ota_url_input", "OTA URL", mqtt_ota_url_callback);
    mqtt_ha_register_button("ota_trigger", "Start OTA", mqtt_ota_trigger_callback);

    mqtt_ha_register_button("music_play", "Play Music", mqtt_music_play_callback);
    mqtt_ha_register_button("music_stop", "Stop Music", mqtt_music_stop_callback);
    mqtt_ha_register_button("led_test", "LED Test", mqtt_led_test_callback);
    
    // Initial State
    mqtt_ha_update_switch("wwd_enabled", true);

    // Publish current IP once MQTT is up (covers cases where network connected earlier).
    char ip_str[16];
    if (network_manager_get_ip(ip_str) == ESP_OK) {
        mqtt_ha_update_sensor("ip_address", ip_str);
    }

    // Publish initial LED brightness and OTA URL state.
    mqtt_ha_update_number("led_brightness", (float)led_status_get_brightness());
    mqtt_ha_update_number("output_volume", (float)bsp_extra_codec_volume_get());
    if (ota_url_value[0] != '\0') {
        mqtt_ha_update_text("ota_url_input", ota_url_value);
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
