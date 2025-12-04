/**
 * @file webserial.h
 * @brief WebSerial - Remote serial console over HTTP/WebSocket
 *
 * Provides web-based serial console for remote monitoring and debugging.
 * Accessible via http://<device-ip>/webserial
 */

#ifndef WEBSERIAL_H
#define WEBSERIAL_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WebSerial server
 *
 * Starts HTTP server with WebSocket endpoint for serial console access.
 * Redirects ESP_LOG output to both UART and WebSocket clients.
 *
 * @return ESP_OK on success
 */
esp_err_t webserial_init(void);

/**
 * @brief Deinitialize WebSerial server
 *
 * Stops HTTP server and restores normal logging.
 *
 * @return ESP_OK on success
 */
esp_err_t webserial_deinit(void);

/**
 * @brief Check if WebSerial is running
 *
 * @return true if server is running
 */
bool webserial_is_running(void);

/**
 * @brief Send message to all connected WebSerial clients
 *
 * @param message Message to send
 * @param length Message length
 * @return ESP_OK on success
 */
esp_err_t webserial_broadcast(const char *message, size_t length);

/**
 * @brief Get number of connected WebSerial clients
 *
 * @return Number of active WebSocket connections
 */
int webserial_get_client_count(void);

#ifdef __cplusplus
}
#endif

#endif // WEBSERIAL_H
