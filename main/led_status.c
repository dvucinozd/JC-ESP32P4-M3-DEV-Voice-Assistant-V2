/**
 * @file led_status.c
 * @brief RGB LED Status Indicator Implementation
 *
 * Uses LEDC PWM for smooth color control and effects.
 */

#include "led_status.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "led_status";

// LEDC Configuration
#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_DUTY_RES LEDC_TIMER_8_BIT
#define LEDC_FREQUENCY 5000

#define LEDC_CHANNEL_RED LEDC_CHANNEL_0
#define LEDC_CHANNEL_GREEN LEDC_CHANNEL_1
#define LEDC_CHANNEL_BLUE LEDC_CHANNEL_2

// Effect timing (ms)
#define PULSE_PERIOD_MS 1000
#define FAST_PULSE_MS 300 // Fast pulsing for SPEAKING and OTA
#define BLINK_PERIOD_MS 500
#define FAST_BLINK_MS 200
#define EFFECT_STEP_MS 20

// Module state
static bool led_initialized = false;
static bool led_enabled = true;
static led_status_t current_status = LED_STATUS_OFF;
static uint8_t brightness = 100; // 100% default
static TaskHandle_t effect_task_handle = NULL;
static volatile bool effect_running = false;

// Current RGB values (0-255)
static uint8_t current_r = 0;
static uint8_t current_g = 0;
static uint8_t current_b = 0;

static TaskHandle_t test_task_handle = NULL;

/**
 * @brief Apply RGB values to LEDs with brightness scaling
 */
static void apply_rgb(uint8_t r, uint8_t g, uint8_t b) {
  if (!led_initialized) {
    return;
  }

  // Allow turning off LED even when led_enabled is false
  if (!led_enabled && (r != 0 || g != 0 || b != 0)) {
    return;
  }

  // Scale by brightness
  uint32_t scaled_r = (r * brightness) / 100;
  uint32_t scaled_g = (g * brightness) / 100;
  uint32_t scaled_b = (b * brightness) / 100;

#if LED_ACTIVE_LOW
  // Invert for Active Low (common-anode / sinking driver) LEDs
  scaled_r = 255 - scaled_r;
  scaled_g = 255 - scaled_g;
  scaled_b = 255 - scaled_b;
#endif

  ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_RED, scaled_r);
  ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_RED);

  ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_GREEN, scaled_g);
  ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_GREEN);

  ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_BLUE, scaled_b);
  ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_BLUE);

  current_r = r;
  current_g = g;
  current_b = b;
}

/**
 * @brief Turn off all LEDs
 */
static void led_off(void) { apply_rgb(0, 0, 0); }

/**
 * @brief Effect task for pulsing, blinking effects
 */
static void led_effect_task(void *arg) {
  uint32_t tick = 0;

  while (effect_running) {
    led_status_t status = current_status;

    switch (status) {
    case LED_STATUS_LISTENING: {
      // Blue pulsing
      float phase = (float)(tick % PULSE_PERIOD_MS) / PULSE_PERIOD_MS;
      float intensity =
          0.3f + 0.7f * (0.5f + 0.5f * sinf(phase * 2 * 3.14159f));
      apply_rgb(0, 0, (uint8_t)(255 * intensity));
      break;
    }

    case LED_STATUS_PROCESSING: {
      // Yellow blinking
      bool on = ((tick / BLINK_PERIOD_MS) % 2) == 0;
      if (on) {
        apply_rgb(255, 180, 0); // Yellow
      } else {
        apply_rgb(50, 35, 0); // Dim yellow
      }
      break;
    }

    case LED_STATUS_ERROR: {
      // Red fast blinking
      bool on = ((tick / FAST_BLINK_MS) % 2) == 0;
      if (on) {
        apply_rgb(255, 0, 0);
      } else {
        led_off();
      }
      break;
    }

    case LED_STATUS_CONNECTING: {
      // Purple slow pulse
      float phase =
          (float)(tick % (PULSE_PERIOD_MS * 2)) / (PULSE_PERIOD_MS * 2);
      float intensity =
          0.2f + 0.8f * (0.5f + 0.5f * sinf(phase * 2 * 3.14159f));
      apply_rgb((uint8_t)(180 * intensity), 0, (uint8_t)(255 * intensity));
      break;
    }

    case LED_STATUS_OTA: {
      // White fast pulsing during OTA update
      float phase = (float)(tick % FAST_PULSE_MS) / FAST_PULSE_MS;
      float intensity =
          0.2f + 0.8f * (0.5f + 0.5f * sinf(phase * 2 * 3.14159f));
      uint8_t val = (uint8_t)(255 * intensity);
      apply_rgb(val, val, val);
      break;
    }

    case LED_STATUS_SPEAKING: {
      // Cyan fast pulsing during TTS playback
      float phase = (float)(tick % FAST_PULSE_MS) / FAST_PULSE_MS;
      float intensity =
          0.3f + 0.7f * (0.5f + 0.5f * sinf(phase * 2 * 3.14159f));
      apply_rgb(0, (uint8_t)(255 * intensity), (uint8_t)(255 * intensity));
      break;
    }

    default:
      // Static colors handled elsewhere
      break;
    }

    tick += EFFECT_STEP_MS;
    vTaskDelay(pdMS_TO_TICKS(EFFECT_STEP_MS));
  }

  vTaskDelete(NULL);
}

