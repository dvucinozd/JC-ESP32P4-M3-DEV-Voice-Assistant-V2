/**
 * @file network_manager.c
 * @brief Network manager implementation - Ethernet priority with WiFi fallback
 */

#include "network_manager.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "network_manager";

// Ethernet hardware configuration (JC-ESP32P4-M3-DEV board)
#define ETH_PHY_ADDR        1
#define ETH_PHY_RST_GPIO    51
#define ETH_MDC_GPIO        31
#define ETH_MDIO_GPIO       52

// Network state
static network_type_t active_network = NETWORK_TYPE_NONE;
static bool ethernet_available = false;
static bool wifi_fallback_active = false;
static network_event_callback_t event_callback = NULL;

// Ethernet driver handles
static esp_eth_handle_t eth_handle = NULL;
static esp_netif_t *eth_netif = NULL;

// Forward declarations
static esp_err_t ethernet_init(void);
static void ethernet_event_handler(void *arg, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data);
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data);

/**
 * @brief Initialize Ethernet driver
 */
static esp_err_t ethernet_init(void)
{
    ESP_LOGI(TAG, "Initializing Ethernet (RMII PHY IP101)...");

    // Configure PHY reset GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << ETH_PHY_RST_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // Reset PHY chip
    gpio_set_level(ETH_PHY_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(ETH_PHY_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "PHY reset complete (GPIO %d)", ETH_PHY_RST_GPIO);

    // Create MAC configuration (common)
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();

    // Create ESP32 EMAC specific configuration
    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp32_emac_config.smi_gpio.mdc_num = ETH_MDC_GPIO;
    esp32_emac_config.smi_gpio.mdio_num = ETH_MDIO_GPIO;

    // Create PHY configuration
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ETH_PHY_ADDR;
    phy_config.reset_gpio_num = ETH_PHY_RST_GPIO;

    // Create MAC instance (ESP32-P4 internal EMAC)
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
    if (mac == NULL) {
        ESP_LOGE(TAG, "Failed to create MAC instance");
        return ESP_FAIL;
    }

    // Create PHY instance (IP101)
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
    if (phy == NULL) {
        ESP_LOGE(TAG, "Failed to create PHY instance");
        return ESP_FAIL;
    }

    // Install Ethernet driver
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t ret = esp_eth_driver_install(&eth_config, &eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Ethernet driver installed");

    // Create network interface for Ethernet
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    eth_netif = esp_netif_new(&netif_config);
    if (eth_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create Ethernet netif");
        return ESP_FAIL;
    }

    // Attach Ethernet driver to TCP/IP stack
    void *glue = esp_eth_new_netif_glue(eth_handle);
    ret = esp_netif_attach(eth_netif, glue);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to attach Ethernet to netif: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register event handlers
    ret = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                      ethernet_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register Ethernet event handler");
        return ret;
    }

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                      ip_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler");
        return ret;
    }

    // Start Ethernet driver
    ret = esp_eth_start(eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Ethernet: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "✅ Ethernet initialized - waiting for link...");
    ethernet_available = true;

    return ESP_OK;
}

/**
 * @brief Ethernet event handler
 */
static void ethernet_event_handler(void *arg, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data)
{
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "🔌 Ethernet cable connected");

        // If WiFi fallback was active, stop it
        if (wifi_fallback_active && wifi_manager_is_active()) {
            ESP_LOGI(TAG, "Stopping WiFi fallback - switching to Ethernet");
            wifi_manager_stop();
            wifi_fallback_active = false;
        }
        break;

    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "🔌 Ethernet cable disconnected");

        // Mark Ethernet as inactive
        if (active_network == NETWORK_TYPE_ETHERNET) {
            active_network = NETWORK_TYPE_NONE;

            // Notify application
            if (event_callback) {
                event_callback(NETWORK_TYPE_ETHERNET, false);
            }
        }

        // Activate WiFi fallback
        if (!wifi_fallback_active) {
            ESP_LOGI(TAG, "⚠️  Activating WiFi fallback...");
            wifi_fallback_active = true;
            wifi_manager_init();
        }
        break;

    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started");
        break;

    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet stopped");
        break;

    default:
        break;
    }
}

