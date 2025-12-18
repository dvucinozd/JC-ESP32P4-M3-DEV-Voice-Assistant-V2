/**
 * @file led_status.h
 * @brief RGB LED Status Indicator Module
 *
 * Controls HW-478 RGB LED for visual status feedback.
 * Uses LEDC PWM for smooth color transitions and effects.
 */

#ifndef LED_STATUS_H
#define LED_STATUS_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LED GPIO Configuration
 */
#define LED_GPIO_RED 45
#define LED_GPIO_GREEN 46
#define LED_GPIO_BLUE 47

/**
 * @brief Set to 1 for active-low LED modules (common-anode / sinking driver).
 * For common-cathode (common to GND), keep 0.
 */
#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 0
#endif

/**
 * @brief LED Status States
 */
typedef enum {
  LED_STATUS_OFF = 0,    ///< LED off
  LED_STATUS_BOOTING,    ///< Yellow (static) - System booting
  LED_STATUS_IDLE,       ///< Green (dim) - Ready for wake word
  LED_STATUS_LISTENING,  ///< Blue (pulsing) - Wake word detected, listening
  LED_STATUS_PROCESSING, ///< Yellow (blinking) - Processing STT/Intent
  LED_STATUS_SPEAKING,   ///< Cyan (static) - TTS playback
  LED_STATUS_ERROR,      ///< Red (fast blinking) - Error state
  LED_STATUS_CONNECTING, ///< Purple (slow pulse) - Connecting to network
  LED_STATUS_OTA,        ///< White (breathing) - OTA update in progress
} led_status_t;

/**
 * @brief Initialize LED status module
 *
 * Configures LEDC PWM channels for R, G, B LEDs.
 *
 * @return ESP_OK on success
 */
esp_err_t led_status_init(void);

/**
 * @brief Set LED status state
 *
 * Changes LED color and effect based on status.
 *
 * @param status New status to display
 */
void led_status_set(led_status_t status);

/**
 * @brief Get current LED status
 *
 * @return Current LED status
 */
led_status_t led_status_get(void);

/**
 * @brief Set LED brightness (0-100%)
 *
 * @param brightness Brightness percentage (0-100)
 */
void led_status_set_brightness(uint8_t brightness);

/**
 * @brief Get current LED brightness
 *
 * @return Brightness percentage (0-100)
 */
uint8_t led_status_get_brightness(void);

/**
 * @brief Enable or disable LED
 *
 * @param enable true to enable, false to disable
 */
void led_status_enable(bool enable);

/**
 * @brief Check if LED is enabled
 *
 * @return true if enabled
 */
bool led_status_is_enabled(void);

/**
 * @brief Set custom RGB color (0-255 each)
 *
 * @param r Red component
 * @param g Green component
 * @param b Blue component
 */
void led_status_set_rgb(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Run a short RGB test pattern
 *
 * Useful for diagnosing wiring/pin issues. Runs asynchronously.
 */
void led_status_test_pattern(void);

/**
 * @brief Deinitialize LED status module
 */
void led_status_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // LED_STATUS_H
