/**
 * Timer and Alarm Manager
 *
 * Provides timer countdown and alarm clock functionality with:
 * - Multiple concurrent timers (up to 5)
 * - Recurring alarms with daily/weekly patterns
 * - Snooze functionality
 * - MQTT integration
 * - TTS notifications
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Configuration
#define MAX_TIMERS 5
#define MAX_ALARMS 5
#define TIMER_NAME_MAX_LENGTH 32
#define ALARM_LABEL_MAX_LENGTH 32
#define ALARM_SOUND_PATH_MAX_LENGTH 64
#define DEFAULT_SNOOZE_DURATION_SEC 600  // 10 minutes

// Timer states
typedef enum {
    TIMER_STATE_INACTIVE = 0,
    TIMER_STATE_RUNNING,
    TIMER_STATE_PAUSED,
    TIMER_STATE_FINISHED
} timer_state_t;

// Alarm repeat patterns (bitfield for days of week)
typedef enum {
    ALARM_REPEAT_ONCE     = 0x00,  // Single time
    ALARM_REPEAT_DAILY    = 0x7F,  // Every day (bits 0-6 set)
    ALARM_REPEAT_WEEKDAYS = 0x1F,  // Mon-Fri (bits 0-4 set)
    ALARM_REPEAT_WEEKENDS = 0x60,  // Sat-Sun (bits 5-6 set)
    ALARM_REPEAT_MONDAY    = 0x01,
    ALARM_REPEAT_TUESDAY   = 0x02,
    ALARM_REPEAT_WEDNESDAY = 0x04,
    ALARM_REPEAT_THURSDAY  = 0x08,
    ALARM_REPEAT_FRIDAY    = 0x10,
    ALARM_REPEAT_SATURDAY  = 0x20,
    ALARM_REPEAT_SUNDAY    = 0x40
} alarm_repeat_pattern_t;

// Timer structure (renamed to avoid conflict with sys/types.h timer_t)
typedef struct {
    uint8_t id;
    char name[TIMER_NAME_MAX_LENGTH];
    uint32_t duration_sec;       // Total duration
    uint32_t remaining_sec;      // Time remaining
    timer_state_t state;
    uint64_t start_timestamp_ms; // For pause/resume
} tm_timer_t;

// Alarm structure
typedef struct {
    uint8_t id;
    char label[ALARM_LABEL_MAX_LENGTH];
    uint8_t hour;                // 0-23
    uint8_t minute;              // 0-59
    bool enabled;
    uint8_t repeat_days;         // Bitfield: alarm_repeat_pattern_t
    bool snooze_active;
    uint8_t snooze_count;
    time_t snooze_until;         // Unix timestamp when snooze ends
    char sound_file[ALARM_SOUND_PATH_MAX_LENGTH];
    uint8_t volume;              // 0-100
    bool fade_in;                // Gradual volume increase
    uint8_t fade_duration_sec;   // Duration of fade-in
} alarm_t;

// Callback types
typedef void (*timer_callback_t)(uint8_t timer_id, const char *timer_name);
typedef void (*alarm_callback_t)(uint8_t alarm_id, const char *alarm_label);

// Configuration structure
typedef struct {
    timer_callback_t timer_finished_callback;
    alarm_callback_t alarm_triggered_callback;
    uint16_t snooze_duration_sec;
    bool tts_notifications;       // Enable TTS for timer/alarm
    bool play_sound;              // Play sound when timer/alarm triggers
} timer_manager_config_t;

/**
 * @brief Initialize timer manager
 *
 * @param config Configuration structure
 * @return esp_err_t ESP_OK on success
 */
esp_err_t timer_manager_init(const timer_manager_config_t *config);

/**
 * @brief Deinitialize timer manager
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t timer_manager_deinit(void);

// ============================================================================
// Timer Functions
// ============================================================================

/**
 * @brief Start a new timer
 *
 * @param name Timer name (optional, can be NULL)
 * @param duration_sec Duration in seconds
 * @param timer_id Output: assigned timer ID (0-4)
 * @return esp_err_t ESP_OK on success, ESP_ERR_NO_MEM if all timers in use
 */
esp_err_t timer_manager_start_timer(const char *name, uint32_t duration_sec, uint8_t *timer_id);

