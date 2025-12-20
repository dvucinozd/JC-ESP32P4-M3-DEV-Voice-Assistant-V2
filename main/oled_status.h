#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OLED_VA_IDLE = 0,
    OLED_VA_LISTENING,
    OLED_VA_PROCESSING,
    OLED_VA_SPEAKING,
    OLED_VA_ERROR,
} oled_va_state_t;

typedef enum {
    OLED_TTS_IDLE = 0,
    OLED_TTS_DOWNLOADING,
    OLED_TTS_PLAYING,
    OLED_TTS_ERROR,
} oled_tts_state_t;

typedef enum {
    OLED_OTA_IDLE = 0,
    OLED_OTA_RUNNING,
    OLED_OTA_OK,
    OLED_OTA_ERROR,
} oled_ota_state_t;

typedef enum {
    OLED_MUSIC_OFF = 0,
    OLED_MUSIC_PLAYING,
    OLED_MUSIC_PAUSED,
} oled_music_state_t;

esp_err_t oled_status_init(void);

void oled_status_set_safe_mode(bool enabled);
void oled_status_set_ha_connected(bool connected);
void oled_status_set_mqtt_connected(bool connected);
void oled_status_set_va_state(oled_va_state_t state);
void oled_status_set_tts_state(oled_tts_state_t state);
void oled_status_set_ota_state(oled_ota_state_t state);
void oled_status_set_music_state(oled_music_state_t state, int current_track, int total_tracks);
void oled_status_set_last_event(const char *code);
void oled_status_set_response_preview(const char *text);
void oled_status_set_ota_url_present(bool present);

#ifdef __cplusplus
}
#endif
