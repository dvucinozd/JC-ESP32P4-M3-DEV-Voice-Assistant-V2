#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char wifi_ssid[32];
    char wifi_password[64];
    
    char ha_hostname[64];
    int ha_port;
    char ha_token[512]; // Increased size for JWT
    bool ha_use_ssl;

    char mqtt_broker_uri[64];
    char mqtt_username[32];
    char mqtt_password[64];
    char mqtt_client_id[32];

    int output_volume; // 0-100
} app_settings_t;

// Initialize settings manager (mounts NVS)
esp_err_t settings_manager_init(void);

// Load settings from NVS. If not found, loads defaults from config.h
esp_err_t settings_manager_load(app_settings_t *settings);

// Save settings to NVS
esp_err_t settings_manager_save(const app_settings_t *settings);

// Reset to defaults (from config.h)
esp_err_t settings_manager_reset_defaults(void);

#ifdef __cplusplus
}
#endif
