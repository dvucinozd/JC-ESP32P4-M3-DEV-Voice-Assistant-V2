/**
 * Audio Capture Implementation (ESP-SR AFE + MultiNet Version)
 * Wraps Espressif Audio Front End (AFE) for VAD, WWD, and AEC.
 * Adds Offline Command Recognition (MultiNet).
 */

#include "audio_capture.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "bsp_board_extra.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "model_path.h" 
#include "esp_vad.h" 
#include "driver/i2s_types.h"
#include "audio_ref_buffer.h"
#include "sys_diag.h" // Phase 9

static const char *TAG = "audio_capture";

// AFE Configuration
#define AFE_TASK_PRIORITY   5
#define AFE_TASK_CORE       1
#define CAPTURE_TASK_PRIORITY 6
#define CAPTURE_TASK_CORE   0
#define I2S_READ_LEN 512 

// -------------------------------------------------------------------------
// STATE VARIABLES
// -------------------------------------------------------------------------
static const esp_afe_sr_iface_t *afe_handle = NULL;
static esp_afe_sr_data_t *afe_data = NULL;
static srmodel_list_t *models = NULL;

static const esp_mn_iface_t *mn_handle = NULL;
static model_iface_data_t *mn_data = NULL;

static TaskHandle_t feed_task_handle = NULL;
static TaskHandle_t fetch_task_handle = NULL;
static volatile bool is_running = false;
static audio_capture_mode_t current_mode = CAPTURE_MODE_IDLE;
static EventGroupHandle_t capture_event_group = NULL;
#define CAPTURE_FEED_DONE_BIT  BIT0
#define CAPTURE_FETCH_DONE_BIT BIT1

static audio_capture_callback_t audio_callback = NULL;
static audio_capture_wwd_callback_t wwd_callback = NULL;
static audio_capture_vad_callback_t vad_callback = NULL;
static audio_capture_cmd_callback_t cmd_callback = NULL;

// -------------------------------------------------------------------------
// TASKS
// -------------------------------------------------------------------------

static void feed_task(void *arg) {
    sys_diag_wdt_add(); // Monitor
    int16_t *mic_buff = (int16_t *)malloc(I2S_READ_LEN * sizeof(int16_t));
    int16_t *ref_buff = (int16_t *)malloc(I2S_READ_LEN * sizeof(int16_t));
    int16_t *afe_buff = (int16_t *)malloc(I2S_READ_LEN * 2 * sizeof(int16_t)); // 2 Channels (Mic+Ref)
    size_t bytes_read;

    ESP_LOGI(TAG, "Feed Task Started (AEC Enabled)");

    if (mic_buff == NULL || ref_buff == NULL || afe_buff == NULL) {
        ESP_LOGE(TAG, "Feed task OOM (mic=%p, ref=%p, afe=%p)", mic_buff, ref_buff, afe_buff);
        free(mic_buff);
        free(ref_buff);
        free(afe_buff);
        if (capture_event_group) xEventGroupSetBits(capture_event_group, CAPTURE_FEED_DONE_BIT);
        feed_task_handle = NULL;
        sys_diag_wdt_remove();
        vTaskDelete(NULL);
    }

    while (is_running) {
        sys_diag_wdt_feed(); // Reset WDT

        // Read from I2S (Mic)
        esp_err_t ret = bsp_extra_i2s_read(mic_buff, I2S_READ_LEN * sizeof(int16_t), &bytes_read, 100);
        
        if (ret == ESP_OK && bytes_read > 0) {
            // Read Reference (Playback Loopback)
            audio_ref_buffer_read(ref_buff, I2S_READ_LEN * sizeof(int16_t));

            // Interleave [Mic, Ref, Mic, Ref...]
            for (int i = 0; i < I2S_READ_LEN; i++) {
                afe_buff[i*2] = mic_buff[i];
                afe_buff[i*2+1] = ref_buff[i];
            }

            // Feed to AFE (2 channels)
            afe_handle->feed(afe_data, afe_buff);
        } else {
             vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    free(mic_buff);
    free(ref_buff);
    free(afe_buff);
    if (capture_event_group) xEventGroupSetBits(capture_event_group, CAPTURE_FEED_DONE_BIT);
    feed_task_handle = NULL;
    sys_diag_wdt_remove();
    vTaskDelete(NULL);
}

static void fetch_task(void *arg) {
    sys_diag_wdt_add(); // Monitor
    ESP_LOGI(TAG, "Fetch Task Started");
    
    int vad_state_prev = -1;

    while (is_running) {
        sys_diag_wdt_feed(); // Reset WDT

        // Fetch processed data from AFE
        afe_fetch_result_t *res = afe_handle->fetch(afe_data);
        
        if (!res || res->ret_value == ESP_FAIL) {
            continue;
        }

        // 1. Handle Wake Word
        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "AFE: Wake Word Detected! (Index: %d)", res->wake_word_index);
            if (current_mode == CAPTURE_MODE_WAKE_WORD && wwd_callback) {
                wwd_callback(NULL, 0); 
            }
        }

        // 2. Handle Processing (VAD + MultiNet)
        if (current_mode == CAPTURE_MODE_RECORDING) {
             
             // VAD Events
             if (res->vad_state != vad_state_prev) {
                 if (res->vad_state == VAD_SPEECH) {
                     if (vad_callback) vad_callback(VAD_EVENT_SPEECH_START);
                 } else if (res->vad_state == VAD_SILENCE) {
                     if (vad_callback) vad_callback(VAD_EVENT_SPEECH_END);
                 }
                 vad_state_prev = res->vad_state;
             }
             
             // Send cleaned audio data (for Streaming to HA)
             if (audio_callback && res->data_size > 0) {
                 audio_callback((const uint8_t*)res->data, res->data_size);
             }

             // 3. MultiNet (Offline Commands)
             if (mn_handle && mn_data) {
                 // Feed MultiNet
                 esp_mn_state_t mn_state = mn_handle->detect(mn_data, res->data);

                 if (mn_state == ESP_MN_STATE_DETECTED) {
                     esp_mn_results_t *mn_result = mn_handle->get_results(mn_data);
                     if (mn_result) {
                         ESP_LOGI(TAG, "Offline Command: ID=%d, Index=%d, Prob=%.2f", 
                                  mn_result->command_id[0], mn_result->phrase_id[0], mn_result->prob[0]);
                         
                         if (cmd_callback) {
                             cmd_callback(mn_result->command_id[0], mn_result->phrase_id[0]);
                         }
                     }
                 }
             }
        }
    }
    if (capture_event_group) xEventGroupSetBits(capture_event_group, CAPTURE_FETCH_DONE_BIT);
    fetch_task_handle = NULL;
    sys_diag_wdt_remove();
    vTaskDelete(NULL);
}

// -------------------------------------------------------------------------
// PUBLIC API
// -------------------------------------------------------------------------

esp_err_t audio_capture_init(void) {
    if (afe_handle) return ESP_OK;

    ESP_LOGI(TAG, "Initializing ESP-SR AFE & MultiNet with AEC...");

    if (capture_event_group == NULL) {
        capture_event_group = xEventGroupCreate();
        if (capture_event_group == NULL) {
            ESP_LOGE(TAG, "Failed to create capture event group");
            return ESP_ERR_NO_MEM;
        }
    }
    
    // Init Reference Buffer (16KB ~ 0.5s)
    audio_ref_buffer_init(16 * 1024);
    
    // Register Hook
    bsp_extra_i2s_write_register_callback(audio_ref_buffer_write);

    // 1. Load models
    if (models == NULL) {
        models = esp_srmodel_init("model"); 
        if (models == NULL) {
            ESP_LOGE(TAG, "Failed to load models");
        }
    }

    // 2. Init AFE for AEC (Mic + Ref)
    afe_config_t *afe_config = afe_config_init("MR", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!afe_config) return ESP_FAIL;

    // Force AEC config
    afe_config->pcm_config.total_ch_num = 2;
    afe_config->pcm_config.mic_num = 1;
    afe_config->pcm_config.ref_num = 1;
    
    afe_config->wakenet_init = true;
    afe_config->vad_init = true;
    afe_config->aec_init = true;
    
    afe_handle = esp_afe_handle_from_config(afe_config);
    afe_data = afe_handle->create_from_config(afe_config);
    if (!afe_data) return ESP_FAIL;

    // 3. Init MultiNet
    if (models) {
        char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, NULL);
        if (mn_name) {
            mn_handle = esp_mn_handle_from_name(mn_name);
            if (mn_handle) {
                mn_data = mn_handle->create(mn_name, 6000);
                ESP_LOGI(TAG, "MultiNet initialized: %s", mn_name);
            }
        } else {
            ESP_LOGW(TAG, "MultiNet model not found");
        }
    }

    ESP_LOGI(TAG, "Audio subsystem ready");
    return ESP_OK;
}

void audio_capture_register_cmd_callback(audio_capture_cmd_callback_t callback) {
    cmd_callback = callback;
}

esp_err_t audio_capture_start(audio_capture_callback_t callback) {
    if (is_running) return ESP_OK;

    extern esp_err_t bsp_extra_codec_set_fs(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch); 
    bsp_extra_codec_set_fs(16000, 16, I2S_SLOT_MODE_MONO); 

    audio_callback = callback;
    current_mode = CAPTURE_MODE_RECORDING;
    is_running = true;

    if (capture_event_group) {
        xEventGroupClearBits(capture_event_group, CAPTURE_FEED_DONE_BIT | CAPTURE_FETCH_DONE_BIT);
    }

    // Increase stack for MultiNet processing
    if (xTaskCreatePinnedToCore(feed_task, "afe_feed", 8192, NULL, CAPTURE_TASK_PRIORITY, &feed_task_handle, CAPTURE_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create feed task");
        is_running = false;
        current_mode = CAPTURE_MODE_IDLE;
        return ESP_FAIL;
    }
    if (xTaskCreatePinnedToCore(fetch_task, "afe_fetch", 16384, NULL, AFE_TASK_PRIORITY, &fetch_task_handle, AFE_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create fetch task");
        is_running = false;
        current_mode = CAPTURE_MODE_IDLE;
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t audio_capture_start_wake_word_mode(audio_capture_wwd_callback_t callback) {
    if (is_running) return ESP_OK;

    extern esp_err_t bsp_extra_codec_set_fs(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch); 
    bsp_extra_codec_set_fs(16000, 16, I2S_SLOT_MODE_MONO);

    wwd_callback = callback;
    current_mode = CAPTURE_MODE_WAKE_WORD;
    is_running = true;

    if (capture_event_group) {
        xEventGroupClearBits(capture_event_group, CAPTURE_FEED_DONE_BIT | CAPTURE_FETCH_DONE_BIT);
    }

    if (xTaskCreatePinnedToCore(feed_task, "afe_feed", 8192, NULL, CAPTURE_TASK_PRIORITY, &feed_task_handle, CAPTURE_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create feed task");
        is_running = false;
        current_mode = CAPTURE_MODE_IDLE;
        return ESP_FAIL;
    }
    if (xTaskCreatePinnedToCore(fetch_task, "afe_fetch", 16384, NULL, AFE_TASK_PRIORITY, &fetch_task_handle, AFE_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create fetch task");
        is_running = false;
        current_mode = CAPTURE_MODE_IDLE;
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

void audio_capture_stop(void) {
    if (!is_running) return;
    
    is_running = false;
    current_mode = CAPTURE_MODE_IDLE;
    ESP_LOGI(TAG, "Capture Stopped");
}

esp_err_t audio_capture_stop_wait(uint32_t timeout_ms) {
    if (!is_running) {
        return ESP_OK;
    }

    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (self != NULL && (self == feed_task_handle || self == fetch_task_handle)) {
        audio_capture_stop();
        return ESP_OK;
    }

    audio_capture_stop();

    if (timeout_ms == 0 || capture_event_group == NULL) {
        return ESP_OK;
    }

    EventBits_t bits = xEventGroupWaitBits(
        capture_event_group,
        CAPTURE_FEED_DONE_BIT | CAPTURE_FETCH_DONE_BIT,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(timeout_ms)
    );

    if ((bits & (CAPTURE_FEED_DONE_BIT | CAPTURE_FETCH_DONE_BIT)) ==
        (CAPTURE_FEED_DONE_BIT | CAPTURE_FETCH_DONE_BIT)) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

void audio_capture_deinit(void) {
    if (afe_handle && afe_data) {
        afe_handle->destroy(afe_data);
        afe_data = NULL;
    }
    if (mn_handle && mn_data) {
        // MultiNet destroy if API supports it
    }
}

// -------------------------------------------------------------------------
// Wrappers
// -------------------------------------------------------------------------
audio_capture_mode_t audio_capture_get_mode(void) { return current_mode; }

esp_err_t audio_capture_enable_vad(const void *config, audio_capture_vad_callback_t callback) {
    vad_callback = callback;
    return ESP_OK;
}

void audio_capture_disable_vad(void) { vad_callback = NULL; }
void audio_capture_reset_vad(void) {}

esp_err_t audio_capture_enable_agc(uint16_t target_level) { return ESP_OK; }
void audio_capture_disable_agc(void) {}
bool audio_capture_is_agc_enabled(void) { return true; }
float audio_capture_get_agc_gain(void) { return 1.0f; }
esp_err_t audio_capture_set_agc_target(uint16_t target_level) { return ESP_OK; }
