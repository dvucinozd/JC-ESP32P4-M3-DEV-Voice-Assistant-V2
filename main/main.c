#include <stdio.h>
#include <string.h>
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
    if (local_music_player_is_initialized()) local_music_player_play();
}

static void mqtt_music_stop_callback(const char *entity_id, const char *payload) {
    if (local_music_player_is_initialized()) local_music_player_stop();
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

    mqtt_ha_register_button("music_play", "Play Music", mqtt_music_play_callback);
    mqtt_ha_register_button("music_stop", "Stop Music", mqtt_music_stop_callback);
    
    // Initial State
    mqtt_ha_update_switch("wwd_enabled", true);
    
    // Report System Status (Crash info)
    sys_diag_report_status();

    vTaskDelete(NULL);
}

static void network_event_callback(network_type_t type, bool connected) {
    if (connected) {
        char ip_str[16];
        network_manager_get_ip(ip_str);
        ESP_LOGI(TAG, "Network Connected: %s (IP: %s)", network_manager_type_to_string(type), ip_str);
        
        if (mqtt_ha_is_connected()) {
             mqtt_ha_update_sensor("ip_address", ip_str);
        }

        webserial_init();

        // Only mount SD if NOT in Safe Mode (keep minimal)
        if (!sys_diag_is_safe_mode()) {
            if (bsp_sdcard_mount() == ESP_OK) {
                ESP_LOGI(TAG, "SD Card mounted");
                local_music_player_init();
                local_music_player_register_callback(NULL); // Or proper callback
            }
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
        
        led_status_init();
        led_status_set(LED_STATUS_BOOTING);
    }

    // 4. Watchdog Init (30 seconds timeout)
    sys_diag_wdt_init(30);

    // 5. Load Settings
    app_settings_t settings;
    if (settings_manager_load(&settings) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load settings!");
    }

    // 6. Network Init
    network_manager_init();
    network_manager_register_callback(network_event_callback);
    
    wifi_manager_init(settings.wifi_ssid, settings.wifi_password);
    
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