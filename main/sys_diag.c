#include "sys_diag.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_ha.h"
#include "led_status.h"

static const char *TAG = "sys_diag";
static const char *NVS_NAMESPACE = "diag";

static bool safe_mode_active = false;
static int boot_count = 0;
static const char *reset_reason_str = "Unknown";
static esp_reset_reason_t last_reset_reason = ESP_RST_UNKNOWN;

// Timer handle to clear boot count after stable run
static TimerHandle_t stable_timer = NULL;
static TaskHandle_t diag_worker_task_handle = NULL;

static void diag_worker_task(void *arg) {
    (void)arg;

    while (1) {
        // Wait for a timer notification to do heavier work outside the timer service task.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (safe_mode_active) {
            ESP_LOGW(TAG, "Safe mode stable period reached - clearing boot count and rebooting");
        } else {
            ESP_LOGI(TAG, "System running stable - resetting boot count");
        }

        nvs_handle_t handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
            nvs_set_i32(handle, "boot_count", 0);
            nvs_commit(handle);
            nvs_close(handle);
        }
        boot_count = 0;

        if (safe_mode_active) {
            esp_restart();
        }
    }
}

static void stable_timer_callback(TimerHandle_t xTimer) {
    (void)xTimer;
    if (diag_worker_task_handle) {
        xTaskNotifyGive(diag_worker_task_handle);
    }
}

static void determine_reset_reason(void) {
    last_reset_reason = esp_reset_reason();
    switch (last_reset_reason) {
        case ESP_RST_POWERON: reset_reason_str = "Power On"; break;
        case ESP_RST_EXT: reset_reason_str = "External Pin"; break;
        case ESP_RST_SW: reset_reason_str = "Software Reset"; break;
        case ESP_RST_PANIC: reset_reason_str = "Crash/Panic"; break;
        case ESP_RST_INT_WDT: reset_reason_str = "Interrupt WDT (Loop)"; break;
        case ESP_RST_TASK_WDT: reset_reason_str = "Task WDT (Hang)"; break;
        case ESP_RST_WDT: reset_reason_str = "Other WDT"; break;
        case ESP_RST_DEEPSLEEP: reset_reason_str = "Deep Sleep"; break;
        case ESP_RST_BROWNOUT: reset_reason_str = "Brownout (Voltage)"; break;
        case ESP_RST_SDIO: reset_reason_str = "SDIO Reset"; break;
        default: reset_reason_str = "Unknown"; break;
    }
    ESP_LOGI(TAG, "Last Reset Reason: %s", reset_reason_str);
}

esp_err_t sys_diag_init(void) {
    determine_reset_reason();

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        // NVS might not be ready if main hasn't init it yet, but we usually call this after NVS init.
        // Assuming NVS init is done in main before this.
        return ESP_OK; 
    }

    // Read Boot Count
    int32_t val = 0;
    nvs_get_i32(handle, "boot_count", &val);
    boot_count = (int)val;
    
    // Only count crash-like resets; avoid false safe-mode from flashing/manual reset.
    bool should_count = (last_reset_reason == ESP_RST_PANIC ||
                         last_reset_reason == ESP_RST_INT_WDT ||
                         last_reset_reason == ESP_RST_TASK_WDT ||
                         last_reset_reason == ESP_RST_WDT ||
                         last_reset_reason == ESP_RST_BROWNOUT);
    if (should_count) {
        boot_count++;
    } else {
        boot_count = 0;
    }
    nvs_set_i32(handle, "boot_count", boot_count);
    nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "Boot Count: %d", boot_count);

    if (diag_worker_task_handle == NULL) {
        xTaskCreate(diag_worker_task, "diag_worker", 4096, NULL, 5, &diag_worker_task_handle);
    }

    if (boot_count >= 3) {
        ESP_LOGE(TAG, "BOOT LOOP DETECTED! Entering Safe Mode.");
        safe_mode_active = true;
        // In safe mode, we do NOT start the stable timer, so count remains high
        // User must manually fix via OTA/Web or hard reset might trigger stable timer if they let it run?
        // Actually, if we stay in Safe Mode stable, we should eventually clear it?
        // Let's say Safe Mode is stable if it runs for 2 mins.
        
        stable_timer = xTimerCreate("diag_stable", pdMS_TO_TICKS(120000), pdFALSE, NULL, stable_timer_callback);
        xTimerStart(stable_timer, 0);
        
        return ESP_FAIL; // Indicate Safe Mode
    } else {
        // Normal boot - start timer to clear count if we survive
        stable_timer = xTimerCreate("diag_stable", pdMS_TO_TICKS(60000), pdFALSE, NULL, stable_timer_callback);
        xTimerStart(stable_timer, 0);
    }

    return ESP_OK;
}

bool sys_diag_is_safe_mode(void) {
    return safe_mode_active;
}

void sys_diag_wdt_init(int timeout_sec) {
    ESP_LOGI(TAG, "Initializing TWDT (Timeout: %ds)", timeout_sec);
    
    esp_task_wdt_config_t config = {
        .timeout_ms = timeout_sec * 1000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1, // Monitor idle tasks too
        .trigger_panic = true,
    };
    
    // Prefer reconfigure first to avoid noisy internal "already initialized" error logs.
    esp_err_t err = esp_task_wdt_reconfigure(&config);
    if (err == ESP_ERR_INVALID_STATE) {
        err = esp_task_wdt_init(&config);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TWDT init/reconfigure failed: %s", esp_err_to_name(err));
    }
    
    esp_task_wdt_add(NULL); // Add current task (main)
}

void sys_diag_wdt_add(void) {
    esp_task_wdt_add(NULL); // Add calling task
}

void sys_diag_wdt_feed(void) {
    esp_task_wdt_reset();
}

void sys_diag_wdt_remove(void) {
    esp_task_wdt_delete(NULL);
}

const char* sys_diag_get_reset_reason(void) {
    return reset_reason_str;
}

int sys_diag_get_boot_count(void) {
    return boot_count;
}

void sys_diag_report_status(void) {
    if (mqtt_ha_is_connected()) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Boot: %s (Count: %d)", reset_reason_str, boot_count);
        
        if (safe_mode_active) {
            strncat(msg, " [SAFE MODE]", sizeof(msg) - strlen(msg) - 1);
        }
        
        // Use va_response sensor for system messages
        mqtt_ha_update_sensor("va_response", msg);
    }
}