/**
 * @brief Stop/cancel a timer
 *
 * @param timer_id Timer ID (0-4)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t timer_manager_stop_timer(uint8_t timer_id);

/**
 * @brief Pause a running timer
 *
 * @param timer_id Timer ID (0-4)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t timer_manager_pause_timer(uint8_t timer_id);

/**
 * @brief Resume a paused timer
 *
 * @param timer_id Timer ID (0-4)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t timer_manager_resume_timer(uint8_t timer_id);

/**
 * @brief Get timer information
 *
 * @param timer_id Timer ID (0-4)
 * @param timer Output: timer information
 * @return esp_err_t ESP_OK on success
 */
esp_err_t timer_manager_get_timer(uint8_t timer_id, tm_timer_t *timer);

/**
 * @brief Get all active timers
 *
 * @param timers Output: array of timers
 * @param count Output: number of active timers
 * @return esp_err_t ESP_OK on success
 */
esp_err_t timer_manager_get_all_timers(tm_timer_t timers[MAX_TIMERS], uint8_t *count);

/**
 * @brief Find timer by name
 *
 * @param name Timer name
 * @param timer_id Output: timer ID if found
 * @return esp_err_t ESP_OK if found, ESP_ERR_NOT_FOUND otherwise
 */
esp_err_t timer_manager_find_timer_by_name(const char *name, uint8_t *timer_id);

// ============================================================================
// Alarm Functions
// ============================================================================

/**
 * @brief Create a new alarm
 *
 * @param hour Hour (0-23)
 * @param minute Minute (0-59)
 * @param repeat_days Repeat pattern (bitfield)
 * @param label Alarm label (optional)
 * @param alarm_id Output: assigned alarm ID (0-4)
 * @return esp_err_t ESP_OK on success, ESP_ERR_NO_MEM if all alarms in use
 */
esp_err_t timer_manager_create_alarm(uint8_t hour, uint8_t minute,
                                      uint8_t repeat_days, const char *label,
                                      uint8_t *alarm_id);

/**
 * @brief Enable/disable an alarm
 *
 * @param alarm_id Alarm ID (0-4)
 * @param enabled Enable (true) or disable (false)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t timer_manager_set_alarm_enabled(uint8_t alarm_id, bool enabled);

/**
 * @brief Update alarm time
 *
 * @param alarm_id Alarm ID (0-4)
 * @param hour New hour (0-23)
 * @param minute New minute (0-59)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t timer_manager_set_alarm_time(uint8_t alarm_id, uint8_t hour, uint8_t minute);

/**
 * @brief Update alarm repeat pattern
 *
 * @param alarm_id Alarm ID (0-4)
 * @param repeat_days Repeat pattern (bitfield)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t timer_manager_set_alarm_repeat(uint8_t alarm_id, uint8_t repeat_days);

/**
 * @brief Set alarm sound file
 *
 * @param alarm_id Alarm ID (0-4)
 * @param sound_file Path to sound file (NULL for default)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t timer_manager_set_alarm_sound(uint8_t alarm_id, const char *sound_file);

/**
 * @brief Delete an alarm
 *
 * @param alarm_id Alarm ID (0-4)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t timer_manager_delete_alarm(uint8_t alarm_id);

/**
 * @brief Snooze the currently active alarm
 *
 * @param alarm_id Alarm ID (0-4)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t timer_manager_snooze_alarm(uint8_t alarm_id);

/**
 * @brief Dismiss the currently active alarm
 *
 * @param alarm_id Alarm ID (0-4)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t timer_manager_dismiss_alarm(uint8_t alarm_id);

/**
 * @brief Get alarm information
 *
 * @param alarm_id Alarm ID (0-4)
 * @param alarm Output: alarm information
 * @return esp_err_t ESP_OK on success
 */
esp_err_t timer_manager_get_alarm(uint8_t alarm_id, alarm_t *alarm);

/**
 * @brief Get all alarms
 *
 * @param alarms Output: array of alarms
 * @param count Output: number of alarms
 * @return esp_err_t ESP_OK on success
 */
esp_err_t timer_manager_get_all_alarms(alarm_t alarms[MAX_ALARMS], uint8_t *count);

// ============================================================================
// Time Sync Functions
// ============================================================================

/**
 * @brief Initialize SNTP time synchronization
 *
 * @param timezone Timezone string (e.g., "CET-1CEST,M3.5.0,M10.5.0/3")
 * @return esp_err_t ESP_OK on success
 */
esp_err_t timer_manager_init_sntp(const char *timezone);

/**
 * @brief Check if time is synchronized
 *
 * @return true if time is synchronized
 */
bool timer_manager_is_time_synced(void);

/**
 * @brief Get current time as string
 *
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @return esp_err_t ESP_OK on success
 */
esp_err_t timer_manager_get_time_string(char *buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif
