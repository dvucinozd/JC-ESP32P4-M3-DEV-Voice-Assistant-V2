/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"

static const char *TAG = "bsp_extra_board";

static esp_codec_dev_handle_t play_dev_handle;
static esp_codec_dev_handle_t record_dev_handle;
static bool play_dev_open = false;
static bool record_dev_open = false;

static bool _is_audio_init = false;
static bool _is_player_init = false;
static int _vloume_intensity = CODEC_DEFAULT_VOLUME;

static SemaphoreHandle_t audio_bus_mutex = NULL;

static audio_player_cb_t audio_idle_callback = NULL;
static void *audio_idle_cb_user_data = NULL;
static char audio_file_path[128];

// I2S Write Hook
static i2s_write_callback_t i2s_write_cb = NULL;

void bsp_extra_i2s_write_register_callback(i2s_write_callback_t cb) {
    i2s_write_cb = cb;
}

static SemaphoreHandle_t get_audio_bus_mutex(void) {
    if (audio_bus_mutex == NULL) {
        audio_bus_mutex = xSemaphoreCreateMutex();
    }
    return audio_bus_mutex;
}

static void audio_bus_lock(void) {
    SemaphoreHandle_t mutex = get_audio_bus_mutex();
    if (mutex) {
        xSemaphoreTake(mutex, portMAX_DELAY);
    }
}

static void audio_bus_unlock(void) {
    if (audio_bus_mutex) {
        xSemaphoreGive(audio_bus_mutex);
    }
}

/**************************************************************************************************
 *
 * Extra Board Function
 *
 **************************************************************************************************/

static esp_err_t audio_mute_function(AUDIO_PLAYER_MUTE_SETTING setting)
{
    // Volume saved when muting and restored when unmuting. Restoring volume is necessary
    // as es8311_set_voice_mute(true) results in voice volume (REG32) being set to zero.

    bsp_extra_codec_mute_set(setting == AUDIO_PLAYER_MUTE ? true : false);

    // restore the voice volume upon unmuting
    if (setting == AUDIO_PLAYER_UNMUTE) {
        ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(play_dev_handle, _vloume_intensity), TAG, "Set Codec volume failed");
    }

    return ESP_OK;
}

static void audio_callback(audio_player_cb_ctx_t *ctx)
{
    if (audio_idle_callback) {
        ctx->user_ctx = audio_idle_cb_user_data;
        audio_idle_callback(ctx);
    }
}

