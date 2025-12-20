/**
 * @file ota_update.c
 * @brief OTA (Over-The-Air) firmware update implementation
 */

#include "ota_update.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_status.h"
#include "oled_status.h"
#include <string.h>

static const char *TAG = "ota_update";

#define OTA_TASK_STACK_WORDS 4096

// OTA state
static ota_state_t ota_state = OTA_STATE_IDLE;
static int ota_progress = 0;
static bool ota_running = false;
static ota_progress_callback_t progress_callback = NULL;

// Task handle
static TaskHandle_t ota_task_handle = NULL;

typedef struct {
  char *url;
  StackType_t *stack;
  StaticTask_t *tcb;
} ota_task_ctx_t;

/**
 * @brief Notify progress callback
 */
static void notify_progress(ota_state_t state, int progress,
                            const char *message) {
  ota_state = state;
  ota_progress = progress;

  if (progress_callback) {
    progress_callback(state, progress, message);
  }

  ESP_LOGI(TAG, "[%d%%] %s", progress, message);
}

/**
 * @brief OTA update task - Uses direct HTTP client + OTA ops for HTTP support
 */
static void ota_update_task(void *pvParameter) {
  ota_task_ctx_t *ctx = (ota_task_ctx_t *)pvParameter;
  const char *url = ctx ? ctx->url : NULL;
  esp_err_t ret = ESP_FAIL;
  esp_ota_handle_t ota_handle = 0;
  const esp_partition_t *update_partition = NULL;
  char *buffer = NULL;
  const int buffer_size = 4096;
  int total_read = 0;
  int content_length = 0;

  if (!url) {
    ESP_LOGE(TAG, "OTA URL is NULL");
    goto ota_end;
  }

  ESP_LOGI(TAG, "Starting OTA update from: %s", url);
  notify_progress(OTA_STATE_DOWNLOADING, 0, "Starting OTA update");
  oled_status_set_ota_state(OLED_OTA_RUNNING);
  oled_status_set_last_event("ota-start");

  // Set LED to OTA mode (white breathing)
  led_status_set(LED_STATUS_OTA);

  // Allocate download buffer
  buffer = malloc(buffer_size);
  if (buffer == NULL) {
    ESP_LOGE(TAG, "Failed to allocate buffer");
    notify_progress(OTA_STATE_FAILED, 0, "Memory allocation failed");
    goto ota_end;
  }

  // Configure HTTP client for plain HTTP
  esp_http_client_config_t config = {
      .url = url,
      .timeout_ms = 30000,
      .keep_alive_enable = true,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    ESP_LOGE(TAG, "Failed to init HTTP client");
    notify_progress(OTA_STATE_FAILED, 0, "HTTP client init failed");
    goto ota_end;
  }

  // Open HTTP connection
  ret = esp_http_client_open(client, 0);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(ret));
    notify_progress(OTA_STATE_FAILED, 0, "HTTP connection failed");
    esp_http_client_cleanup(client);
    goto ota_end;
  }

  // Fetch headers
  content_length = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  if (status != 200) {
    ESP_LOGE(TAG, "HTTP status %d", status);
    notify_progress(OTA_STATE_FAILED, 0, "HTTP status not OK");
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    goto ota_end;
  }

  bool length_known = (content_length > 0);
  if (length_known) {
    ESP_LOGI(TAG, "Image size: %d bytes", content_length);
  } else {
    ESP_LOGW(TAG, "Image size unknown (no Content-Length)");
  }

  // Get update partition
  update_partition = esp_ota_get_next_update_partition(NULL);
  if (update_partition == NULL) {
    ESP_LOGE(TAG, "No OTA partition found");
    notify_progress(OTA_STATE_FAILED, 0, "No OTA partition");
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    goto ota_end;
  }

  ESP_LOGI(TAG, "Writing to partition: %s at 0x%lx", update_partition->label,
           update_partition->address);

  // Begin OTA
  ret =
      esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(ret));
    notify_progress(OTA_STATE_FAILED, 0, "OTA begin failed");
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    goto ota_end;
  }

  // Download and write firmware
  while (1) {
    int read_len = esp_http_client_read(client, buffer, buffer_size);
    if (read_len < 0) {
      ESP_LOGE(TAG, "HTTP read error");
      notify_progress(OTA_STATE_FAILED, ota_progress, "Download error");
      esp_ota_abort(ota_handle);
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      goto ota_end;
    } else if (read_len == 0) {
      // Connection closed
      break;
    }

    // Write to flash
    ret = esp_ota_write(ota_handle, buffer, read_len);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(ret));
      notify_progress(OTA_STATE_FAILED, ota_progress, "Flash write failed");
      esp_ota_abort(ota_handle);
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      goto ota_end;
    }

    total_read += read_len;
    char msg[64];
    int progress = 0;
    if (length_known) {
      progress = (total_read * 100) / content_length;
      snprintf(msg, sizeof(msg), "Downloading: %d/%d bytes", total_read,
               content_length);
    } else {
      snprintf(msg, sizeof(msg), "Downloading: %d bytes", total_read);
    }
    notify_progress(OTA_STATE_DOWNLOADING, progress, msg);
  }

  // Close HTTP
  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  // Verify complete download
  if (total_read == 0) {
    ESP_LOGE(TAG, "Download failed: no data received");
    notify_progress(OTA_STATE_FAILED, ota_progress, "No data received");
    esp_ota_abort(ota_handle);
    goto ota_end;
  }

  if (length_known && total_read != content_length) {
    ESP_LOGE(TAG, "Incomplete download: %d/%d", total_read, content_length);
    notify_progress(OTA_STATE_FAILED, ota_progress, "Incomplete download");
    esp_ota_abort(ota_handle);
    goto ota_end;
  }

  notify_progress(OTA_STATE_VERIFYING, 100, "Verifying firmware");

  // Finish OTA
  ret = esp_ota_end(ota_handle);
  if (ret != ESP_OK) {
    if (ret == ESP_ERR_OTA_VALIDATE_FAILED) {
      ESP_LOGE(TAG, "Image validation failed");
      notify_progress(OTA_STATE_FAILED, 100, "Image validation failed");
    } else {
      ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(ret));
      notify_progress(OTA_STATE_FAILED, 100, "OTA finish failed");
    }
    goto ota_end;
  }

  // Set boot partition
  ret = esp_ota_set_boot_partition(update_partition);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(ret));
    notify_progress(OTA_STATE_FAILED, 100, "Set boot partition failed");
    goto ota_end;
  }

  ESP_LOGI(TAG, "OTA update successful!");
  notify_progress(OTA_STATE_SUCCESS, 100, "Update successful - Rebooting...");
  oled_status_set_ota_state(OLED_OTA_OK);
  oled_status_set_last_event("ota-ok");

  vTaskDelay(pdMS_TO_TICKS(2000));
  esp_restart();

