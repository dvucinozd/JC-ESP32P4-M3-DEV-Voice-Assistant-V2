#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALARM_MAX_COUNT 5
#define ALARM_LABEL_LEN 32

typedef struct {
    uint8_t id;
    uint8_t hour;   // 0-23
    uint8_t minute; // 0-59
    bool active;
    bool recurring; // Daily?
    char label[ALARM_LABEL_LEN];
} alarm_entry_t;

/**
 * @brief Initialize alarm manager (loads from NVS)
 */
esp_err_t alarm_manager_init(void);

/**
 * @brief Set a new alarm
 * 
 * @param hour Hour (0-23)
 * @param minute Minute (0-59)
 * @param recurring Repeat daily
 * @param label Alarm label/name
 * @param out_id Pointer to store the assigned alarm ID
 * @return ESP_OK on success
 */
esp_err_t alarm_manager_set(uint8_t hour, uint8_t minute, bool recurring, const char *label, uint8_t *out_id);

/**
 * @brief Delete an alarm by ID
 */
esp_err_t alarm_manager_delete(uint8_t id);

/**
 * @brief Get all alarms
 * 
 * @param alarms Array to store alarms
 * @param max_count Size of array
 * @param count Output number of alarms found
 */
esp_err_t alarm_manager_get_all(alarm_entry_t *alarms, size_t max_count, size_t *count);

/**
 * @brief Stop a currently ringing alarm
 */
void alarm_manager_stop_ringing(void);

#ifdef __cplusplus
}
#endif