/**
 * @brief Start effect task if needed
 */
static void start_effect_task(void) {
  if (effect_task_handle == NULL) {
    effect_running = true;
    xTaskCreate(led_effect_task, "led_effect", 4096, NULL, 3,
                &effect_task_handle);
  }
}

/**
 * @brief Stop effect task
 */
static void stop_effect_task(void) {
  if (effect_task_handle != NULL) {
    effect_running = false;
    vTaskDelay(pdMS_TO_TICKS(EFFECT_STEP_MS * 2));
    effect_task_handle = NULL;
  }
}

/**
 * @brief Check if status needs effect task
 */
static bool status_needs_effect(led_status_t status) {
  return (status == LED_STATUS_LISTENING || status == LED_STATUS_PROCESSING ||
          status == LED_STATUS_ERROR || status == LED_STATUS_CONNECTING ||
          status == LED_STATUS_OTA || status == LED_STATUS_SPEAKING);
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t led_status_init(void) {
  if (led_initialized) {
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Initializing LED status (R=%d, G=%d, B=%d)", LED_GPIO_RED,
           LED_GPIO_GREEN, LED_GPIO_BLUE);
  ESP_LOGI(TAG, "LED_ACTIVE_LOW=%d", LED_ACTIVE_LOW);

  (void)gpio_set_drive_capability(LED_GPIO_RED, GPIO_DRIVE_CAP_3);
  (void)gpio_set_drive_capability(LED_GPIO_GREEN, GPIO_DRIVE_CAP_3);
  (void)gpio_set_drive_capability(LED_GPIO_BLUE, GPIO_DRIVE_CAP_3);

  // Configure LEDC timer
  ledc_timer_config_t timer_conf = {
      .speed_mode = LEDC_MODE,
      .timer_num = LEDC_TIMER,
      .duty_resolution = LEDC_DUTY_RES,
      .freq_hz = LEDC_FREQUENCY,
      .clk_cfg = LEDC_AUTO_CLK,
  };
  ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));

  // Configure Red channel
  ledc_channel_config_t red_conf = {
      .speed_mode = LEDC_MODE,
      .channel = LEDC_CHANNEL_RED,
      .timer_sel = LEDC_TIMER,
      .intr_type = LEDC_INTR_DISABLE,
      .gpio_num = LED_GPIO_RED,
      .duty = 0,
      .hpoint = 0,
  };
  ESP_ERROR_CHECK(ledc_channel_config(&red_conf));

  // Configure Green channel
  ledc_channel_config_t green_conf = {
      .speed_mode = LEDC_MODE,
      .channel = LEDC_CHANNEL_GREEN,
      .timer_sel = LEDC_TIMER,
      .intr_type = LEDC_INTR_DISABLE,
      .gpio_num = LED_GPIO_GREEN,
      .duty = 0,
      .hpoint = 0,
  };
  ESP_ERROR_CHECK(ledc_channel_config(&green_conf));

  // Configure Blue channel
  ledc_channel_config_t blue_conf = {
      .speed_mode = LEDC_MODE,
      .channel = LEDC_CHANNEL_BLUE,
      .timer_sel = LEDC_TIMER,
      .intr_type = LEDC_INTR_DISABLE,
      .gpio_num = LED_GPIO_BLUE,
      .duty = 0,
      .hpoint = 0,
  };
  ESP_ERROR_CHECK(ledc_channel_config(&blue_conf));

  led_initialized = true;
  ESP_LOGI(TAG, "LED status initialized");

  // Start with BOOTING state (yellow)
  led_status_set(LED_STATUS_BOOTING);

  return ESP_OK;
}

