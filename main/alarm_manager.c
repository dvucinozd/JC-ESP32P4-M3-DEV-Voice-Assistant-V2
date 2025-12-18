#include "alarm_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sntp.h"
#include <string.h>
#include <time.h>

#include "voice_pipeline.h" // To trigger events

static const char *TAG = "alarm_mgr";
static const char *NVS_NAMESPACE = "alarms";

static alarm_entry_t alarms[ALARM_MAX_COUNT];
static bool is_ringing = false;
static uint8_t ringing_alarm_id = 0;

static void alarm_check_task(void *arg) {
    time_t now;
    struct tm timeinfo;
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); // Check every second

        time(&now);
        localtime_r(&now, &timeinfo);

        // Simple debounce: only trigger at 00 seconds
        if (timeinfo.tm_sec != 0) continue;

        for (int i = 0; i < ALARM_MAX_COUNT; i++) {
            if (alarms[i].active) {
                if (alarms[i].hour == timeinfo.tm_hour && alarms[i].minute == timeinfo.tm_min) {
                    ESP_LOGI(TAG, "⏰ ALARM TRIGGERED: %s", alarms[i].label);
                    
                    // Trigger pipeline event
                    // We need a way to tell pipeline to play alarm sound.
                    voice_pipeline_trigger_alarm(alarms[i].id); 
                    
                    if (!alarms[i].recurring) {
                        alarms[i].active = false;
                        // Save state update to NVS asynchronously if possible, or just dirty flag
                    }
                    
                    is_ringing = true;
                    ringing_alarm_id = alarms[i].id;
                }
            }
        }
    }
}

static esp_err_t load_alarms(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t size = sizeof(alarms);
    err = nvs_get_blob(handle, "data", alarms, &size);
    nvs_close(handle);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Alarms loaded from NVS");
    }
    return err;
}

static esp_err_t save_alarms(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(handle, "data", alarms, sizeof(alarms));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t alarm_manager_init(void) {
    ESP_LOGI(TAG, "Initializing Alarm Manager");
    
    // Clear alarms initially
    memset(alarms, 0, sizeof(alarms));
    
    // Load from NVS
    load_alarms();

    // Start check task
    xTaskCreate(alarm_check_task, "alarm_check", 2048, NULL, 5, NULL);
    
    return ESP_OK;
}

esp_err_t alarm_manager_set(uint8_t hour, uint8_t minute, bool recurring, const char *label, uint8_t *out_id) {
    // Find empty slot
    int slot = -1;
    for (int i = 0; i < ALARM_MAX_COUNT; i++) {
        if (!alarms[i].active) {
            slot = i;
            break;
        }
    }

    if (slot == -1) return ESP_ERR_NO_MEM; // Max alarms reached

    alarms[slot].id = slot + 1; // ID 1-based
    alarms[slot].hour = hour;
    alarms[slot].minute = minute;
    alarms[slot].active = true;
    alarms[slot].recurring = recurring;
    strncpy(alarms[slot].label, label ? label : "Alarm", ALARM_LABEL_LEN - 1);
    alarms[slot].label[ALARM_LABEL_LEN - 1] = '\0';
    
    if (out_id) *out_id = alarms[slot].id;

    save_alarms();
    ESP_LOGI(TAG, "Alarm set for %02d:%02d (%s)", hour, minute, label);
    return ESP_OK;
}

esp_err_t alarm_manager_delete(uint8_t id) {
    for (int i = 0; i < ALARM_MAX_COUNT; i++) {
        if (alarms[i].id == id) {
            alarms[i].active = false;
            save_alarms();
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t alarm_manager_get_all(alarm_entry_t *out_alarms, size_t max_count, size_t *count) {
    if (!out_alarms || !count) return ESP_ERR_INVALID_ARG;
    
    *count = 0;
    for (int i = 0; i < ALARM_MAX_COUNT; i++) {
        if (alarms[i].active && *count < max_count) {
            out_alarms[*count] = alarms[i];
            (*count)++;
        }
    }
    return ESP_OK;
}

void alarm_manager_stop_ringing(void) {
    if (is_ringing) {
        ESP_LOGI(TAG, "Alarm stopped");
        is_ringing = false;
        // Notify pipeline to stop playing sound if needed
    }
}
