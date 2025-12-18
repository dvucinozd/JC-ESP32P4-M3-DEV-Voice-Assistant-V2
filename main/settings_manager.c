#include "settings_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "string.h"
#include "config.h" // Fallback defaults

#define TAG "settings"
#define NVS_NAMESPACE "sys_config"

esp_err_t settings_manager_init(void) {
    // NVS init is usually done in main, but we can double check here
    return ESP_OK;
}

static esp_err_t nvs_get_str_safe(nvs_handle_t handle, const char* key, char* out_buffer, size_t max_len, const char* default_val) {
    size_t required_size = 0;
    esp_err_t err = nvs_get_str(handle, key, NULL, &required_size);
    
    if (err == ESP_OK && required_size <= max_len) {
        return nvs_get_str(handle, key, out_buffer, &max_len);
    }
    
    // If not found or too big, use default
    if (default_val) {
        strncpy(out_buffer, default_val, max_len - 1);
        out_buffer[max_len - 1] = '\0';
    } else {
        out_buffer[0] = '\0';
    }
    
    return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
}

esp_err_t settings_manager_load(app_settings_t *settings) {
    if (!settings) return ESP_ERR_INVALID_ARG;
    
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle);
    
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS config not found, loading defaults from config.h");
        settings_manager_reset_defaults();
        // Recurse once? No, just load defaults manually to struct
        strncpy(settings->wifi_ssid, WIFI_SSID, sizeof(settings->wifi_ssid)-1);
        strncpy(settings->wifi_password, WIFI_PASSWORD, sizeof(settings->wifi_password)-1);
        strncpy(settings->ha_hostname, HA_HOSTNAME, sizeof(settings->ha_hostname)-1);
        settings->ha_port = HA_PORT;
        strncpy(settings->ha_token, HA_TOKEN, sizeof(settings->ha_token)-1);
        settings->ha_use_ssl = HA_USE_SSL;
        strncpy(settings->mqtt_broker_uri, MQTT_BROKER_URI, sizeof(settings->mqtt_broker_uri)-1);
        if (MQTT_USERNAME) strncpy(settings->mqtt_username, MQTT_USERNAME, sizeof(settings->mqtt_username)-1);
        if (MQTT_PASSWORD) strncpy(settings->mqtt_password, MQTT_PASSWORD, sizeof(settings->mqtt_password)-1);
        strncpy(settings->mqtt_client_id, MQTT_CLIENT_ID, sizeof(settings->mqtt_client_id)-1);
        return ESP_OK;
    }

    // Load from NVS
    nvs_get_str_safe(my_handle, "wifi_ssid", settings->wifi_ssid, sizeof(settings->wifi_ssid), WIFI_SSID);
    nvs_get_str_safe(my_handle, "wifi_pass", settings->wifi_password, sizeof(settings->wifi_password), WIFI_PASSWORD);
    
    nvs_get_str_safe(my_handle, "ha_host", settings->ha_hostname, sizeof(settings->ha_hostname), HA_HOSTNAME);
    nvs_get_str_safe(my_handle, "ha_token", settings->ha_token, sizeof(settings->ha_token), HA_TOKEN);
    
    int32_t port = HA_PORT;
    nvs_get_i32(my_handle, "ha_port", &port);
    settings->ha_port = port;

    uint8_t ssl = HA_USE_SSL;
    nvs_get_u8(my_handle, "ha_ssl", &ssl);
    settings->ha_use_ssl = (bool)ssl;

    nvs_get_str_safe(my_handle, "mqtt_uri", settings->mqtt_broker_uri, sizeof(settings->mqtt_broker_uri), MQTT_BROKER_URI);
    nvs_get_str_safe(my_handle, "mqtt_user", settings->mqtt_username, sizeof(settings->mqtt_username), MQTT_USERNAME);
    nvs_get_str_safe(my_handle, "mqtt_pass", settings->mqtt_password, sizeof(settings->mqtt_password), MQTT_PASSWORD);
    nvs_get_str_safe(my_handle, "mqtt_id", settings->mqtt_client_id, sizeof(settings->mqtt_client_id), MQTT_CLIENT_ID);

    nvs_close(my_handle);
    return ESP_OK;
}

esp_err_t settings_manager_save(const app_settings_t *settings) {
    if (!settings) return ESP_ERR_INVALID_ARG;

    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;

    nvs_set_str(my_handle, "wifi_ssid", settings->wifi_ssid);
    nvs_set_str(my_handle, "wifi_pass", settings->wifi_password);
    
    nvs_set_str(my_handle, "ha_host", settings->ha_hostname);
    nvs_set_str(my_handle, "ha_token", settings->ha_token);
    nvs_set_i32(my_handle, "ha_port", settings->ha_port);
    nvs_set_u8(my_handle, "ha_ssl", (uint8_t)settings->ha_use_ssl);

    nvs_set_str(my_handle, "mqtt_uri", settings->mqtt_broker_uri);
    nvs_set_str(my_handle, "mqtt_user", settings->mqtt_username);
    nvs_set_str(my_handle, "mqtt_pass", settings->mqtt_password);
    nvs_set_str(my_handle, "mqtt_id", settings->mqtt_client_id);

    err = nvs_commit(my_handle);
    nvs_close(my_handle);
    return err;
}

esp_err_t settings_manager_reset_defaults(void) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        nvs_erase_all(my_handle);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
    return ESP_OK;
}