ota_end:
  // Restore LED to IDLE on failure (success path restarts, so this only runs on
  // failure)
  if (ota_state == OTA_STATE_FAILED) {
    led_status_set(LED_STATUS_IDLE);
    oled_status_set_ota_state(OLED_OTA_ERROR);
    oled_status_set_last_event("ota-fail");
  }

  // Free buffer
  if (buffer) {
    free(buffer);
  }

  // Free URL string
  if (ctx) {
    if (ctx->url) {
      free(ctx->url);
    }
    if (ctx->stack) {
      heap_caps_free(ctx->stack);
    }
    if (ctx->tcb) {
      heap_caps_free(ctx->tcb);
    }
    free(ctx);
  }

  ota_running = false;
  ota_task_handle = NULL;
  vTaskDelete(NULL);
}

/**
 * @brief Initialize OTA update module
 */
esp_err_t ota_update_init(void) {
  ESP_LOGI(TAG, "OTA update module initialized");
  ESP_LOGI(TAG, "Current version: %s", ota_update_get_current_version());

  // Check if we rolled back from a failed update
  if (ota_update_check_rollback()) {
    ESP_LOGW(TAG, "Device rolled back from failed OTA update");
  }

  return ESP_OK;
}

/**
 * @brief Start OTA update from HTTP URL
 */
