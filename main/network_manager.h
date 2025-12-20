/**
 * @file network_manager.h
 * @brief Unified network manager for Ethernet (priority) + WiFi (fallback)
 *
 * Network priority:
 * 1. Ethernet (RMII PHY) - Primary network interface
 * 2. WiFi - Fallback when Ethernet unavailable
 *
 * Features:
 * - Automatic network selection on boot
 * - Runtime failover on Ethernet disconnect/reconnect
 * - Event callbacks for network state changes
 * - Unified API for application layer
 */

#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include "esp_err.h"
#include "esp_netif.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Network interface types
 */
typedef enum {
    NETWORK_TYPE_NONE = 0,      ///< No network connected
    NETWORK_TYPE_ETHERNET,       ///< Ethernet connected (priority)
    NETWORK_TYPE_WIFI            ///< WiFi connected (fallback)
} network_type_t;

/**
 * @brief Network event callback function type
 *
 * Called when network state changes (connected/disconnected)
 *
 * @param type Network type that changed state
 * @param connected true if connected, false if disconnected
 */
typedef void (*network_event_callback_t)(network_type_t type, bool connected);

/**
 * @brief Initialize network manager
 *
 * Attempts to initialize Ethernet first (priority).
 * If Ethernet fails, falls back to WiFi.
 * Registers event handlers for automatic failover.
 *
 * @return ESP_OK on success
 */
esp_err_t network_manager_init(void);

/**
 * @brief Get currently active network type
 *
 * @return NETWORK_TYPE_ETHERNET if Ethernet active
 *         NETWORK_TYPE_WIFI if WiFi active
 *         NETWORK_TYPE_NONE if no network connected
 */
network_type_t network_manager_get_active_type(void);

/**
 * @brief Check if any network is connected
 *
 * @return true if Ethernet OR WiFi connected
 *         false if no network available
 */
bool network_manager_is_connected(void);

/**
 * @brief Get IP address of active network interface
 *
 * @param ip_str Buffer to store IP address string (min 16 bytes)
 * @return ESP_OK if IP retrieved, ESP_FAIL if no network
 */
esp_err_t network_manager_get_ip(char *ip_str);

/**
 * @brief Get IP info (ip/gw/netmask) for active interface
 *
 * @param info Output ip info
 * @return ESP_OK if retrieved, ESP_FAIL if no network
 */
esp_err_t network_manager_get_ip_info(esp_netif_ip_info_t *info);

/**
 * @brief Get DNS info for active interface (primary DNS)
 *
 * @param info Output DNS info
 * @return ESP_OK if retrieved, ESP_FAIL if no network
 */
esp_err_t network_manager_get_dns_info(esp_netif_dns_info_t *info);

/**
 * @brief Register callback for network events
 *
 * Application can register a callback to be notified when:
 * - Ethernet connects/disconnects
 * - WiFi connects/disconnects
 * - Network type changes (failover)
 *
 * @param callback Function to call on network events
 */
void network_manager_register_callback(network_event_callback_t callback);

/**
 * @brief Manually trigger WiFi fallback (for testing)
 *
 * @return ESP_OK on success
 */
esp_err_t network_manager_force_wifi_fallback(void);

/**
 * @brief Get network type as string
 *
 * @param type Network type
 * @return String representation ("ethernet", "wifi", "none")
 */
const char* network_manager_type_to_string(network_type_t type);

#ifdef __cplusplus
}
#endif

#endif // NETWORK_MANAGER_H