/**
 * @brief IP event handler (for both Ethernet and WiFi)
 */
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "✅ Ethernet IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "   Gateway: " IPSTR, IP2STR(&event->ip_info.gw));
        ESP_LOGI(TAG, "   Netmask: " IPSTR, IP2STR(&event->ip_info.netmask));

        // Set Ethernet as active network
        active_network = NETWORK_TYPE_ETHERNET;

        // Notify application
        if (event_callback) {
            event_callback(NETWORK_TYPE_ETHERNET, true);
        }

    } else if (event_id == IP_EVENT_STA_GOT_IP) {
        // WiFi got IP - only use if Ethernet is not active
        if (active_network != NETWORK_TYPE_ETHERNET) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "✅ WiFi IP (fallback): " IPSTR, IP2STR(&event->ip_info.ip));

            active_network = NETWORK_TYPE_WIFI;

            // Notify application
            if (event_callback) {
                event_callback(NETWORK_TYPE_WIFI, true);
            }
        } else {
            ESP_LOGI(TAG, "WiFi IP acquired but Ethernet is active - ignoring");
        }
    }
}

/**
 * @brief Initialize network manager
 */
esp_err_t network_manager_init(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Network Manager Initialization");
    ESP_LOGI(TAG, "Priority: Ethernet → WiFi fallback");
    ESP_LOGI(TAG, "========================================");

    // Register WiFi IP event handler
    esp_err_t ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                ip_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WiFi IP event handler");
    }

    // Try to initialize Ethernet first (priority)
    ret = ethernet_init();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Ethernet initialization successful");
        // Wait for Ethernet link (timeout 5 seconds)
        ESP_LOGI(TAG, "Waiting for Ethernet link... (5s timeout)");
        vTaskDelay(pdMS_TO_TICKS(5000));

        // Check if we got IP via Ethernet
        if (active_network == NETWORK_TYPE_ETHERNET) {
            ESP_LOGI(TAG, "✅ Ethernet active - skipping WiFi");
            return ESP_OK;
        } else {
            ESP_LOGW(TAG, "Ethernet initialized but no link detected");
        }
    } else {
        ESP_LOGW(TAG, "Ethernet initialization failed: %s", esp_err_to_name(ret));
        ethernet_available = false;
    }

    // Ethernet not available or no link - fall back to WiFi
    ESP_LOGI(TAG, "Starting WiFi fallback...");
    wifi_fallback_active = true;
    ret = wifi_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi fallback initialization failed!");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief Get active network type
 */
network_type_t network_manager_get_active_type(void)
{
    return active_network;
}

/**
 * @brief Check if network is connected
 */
bool network_manager_is_connected(void)
{
    return (active_network != NETWORK_TYPE_NONE);
}

/**
 * @brief Get IP address of active network
 */
esp_err_t network_manager_get_ip(char *ip_str)
{
    if (!ip_str) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_t *netif = NULL;

    if (active_network == NETWORK_TYPE_ETHERNET) {
        netif = eth_netif;
    } else if (active_network == NETWORK_TYPE_WIFI) {
        netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    }

    if (netif == NULL) {
        strcpy(ip_str, "0.0.0.0");
        return ESP_FAIL;
    }

    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(netif, &ip_info);
    if (ret == ESP_OK) {
        sprintf(ip_str, IPSTR, IP2STR(&ip_info.ip));
        return ESP_OK;
    }

    strcpy(ip_str, "0.0.0.0");
    return ESP_FAIL;
}

/**
 * @brief Register network event callback
 */
void network_manager_register_callback(network_event_callback_t callback)
{
    event_callback = callback;
    ESP_LOGI(TAG, "Network event callback registered");
}

/**
 * @brief Force WiFi fallback (for testing)
 */
esp_err_t network_manager_force_wifi_fallback(void)
{
    ESP_LOGI(TAG, "Manual WiFi fallback triggered");

    if (eth_handle && active_network == NETWORK_TYPE_ETHERNET) {
        esp_eth_stop(eth_handle);
        active_network = NETWORK_TYPE_NONE;
    }

    if (!wifi_fallback_active) {
        wifi_fallback_active = true;
        return wifi_manager_init();
    }

    return ESP_OK;
}

/**
 * @brief Convert network type to string
 */
const char* network_manager_type_to_string(network_type_t type)
{
    switch (type) {
        case NETWORK_TYPE_ETHERNET:
            return "ethernet";
        case NETWORK_TYPE_WIFI:
            return "wifi";
        case NETWORK_TYPE_NONE:
        default:
            return "none";
    }
}