esp_err_t bsp_extra_i2s_read(void *audio_buffer, size_t len, size_t *bytes_read, uint32_t timeout_ms)
{
    if (bytes_read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *bytes_read = 0;

    if (audio_buffer == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Ensure the record codec path is opened; otherwise reads can block or fail.
    if (record_dev_handle == NULL || !record_dev_open) {
        return ESP_ERR_INVALID_STATE;
    }

    i2s_chan_handle_t rx = bsp_audio_get_rx_chan();
    if (rx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return i2s_channel_read(rx, audio_buffer, len, bytes_read, ticks);
}

esp_err_t bsp_extra_i2s_write(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    if (bytes_written == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *bytes_written = 0;

    if (audio_buffer == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Ensure the playback codec path is opened; otherwise writes can block or fail.
    if (play_dev_handle == NULL || !play_dev_open) {
        return ESP_ERR_INVALID_STATE;
    }

    // Hook for AEC reference
    if (i2s_write_cb) {
        i2s_write_cb(audio_buffer, len);
    }

    i2s_chan_handle_t tx = bsp_audio_get_tx_chan();
    if (tx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return i2s_channel_write(tx, audio_buffer, len, bytes_written, ticks);
}

esp_err_t bsp_extra_codec_set_fs(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    esp_err_t ret = ESP_OK;

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = rate,
        .channel = ch,
        .bits_per_sample = bits_cfg,
    };

    audio_bus_lock();
    if (record_dev_handle) {
        if (record_dev_open) {
            esp_codec_dev_close(record_dev_handle);
            record_dev_open = false;
        }
    }
    if (play_dev_handle) {
        // Avoid calling close() on an unopened device to prevent noisy I2S driver logs.
        // Close playback after record to avoid I2S disable pending state churn.
        if (play_dev_open) {
            esp_codec_dev_close(play_dev_handle);
            play_dev_open = false;
        }
    }

    // Important ordering note:
    // On ES8311, opening the OUT-only device can power down/disable ADC paths.
    // To keep microphone capture alive (wakeword/VAD), open playback first (if needed),
    // then open the record device last so it can re-enable the ADC path.
    if (play_dev_handle) {
        esp_err_t open_ret = esp_codec_dev_open(play_dev_handle, &fs);
        ret |= open_ret;
        play_dev_open = (open_ret == ESP_OK);
        if (open_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open playback codec (ret=%s)", esp_err_to_name(open_ret));
        }
    }
    if (record_dev_handle) {
        // Program desired input level before opening (works with ES8311)
        ret |= esp_codec_dev_set_in_gain(record_dev_handle, CODEC_DEFAULT_ADC_VOLUME);
        esp_err_t open_ret = esp_codec_dev_open(record_dev_handle, &fs);
        ret |= open_ret;
        record_dev_open = (open_ret == ESP_OK);
        if (open_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open record codec (ret=%s)", esp_err_to_name(open_ret));
        }
    }

    // Restore output volume after codec reopen/reconfig.
    if (play_dev_handle && play_dev_open) {
        esp_err_t vret = esp_codec_dev_set_out_vol(play_dev_handle, _vloume_intensity);
        if (vret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to restore codec volume (%d): %s", _vloume_intensity, esp_err_to_name(vret));
            ret |= vret;
        }
    }
    audio_bus_unlock();
    return ret;
}

// Open only playback device without closing (for TTS after audio capture stops)
esp_err_t bsp_extra_codec_open_playback(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    esp_err_t ret = ESP_OK;

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = rate,
        .channel = ch,
        .bits_per_sample = bits_cfg,
    };

    audio_bus_lock();
    if (play_dev_handle) {
        // Close first to allow channel/rate reconfiguration
        if (play_dev_open) {
            esp_codec_dev_close(play_dev_handle);
            play_dev_open = false;
        }
        ret = esp_codec_dev_open(play_dev_handle, &fs);
        play_dev_open = (ret == ESP_OK);
        ESP_LOGI(TAG, "Setting codec to %d Hz, %d bits, %d channels", rate, bits_cfg, ch);

        // Restore output volume after open.
        if (play_dev_open) {
            esp_err_t vret = esp_codec_dev_set_out_vol(play_dev_handle, _vloume_intensity);
            if (vret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to restore codec volume (%d): %s", _vloume_intensity, esp_err_to_name(vret));
                ret |= vret;
            }
        }
    }
    audio_bus_unlock();

    return ret;
}

esp_err_t bsp_extra_codec_volume_set(int volume, int *volume_set)
{
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(play_dev_handle, volume), TAG, "Set Codec volume failed");
    _vloume_intensity = volume;

    ESP_LOGI(TAG, "Setting volume: %d", volume);

    return ESP_OK;
}

int bsp_extra_codec_volume_get(void)
{
    return _vloume_intensity;
}

esp_err_t bsp_extra_codec_mute_set(bool enable)
{
    esp_err_t ret = ESP_OK;
    audio_bus_lock();
    ret = esp_codec_dev_set_out_mute(play_dev_handle, enable);
    audio_bus_unlock();
    return ret;
}

esp_err_t bsp_extra_codec_dev_stop(void)
{
    esp_err_t ret = ESP_OK;

    audio_bus_lock();
    if (record_dev_handle) {
        if (record_dev_open) {
            ret = esp_codec_dev_close(record_dev_handle);
            record_dev_open = false;
        }
    }

    if (play_dev_handle) {
        if (play_dev_open) {
            ret = esp_codec_dev_close(play_dev_handle);
            play_dev_open = false;
        }
    }
    audio_bus_unlock();
    return ret;
}

esp_err_t bsp_extra_codec_dev_resume(void)
{
    return bsp_extra_codec_set_fs(CODEC_DEFAULT_SAMPLE_RATE, CODEC_DEFAULT_BIT_WIDTH, CODEC_DEFAULT_CHANNEL);
}

esp_err_t bsp_extra_codec_init()
{
    if (_is_audio_init) {
        return ESP_OK;
    }

    play_dev_handle = bsp_audio_codec_speaker_init();
    assert((play_dev_handle) && "play_dev_handle not initialized");

    record_dev_handle = bsp_audio_codec_microphone_init();
    assert((record_dev_handle) && "record_dev_handle not initialized");

    bsp_extra_codec_set_fs(CODEC_DEFAULT_SAMPLE_RATE, CODEC_DEFAULT_BIT_WIDTH, CODEC_DEFAULT_CHANNEL);

    _is_audio_init = true;

    return ESP_OK;
}

esp_err_t bsp_extra_player_init(void)
{
    if (_is_player_init) {
        return ESP_OK;
    }

    audio_player_config_t config = { .mute_fn = audio_mute_function,
                                     .write_fn = bsp_extra_i2s_write,
                                     .clk_set_fn = bsp_extra_codec_set_fs,
                                     .priority = 5
                                   };
    ESP_RETURN_ON_ERROR(audio_player_new(config), TAG, "audio_player_init failed");
    audio_player_callback_register(audio_callback, NULL);

    _is_player_init = true;

    return ESP_OK;
}

esp_err_t bsp_extra_player_del(void)
{
    _is_player_init = false;

    ESP_RETURN_ON_ERROR(audio_player_delete(), TAG, "audio_player_delete failed");

    return ESP_OK;
}

esp_err_t bsp_extra_file_instance_init(const char *path, file_iterator_instance_t **ret_instance)
{
    ESP_RETURN_ON_FALSE(path, ESP_FAIL, TAG, "path is NULL");
    ESP_RETURN_ON_FALSE(ret_instance, ESP_FAIL, TAG, "ret_instance is NULL");

    file_iterator_instance_t *file_iterator = file_iterator_new(path);
    ESP_RETURN_ON_FALSE(file_iterator, ESP_FAIL, TAG, "file_iterator_new failed, %s", path);

    *ret_instance = file_iterator;

    return ESP_OK;
}

esp_err_t bsp_extra_player_play_index(file_iterator_instance_t *instance, int index)
{
    ESP_RETURN_ON_FALSE(instance, ESP_FAIL, TAG, "instance is NULL");

    ESP_LOGI(TAG, "play_index(%d)", index);
    char filename[128];
    int retval = file_iterator_get_full_path_from_index(instance, index, filename, sizeof(filename));
    ESP_RETURN_ON_FALSE(retval != 0, ESP_FAIL, TAG, "file_iterator_get_full_path_from_index failed");

    ESP_LOGI(TAG, "opening file '%s'", filename);
    FILE *fp = fopen(filename, "rb");
    ESP_RETURN_ON_FALSE(fp, ESP_FAIL, TAG, "unable to open file");

    ESP_LOGI(TAG, "Playing '%s'", filename);
    ESP_RETURN_ON_ERROR(audio_player_play(fp), TAG, "audio_player_play failed");

    memcpy(audio_file_path, filename, sizeof(audio_file_path));

    return ESP_OK;
}

esp_err_t bsp_extra_player_play_file(const char *file_path)
{
    ESP_LOGI(TAG, "opening file '%s'", file_path);
    FILE *fp = fopen(file_path, "rb");
    ESP_RETURN_ON_FALSE(fp, ESP_FAIL, TAG, "unable to open file");

    ESP_LOGI(TAG, "Playing '%s'", file_path);
    ESP_RETURN_ON_ERROR(audio_player_play(fp), TAG, "audio_player_play failed");

    memcpy(audio_file_path, file_path, sizeof(audio_file_path));

    return ESP_OK;
}

void bsp_extra_player_register_callback(audio_player_cb_t cb, void *user_data)
{
    audio_idle_callback = cb;
    audio_idle_cb_user_data = user_data;
}

bool bsp_extra_player_is_playing_by_path(const char *file_path)
{
    return (strcmp(audio_file_path, file_path) == 0);
}

bool bsp_extra_player_is_playing_by_index(file_iterator_instance_t *instance, int index)
{
    return (index == file_iterator_get_index(instance));
}
