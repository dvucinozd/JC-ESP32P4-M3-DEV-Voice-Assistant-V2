/**
 * WiFi Manager for ESP32-P4 Voice Assistant
 * Uses ESP32-C6 coprocessor via SDIO (esp_wifi_remote)
 */

#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "config.h"

static const char *TAG = "wifi_manager";

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
static bool s_wifi_initialized = false;
static bool s_handlers_registered = false;
static esp_event_handler_instance_t s_instance_any_id;
static esp_event_handler_instance_t s_instance_got_ip;

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry to connect to the AP (attempt %d/%d)", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"Connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_init_sta(const char* ssid, const char* password)
{
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        if (s_wifi_event_group == NULL) {
            ESP_LOGE(TAG, "Failed to create WiFi event group");
            return ESP_ERR_NO_MEM;
        }
    }
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;

    // Initialize netif and event loop (skip if already done by network_manager)
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to init netif");
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to create event loop");
        return ret;
    }

    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        esp_netif_create_default_wifi_sta();
    }

    ESP_LOGI(TAG, "Initializing WiFi via ESP32-C6 (SDIO)...");

    if (!s_wifi_initialized) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ret = esp_wifi_init(&cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_wifi_initialized = true;
    }

    if (!s_handlers_registered) {
        ret = esp_event_handler_instance_register(WIFI_EVENT,
                                                  ESP_EVENT_ANY_ID,
                                                  &event_handler,
                                                  NULL,
                                                  &s_instance_any_id);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register WIFI_EVENT handler: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = esp_event_handler_instance_register(IP_EVENT,
                                                  IP_EVENT_STA_GOT_IP,
                                                  &event_handler,
                                                  NULL,
                                                  &s_instance_got_ip);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register IP_EVENT handler: %s", esp_err_to_name(ret));
            return ret;
        }
        s_handlers_registered = true;
    }

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };

    if (ssid) {
        strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
        wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';
    }
    if (password) {
        strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    (void)esp_wifi_connect();

    ESP_LOGI(TAG, "WiFi init finished. Waiting for connection to '%s'...", ssid ? ssid : "NULL");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP SSID:%s", ssid ? ssid : "NULL");
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to SSID:%s", ssid ? ssid : "NULL");
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "Unexpected WiFi event");
        return ESP_ERR_TIMEOUT;
    }
}

bool wifi_is_connected(void)
{
    if (s_wifi_event_group == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t wifi_manager_stop(void)
{
    ESP_LOGI(TAG, "Stopping WiFi (switching to Ethernet priority)...");

    if (s_wifi_event_group != NULL) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }

    esp_err_t ret = esp_wifi_disconnect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "WiFi disconnect failed: %s", esp_err_to_name(ret));
    }

    ret = esp_wifi_stop();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "WiFi stop failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "WiFi stopped successfully");
    }

    return ESP_OK;
}
