#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Global flag: Is the system in Safe Mode?
bool sys_diag_is_safe_mode(void);

/**
 * @brief Initialize Diagnostics (Boot detection, Crash analysis)
 * Must be called VERY early in app_main (before other systems).
 * 
 * @return ESP_OK if normal boot, ESP_FAIL if Safe Mode triggered
 */
esp_err_t sys_diag_init(void);

/**
 * @brief Initialize Task Watchdog Timer
 * @param timeout_sec Seconds before panic/reset (e.g. 15)
 */
void sys_diag_wdt_init(int timeout_sec);

/**
 * @brief Add current task to Watchdog monitoring
 * Call this inside the task you want to monitor.
 */
void sys_diag_wdt_add(void);

/**
 * @brief Feed the Watchdog (Pet the dog)
 * Call this periodically in the monitored task loop.
 */
void sys_diag_wdt_feed(void);

/**
 * @brief Remove current task from Watchdog
 */
void sys_diag_wdt_remove(void);

/**
 * @brief Get the reason for the last reset as a string
 */
const char* sys_diag_get_reset_reason(void);

/**
 * @brief Get boot loop counter
 */
int sys_diag_get_boot_count(void);

/**
 * @brief Report status to MQTT/Logs
 * Should be called after WiFi/MQTT connection established.
 */
void sys_diag_report_status(void);

#ifdef __cplusplus
}
#endif
