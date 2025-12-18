/**
 * WiFi Manager Header
 * ESP32-P4 Voice Assistant
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WiFi in station mode and connect to configured AP
 *
 * This function initializes the WiFi subsystem using ESP32-C6 coprocessor
 * via SDIO interface (esp_wifi_remote). It will block until connection
 * is established or max retries reached.
 *
 * @param ssid WiFi SSID
 * @param password WiFi Password
 * 
 * @return
 *    - ESP_OK: WiFi connected successfully
 *    - ESP_FAIL: Failed to connect after max retries
 *    - ESP_ERR_TIMEOUT: Unexpected event
 */
esp_err_t wifi_init_sta(const char* ssid, const char* password);

/**
 * @brief Check if WiFi is currently connected
 *
 * @return true if connected to AP and has IP address, false otherwise
 */
bool wifi_is_connected(void);

/**
 * @brief Wrapper for wifi_init_sta (for backward compatibility if needed, but better to use direct call)
 */
static inline esp_err_t wifi_manager_init(const char* ssid, const char* password) {
    return wifi_init_sta(ssid, password);
}

/**
 * @brief Check if WiFi is active (for network manager)
 *
 * @return true if WiFi is active
 */
static inline bool wifi_manager_is_active(void) {
    return wifi_is_connected();
}

/**
 * @brief Stop WiFi (for Ethernet failover)
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_H */