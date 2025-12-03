/**
 * TTS Audio Player
 * Handles playback of TTS audio from Home Assistant
 */

#ifndef TTS_PLAYER_H
#define TTS_PLAYER_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize TTS audio player
 *
 * @return ESP_OK on success
 */
esp_err_t tts_player_init(void);

/**
 * @brief Feed audio data chunk to player
 *
 * @param audio_data Audio chunk (MP3/PCM format)
 * @param length Length of audio data
 * @return ESP_OK on success
 */
esp_err_t tts_player_feed(const uint8_t *audio_data, size_t length);

/**
 * @brief Stop playback and clear buffer
 */
void tts_player_stop(void);

/**
 * @brief Deinitialize TTS player
 */
void tts_player_deinit(void);

/**
 * @brief Callback when TTS playback completes
 */
typedef void (*tts_playback_complete_callback_t)(void);

/**
 * @brief Register callback for TTS playback completion
 *
 * @param callback Function to call when playback finishes
 */
void tts_player_register_complete_callback(tts_playback_complete_callback_t callback);

#ifdef __cplusplus
}
#endif

#endif /* TTS_PLAYER_H */
