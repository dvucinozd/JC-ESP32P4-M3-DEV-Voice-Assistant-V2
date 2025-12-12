/**
 * Timer and Alarm Manager Implementation
 */

#include "timer_manager.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "timer_manager";

// Manager state
static struct {
    bool initialized;
    timer_manager_config_t config;
    tm_timer_t timers[MAX_TIMERS];
    alarm_t alarms[MAX_ALARMS];
    SemaphoreHandle_t mutex;
    TaskHandle_t task_handle;
    bool time_synced;
    char timezone[64];
} tm_state = {0};

// NVS keys
#define NVS_NAMESPACE "timer_mgr"
#define NVS_KEY_ALARMS "alarms"

// Forward declarations
static void timer_manager_task(void *pvParameters);
static void check_and_trigger_timers(void);
static void check_and_trigger_alarms(void);
static void save_alarms_to_nvs(void);
static void load_alarms_from_nvs(void);
static void time_sync_notification_cb(struct timeval *tv);

// ============================================================================
// Initialization / Deinitialization
// ============================================================================

esp_err_t timer_manager_init(const timer_manager_config_t *config)
{
    if (tm_state.initialized) {
        ESP_LOGW(TAG, "Timer manager already initialized");
        return ESP_OK;
    }

    if (!config) {
        ESP_LOGE(TAG, "Config is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Initializing timer manager...");

    // Copy configuration
    memcpy(&tm_state.config, config, sizeof(timer_manager_config_t));

    // Set default snooze duration if not specified
    if (tm_state.config.snooze_duration_sec == 0) {
        tm_state.config.snooze_duration_sec = DEFAULT_SNOOZE_DURATION_SEC;
    }

    // Initialize timers
    for (int i = 0; i < MAX_TIMERS; i++) {
        tm_state.timers[i].id = i;
        tm_state.timers[i].state = TIMER_STATE_INACTIVE;
        tm_state.timers[i].name[0] = '\0';
    }

    // Initialize alarms
    for (int i = 0; i < MAX_ALARMS; i++) {
        tm_state.alarms[i].id = i;
        tm_state.alarms[i].enabled = false;
        tm_state.alarms[i].label[0] = '\0';
        tm_state.alarms[i].volume = 70;
        tm_state.alarms[i].fade_in = true;
        tm_state.alarms[i].fade_duration_sec = 30;
    }

    // Load saved alarms from NVS
    load_alarms_from_nvs();

    // Create mutex
    tm_state.mutex = xSemaphoreCreateMutex();
    if (!tm_state.mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_FAIL;
    }

    // Create manager task
    BaseType_t ret = xTaskCreate(
        timer_manager_task,
        "timer_mgr",
        4096,
        NULL,
        5,
        &tm_state.task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create timer manager task");
        vSemaphoreDelete(tm_state.mutex);
        return ESP_FAIL;
    }

    tm_state.initialized = true;
    ESP_LOGI(TAG, "Timer manager initialized successfully");

    return ESP_OK;
}

esp_err_t timer_manager_deinit(void)
{
    if (!tm_state.initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing timer manager...");

    // Delete task
    if (tm_state.task_handle) {
        vTaskDelete(tm_state.task_handle);
        tm_state.task_handle = NULL;
    }

    // Delete mutex
    if (tm_state.mutex) {
        vSemaphoreDelete(tm_state.mutex);
        tm_state.mutex = NULL;
    }

    tm_state.initialized = false;
    ESP_LOGI(TAG, "Timer manager deinitialized");

    return ESP_OK;
}

// ============================================================================
// Manager Task
// ============================================================================

static void timer_manager_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Timer manager task started");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); // Tick every 1 second

        if (xSemaphoreTake(tm_state.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            check_and_trigger_timers();
            check_and_trigger_alarms();
            xSemaphoreGive(tm_state.mutex);
        }
    }
}

static void check_and_trigger_timers(void)
{
    for (int i = 0; i < MAX_TIMERS; i++) {
        tm_timer_t *timer = &tm_state.timers[i];

        if (timer->state == TIMER_STATE_RUNNING) {
            if (timer->remaining_sec > 0) {
                timer->remaining_sec--;

                // Log countdown at significant intervals
                if (timer->remaining_sec == 60 || timer->remaining_sec == 30 ||
                    timer->remaining_sec == 10 || timer->remaining_sec == 5) {
                    ESP_LOGI(TAG, "Timer %d '%s': %lu seconds remaining",
                             i, timer->name, timer->remaining_sec);
                }
            } else {
                // Timer finished
                ESP_LOGI(TAG, "⏰ Timer %d '%s' finished!", i, timer->name);
                timer->state = TIMER_STATE_FINISHED;

                // Call callback
                if (tm_state.config.timer_finished_callback) {
                    tm_state.config.timer_finished_callback(timer->id, timer->name);
                }
            }
        }
    }
}

static void check_and_trigger_alarms(void)
{
    if (!tm_state.time_synced) {
        return; // Can't check alarms without time sync
    }

    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    for (int i = 0; i < MAX_ALARMS; i++) {
        alarm_t *alarm = &tm_state.alarms[i];

        if (!alarm->enabled) {
            continue;
        }

        // Check if snooze is active
        if (alarm->snooze_active) {
            if (now >= alarm->snooze_until) {
                // Snooze period ended, trigger alarm again
                ESP_LOGI(TAG, "⏰ Snooze ended for alarm %d '%s', triggering again",
                         i, alarm->label);
                alarm->snooze_active = false;

                if (tm_state.config.alarm_triggered_callback) {
                    tm_state.config.alarm_triggered_callback(alarm->id, alarm->label);
                }
            }
            continue;
        }

        // Check if current time matches alarm time
        if (timeinfo.tm_hour == alarm->hour && timeinfo.tm_min == alarm->minute &&
            timeinfo.tm_sec == 0) { // Only trigger once per minute

            // Check if alarm should repeat today
            uint8_t today_bit = 1 << timeinfo.tm_wday;

            if (alarm->repeat_days == ALARM_REPEAT_ONCE ||
                (alarm->repeat_days & today_bit)) {

                ESP_LOGI(TAG, "⏰ Alarm %d '%s' triggered! (%02d:%02d)",
                         i, alarm->label, alarm->hour, alarm->minute);

                // Trigger callback
                if (tm_state.config.alarm_triggered_callback) {
                    tm_state.config.alarm_triggered_callback(alarm->id, alarm->label);
                }

                // Disable one-time alarms
                if (alarm->repeat_days == ALARM_REPEAT_ONCE) {
                    alarm->enabled = false;
                    save_alarms_to_nvs();
                    ESP_LOGI(TAG, "One-time alarm %d disabled", i);
                }
            }
        }
    }
}

// ============================================================================
// Timer Functions
// ============================================================================

esp_err_t timer_manager_start_timer(const char *name, uint32_t duration_sec, uint8_t *timer_id)
{
    if (!tm_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (duration_sec == 0) {
        ESP_LOGE(TAG, "Duration must be > 0");
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(tm_state.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // Find inactive timer slot
    int slot = -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (tm_state.timers[i].state == TIMER_STATE_INACTIVE ||
            tm_state.timers[i].state == TIMER_STATE_FINISHED) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        xSemaphoreGive(tm_state.mutex);
        ESP_LOGW(TAG, "All timer slots in use");
        return ESP_ERR_NO_MEM;
    }

    // Initialize timer
    tm_timer_t *timer = &tm_state.timers[slot];
    timer->duration_sec = duration_sec;
    timer->remaining_sec = duration_sec;
    timer->state = TIMER_STATE_RUNNING;
    timer->start_timestamp_ms = esp_timer_get_time() / 1000;

    if (name && strlen(name) > 0) {
        strncpy(timer->name, name, TIMER_NAME_MAX_LENGTH - 1);
        timer->name[TIMER_NAME_MAX_LENGTH - 1] = '\0';
    } else {
        snprintf(timer->name, TIMER_NAME_MAX_LENGTH, "Timer %d", slot + 1);
    }

    if (timer_id) {
        *timer_id = slot;
    }

    xSemaphoreGive(tm_state.mutex);

    ESP_LOGI(TAG, "Started timer %d '%s' for %lu seconds", slot, timer->name, duration_sec);

    return ESP_OK;
}

esp_err_t timer_manager_stop_timer(uint8_t timer_id)
{
    if (!tm_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (timer_id >= MAX_TIMERS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(tm_state.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    tm_timer_t *timer = &tm_state.timers[timer_id];
    timer->state = TIMER_STATE_INACTIVE;
    timer->remaining_sec = 0;

    xSemaphoreGive(tm_state.mutex);

    ESP_LOGI(TAG, "Stopped timer %d '%s'", timer_id, timer->name);

    return ESP_OK;
}

esp_err_t timer_manager_pause_timer(uint8_t timer_id)
{
    if (!tm_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (timer_id >= MAX_TIMERS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(tm_state.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    tm_timer_t *timer = &tm_state.timers[timer_id];

    if (timer->state == TIMER_STATE_RUNNING) {
        timer->state = TIMER_STATE_PAUSED;
        ESP_LOGI(TAG, "Paused timer %d '%s'", timer_id, timer->name);
    }

    xSemaphoreGive(tm_state.mutex);

    return ESP_OK;
}

esp_err_t timer_manager_resume_timer(uint8_t timer_id)
{
    if (!tm_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (timer_id >= MAX_TIMERS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(tm_state.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    tm_timer_t *timer = &tm_state.timers[timer_id];

    if (timer->state == TIMER_STATE_PAUSED) {
        timer->state = TIMER_STATE_RUNNING;
        ESP_LOGI(TAG, "Resumed timer %d '%s'", timer_id, timer->name);
    }

    xSemaphoreGive(tm_state.mutex);

    return ESP_OK;
}

esp_err_t timer_manager_get_timer(uint8_t timer_id, tm_timer_t *timer)
{
    if (!tm_state.initialized || !timer) {
        return ESP_ERR_INVALID_ARG;
    }

    if (timer_id >= MAX_TIMERS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(tm_state.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    memcpy(timer, &tm_state.timers[timer_id], sizeof(tm_timer_t));

    xSemaphoreGive(tm_state.mutex);

    return ESP_OK;
}

esp_err_t timer_manager_get_all_timers(tm_timer_t timers[MAX_TIMERS], uint8_t *count)
{
    if (!tm_state.initialized || !timers || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(tm_state.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    *count = 0;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (tm_state.timers[i].state != TIMER_STATE_INACTIVE) {
            memcpy(&timers[*count], &tm_state.timers[i], sizeof(tm_timer_t));
            (*count)++;
        }
    }

    xSemaphoreGive(tm_state.mutex);

    return ESP_OK;
}

esp_err_t timer_manager_find_timer_by_name(const char *name, uint8_t *timer_id)
{
    if (!tm_state.initialized || !name || !timer_id) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(tm_state.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_ERR_NOT_FOUND;

    for (int i = 0; i < MAX_TIMERS; i++) {
        if (tm_state.timers[i].state != TIMER_STATE_INACTIVE &&
            strcmp(tm_state.timers[i].name, name) == 0) {
            *timer_id = i;
            ret = ESP_OK;
            break;
        }
    }

    xSemaphoreGive(tm_state.mutex);

    return ret;
}

// ============================================================================
// Alarm Functions
// ============================================================================

esp_err_t timer_manager_create_alarm(uint8_t hour, uint8_t minute,
                                      uint8_t repeat_days, const char *label,
                                      uint8_t *alarm_id)
{
    if (!tm_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (hour > 23 || minute > 59) {
        ESP_LOGE(TAG, "Invalid time: %02d:%02d", hour, minute);
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(tm_state.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // Find inactive alarm slot
    int slot = -1;
    for (int i = 0; i < MAX_ALARMS; i++) {
        if (!tm_state.alarms[i].enabled && strlen(tm_state.alarms[i].label) == 0) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        xSemaphoreGive(tm_state.mutex);
        ESP_LOGW(TAG, "All alarm slots in use");
        return ESP_ERR_NO_MEM;
    }

    // Initialize alarm
    alarm_t *alarm = &tm_state.alarms[slot];
    alarm->hour = hour;
    alarm->minute = minute;
    alarm->repeat_days = repeat_days;
    alarm->enabled = true;
    alarm->snooze_active = false;
    alarm->snooze_count = 0;

    if (label && strlen(label) > 0) {
        strncpy(alarm->label, label, ALARM_LABEL_MAX_LENGTH - 1);
        alarm->label[ALARM_LABEL_MAX_LENGTH - 1] = '\0';
    } else {
        snprintf(alarm->label, ALARM_LABEL_MAX_LENGTH, "Alarm %d", slot + 1);
    }

    if (alarm_id) {
        *alarm_id = slot;
    }

    save_alarms_to_nvs();

    xSemaphoreGive(tm_state.mutex);

    ESP_LOGI(TAG, "Created alarm %d '%s' at %02d:%02d (repeat: 0x%02X)",
             slot, alarm->label, hour, minute, repeat_days);

    return ESP_OK;
}

esp_err_t timer_manager_set_alarm_enabled(uint8_t alarm_id, bool enabled)
{
    if (!tm_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (alarm_id >= MAX_ALARMS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(tm_state.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    tm_state.alarms[alarm_id].enabled = enabled;
    save_alarms_to_nvs();

    xSemaphoreGive(tm_state.mutex);

    ESP_LOGI(TAG, "Alarm %d %s", alarm_id, enabled ? "enabled" : "disabled");

    return ESP_OK;
}

esp_err_t timer_manager_set_alarm_time(uint8_t alarm_id, uint8_t hour, uint8_t minute)
{
    if (!tm_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (alarm_id >= MAX_ALARMS || hour > 23 || minute > 59) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(tm_state.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    tm_state.alarms[alarm_id].hour = hour;
    tm_state.alarms[alarm_id].minute = minute;
    save_alarms_to_nvs();

    xSemaphoreGive(tm_state.mutex);

    ESP_LOGI(TAG, "Alarm %d time set to %02d:%02d", alarm_id, hour, minute);

    return ESP_OK;
}

esp_err_t timer_manager_set_alarm_repeat(uint8_t alarm_id, uint8_t repeat_days)
{
    if (!tm_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (alarm_id >= MAX_ALARMS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(tm_state.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    tm_state.alarms[alarm_id].repeat_days = repeat_days;
    save_alarms_to_nvs();

    xSemaphoreGive(tm_state.mutex);

    ESP_LOGI(TAG, "Alarm %d repeat pattern set to 0x%02X", alarm_id, repeat_days);

    return ESP_OK;
}

esp_err_t timer_manager_set_alarm_sound(uint8_t alarm_id, const char *sound_file)
{
    if (!tm_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (alarm_id >= MAX_ALARMS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(tm_state.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (sound_file && strlen(sound_file) > 0) {
        strncpy(tm_state.alarms[alarm_id].sound_file, sound_file, ALARM_SOUND_PATH_MAX_LENGTH - 1);
        tm_state.alarms[alarm_id].sound_file[ALARM_SOUND_PATH_MAX_LENGTH - 1] = '\0';
    } else {
        tm_state.alarms[alarm_id].sound_file[0] = '\0';
    }

    save_alarms_to_nvs();

    xSemaphoreGive(tm_state.mutex);

    ESP_LOGI(TAG, "Alarm %d sound set to '%s'", alarm_id, sound_file ? sound_file : "default");

    return ESP_OK;
}

esp_err_t timer_manager_delete_alarm(uint8_t alarm_id)
{
    if (!tm_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (alarm_id >= MAX_ALARMS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(tm_state.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // Clear alarm
    tm_state.alarms[alarm_id].enabled = false;
    tm_state.alarms[alarm_id].label[0] = '\0';
    tm_state.alarms[alarm_id].sound_file[0] = '\0';
    tm_state.alarms[alarm_id].snooze_active = false;

    save_alarms_to_nvs();

    xSemaphoreGive(tm_state.mutex);

    ESP_LOGI(TAG, "Alarm %d deleted", alarm_id);

    return ESP_OK;
}

esp_err_t timer_manager_snooze_alarm(uint8_t alarm_id)
{
    if (!tm_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (alarm_id >= MAX_ALARMS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(tm_state.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    alarm_t *alarm = &tm_state.alarms[alarm_id];
    time_t now;
    time(&now);

    alarm->snooze_active = true;
    alarm->snooze_until = now + tm_state.config.snooze_duration_sec;
    alarm->snooze_count++;

    xSemaphoreGive(tm_state.mutex);

    ESP_LOGI(TAG, "Alarm %d snoozed for %d seconds (count: %d)",
             alarm_id, tm_state.config.snooze_duration_sec, alarm->snooze_count);

    return ESP_OK;
}

esp_err_t timer_manager_dismiss_alarm(uint8_t alarm_id)
{
    if (!tm_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (alarm_id >= MAX_ALARMS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(tm_state.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    alarm_t *alarm = &tm_state.alarms[alarm_id];
    alarm->snooze_active = false;
    alarm->snooze_count = 0;

    xSemaphoreGive(tm_state.mutex);

    ESP_LOGI(TAG, "Alarm %d dismissed", alarm_id);

    return ESP_OK;
}

esp_err_t timer_manager_get_alarm(uint8_t alarm_id, alarm_t *alarm)
{
    if (!tm_state.initialized || !alarm) {
        return ESP_ERR_INVALID_ARG;
    }

    if (alarm_id >= MAX_ALARMS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(tm_state.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    memcpy(alarm, &tm_state.alarms[alarm_id], sizeof(alarm_t));

    xSemaphoreGive(tm_state.mutex);

    return ESP_OK;
}

esp_err_t timer_manager_get_all_alarms(alarm_t alarms[MAX_ALARMS], uint8_t *count)
{
    if (!tm_state.initialized || !alarms || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(tm_state.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    *count = 0;
    for (int i = 0; i < MAX_ALARMS; i++) {
        if (strlen(tm_state.alarms[i].label) > 0) {
            memcpy(&alarms[*count], &tm_state.alarms[i], sizeof(alarm_t));
            (*count)++;
        }
    }

    xSemaphoreGive(tm_state.mutex);

    return ESP_OK;
}

// ============================================================================
// Time Sync Functions
// ============================================================================

static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "✅ Time synchronized via SNTP");
    tm_state.time_synced = true;
}

esp_err_t timer_manager_init_sntp(const char *timezone)
{
    ESP_LOGI(TAG, "Initializing SNTP time synchronization...");

    // Set timezone
    if (timezone && strlen(timezone) > 0) {
        setenv("TZ", timezone, 1);
        tzset();
        strncpy(tm_state.timezone, timezone, sizeof(tm_state.timezone) - 1);
        ESP_LOGI(TAG, "Timezone set to: %s", timezone);
    }

    // Initialize SNTP
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();

    ESP_LOGI(TAG, "SNTP initialized, waiting for time sync...");

    return ESP_OK;
}

bool timer_manager_is_time_synced(void)
{
    return tm_state.time_synced;
}

esp_err_t timer_manager_get_time_string(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", &timeinfo);

    return ESP_OK;
}

// ============================================================================
// NVS Persistence
// ============================================================================

static void save_alarms_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(handle, NVS_KEY_ALARMS, tm_state.alarms, sizeof(tm_state.alarms));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save alarms to NVS: %s", esp_err_to_name(err));
    } else {
        nvs_commit(handle);
        ESP_LOGD(TAG, "Alarms saved to NVS");
    }

    nvs_close(handle);
}

static void load_alarms_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "No saved alarms in NVS");
        return;
    }

    size_t size = sizeof(tm_state.alarms);
    err = nvs_get_blob(handle, NVS_KEY_ALARMS, tm_state.alarms, &size);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded alarms from NVS");

        // Log loaded alarms
        for (int i = 0; i < MAX_ALARMS; i++) {
            if (strlen(tm_state.alarms[i].label) > 0) {
                ESP_LOGI(TAG, "  Alarm %d: %s at %02d:%02d (%s)",
                         i, tm_state.alarms[i].label,
                         tm_state.alarms[i].hour, tm_state.alarms[i].minute,
                         tm_state.alarms[i].enabled ? "enabled" : "disabled");
            }
        }
    }

    nvs_close(handle);
}