void led_status_set(led_status_t status) {
  if (!led_initialized) {
    return;
  }

  led_status_t old_status = current_status;
  current_status = status;

  // Log with task name for debugging
  const char *task_name = pcTaskGetName(NULL);
  ESP_LOGI(TAG, "LED status: %d -> %d [%s]", old_status, status,
           task_name ? task_name : "unknown");

  // Handle effect task
  bool old_needs_effect = status_needs_effect(old_status);
  bool new_needs_effect = status_needs_effect(status);

  if (!new_needs_effect && old_needs_effect) {
    stop_effect_task();
  }

  if (!led_enabled) {
    led_off();
    return;
  }

  // Set static colors for non-effect states
  switch (status) {
  case LED_STATUS_OFF:
    stop_effect_task();
    led_off();
    break;

  case LED_STATUS_BOOTING:
    // Static yellow during boot
    apply_rgb(255, 180, 0);
    break;

  case LED_STATUS_IDLE:
    // Dim green - wake word ready
    apply_rgb(0, 80, 0);
    break;

  case LED_STATUS_LISTENING:
  case LED_STATUS_PROCESSING:
  case LED_STATUS_ERROR:
  case LED_STATUS_CONNECTING:
  case LED_STATUS_OTA:
  case LED_STATUS_SPEAKING:
    // Effects handled by task
    if (new_needs_effect && !old_needs_effect) {
      start_effect_task();
    }
    break;
  }
}

led_status_t led_status_get(void) { return current_status; }

void led_status_set_brightness(uint8_t new_brightness) {
  if (new_brightness > 100) {
    new_brightness = 100;
  }
  brightness = new_brightness;
  ESP_LOGI(TAG, "LED brightness: %d%%", brightness);

  // Reapply current color with new brightness
  apply_rgb(current_r, current_g, current_b);
}

uint8_t led_status_get_brightness(void) { return brightness; }

void led_status_enable(bool enable) {
  led_enabled = enable;
  ESP_LOGI(TAG, "LED %s", enable ? "enabled" : "disabled");

  if (!enable) {
    stop_effect_task();
    led_off();
  } else {
    led_status_set(current_status);
  }
}

bool led_status_is_enabled(void) { return led_enabled; }

void led_status_set_rgb(uint8_t r, uint8_t g, uint8_t b) {
  stop_effect_task();
  current_status = LED_STATUS_OFF; // Custom color mode
  apply_rgb(r, g, b);
}

static void led_test_task(void *arg) {
  led_status_t saved_status = led_status_get();

  led_status_set_rgb(255, 0, 0);
  vTaskDelay(pdMS_TO_TICKS(300));
  led_status_set_rgb(0, 255, 0);
  vTaskDelay(pdMS_TO_TICKS(300));
  led_status_set_rgb(0, 0, 255);
  vTaskDelay(pdMS_TO_TICKS(300));
  led_status_set_rgb(255, 255, 255);
  vTaskDelay(pdMS_TO_TICKS(300));
  led_off();

  // Restore prior status
  led_status_set(saved_status);

  test_task_handle = NULL;
  vTaskDelete(NULL);
}

void led_status_test_pattern(void) {
  if (!led_initialized) {
    return;
  }
  if (test_task_handle != NULL) {
    return;
  }
  xTaskCreate(led_test_task, "led_test", 2048, NULL, 2, &test_task_handle);
}

void led_status_deinit(void) {
  if (!led_initialized) {
    return;
  }

  stop_effect_task();
  led_off();

  ledc_stop(LEDC_MODE, LEDC_CHANNEL_RED, 0);
  ledc_stop(LEDC_MODE, LEDC_CHANNEL_GREEN, 0);
  ledc_stop(LEDC_MODE, LEDC_CHANNEL_BLUE, 0);

  led_initialized = false;
  ESP_LOGI(TAG, "LED status deinitialized");
}