esp_err_t ota_update_start(const char *url) {
  if (ota_running) {
    ESP_LOGW(TAG, "OTA update already in progress");
    return ESP_ERR_INVALID_STATE;
  }

  if (!url || strlen(url) == 0) {
    ESP_LOGE(TAG, "Invalid URL");
    return ESP_ERR_INVALID_ARG;
  }

  ESP_LOGI(TAG, "Starting OTA update task");

  // Duplicate URL string (task will free it)
  ota_task_ctx_t *ctx = (ota_task_ctx_t *)calloc(1, sizeof(*ctx));
  if (!ctx) {
    ESP_LOGE(TAG, "Failed to allocate OTA context");
    return ESP_ERR_NO_MEM;
  }

  ctx->url = strdup(url);
  if (!ctx->url) {
    ESP_LOGE(TAG, "Failed to allocate URL string");
    free(ctx);
    return ESP_ERR_NO_MEM;
  }

  ota_running = true;
  ota_state = OTA_STATE_IDLE;
  ota_progress = 0;

  // Create OTA task
  BaseType_t ret = xTaskCreate(ota_update_task, "ota_update_task",
                               OTA_TASK_STACK_WORDS, (void *)ctx, 5,
                               &ota_task_handle);

  if (ret != pdPASS) {
    ESP_LOGW(TAG, "OTA task create failed; internal free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    ctx->stack = (StackType_t *)heap_caps_malloc(
        OTA_TASK_STACK_WORDS * sizeof(StackType_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ctx->tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t),
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ctx->stack || !ctx->tcb) {
      ESP_LOGE(TAG, "Failed to allocate OTA task stack/TCB");
      if (ctx->stack) {
        heap_caps_free(ctx->stack);
      }
      if (ctx->tcb) {
        heap_caps_free(ctx->tcb);
      }
      free(ctx->url);
      free(ctx);
      ota_running = false;
      return ESP_FAIL;
    }

    ota_task_handle = xTaskCreateStaticPinnedToCore(
        ota_update_task, "ota_update_task", OTA_TASK_STACK_WORDS, (void *)ctx,
        5, ctx->stack, ctx->tcb, tskNO_AFFINITY);
    if (ota_task_handle == NULL) {
      ESP_LOGE(TAG, "Failed to create OTA task (static)");
      heap_caps_free(ctx->stack);
      heap_caps_free(ctx->tcb);
      free(ctx->url);
      free(ctx);
      ota_running = false;
      return ESP_FAIL;
    }
  }

  return ESP_OK;
}

/**
 * @brief Check if OTA update is in progress
 */
bool ota_update_is_running(void) { return ota_running; }

/**
 * @brief Get current OTA state
 */
ota_state_t ota_update_get_state(void) { return ota_state; }

/**
 * @brief Get current OTA progress
 */
int ota_update_get_progress(void) { return ota_progress; }

/**
 * @brief Register progress callback
 */
void ota_update_register_callback(ota_progress_callback_t callback) {
  progress_callback = callback;
  ESP_LOGI(TAG, "Progress callback registered");
}

/**
 * @brief Get current firmware version
 */
const char *ota_update_get_current_version(void) {
  const esp_app_desc_t *app_desc = esp_app_get_description();
  return app_desc->version;
}

/**
 * @brief Check if partition was rolled back
 */
bool ota_update_check_rollback(void) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t ota_state;

  if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
      ESP_LOGW(TAG, "Running partition is in pending verify state");
      return true;
    }
  }

  return false;
}

/**
 * @brief Mark current partition as valid
 */
esp_err_t ota_update_mark_valid(void) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t ota_state;

  if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
      ESP_LOGI(TAG, "Marking current partition as valid");
      esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mark partition valid: %s",
                 esp_err_to_name(err));
        return err;
      }
      ESP_LOGI(TAG, "Current partition marked as valid");
    }
  }

  return ESP_OK;
}
