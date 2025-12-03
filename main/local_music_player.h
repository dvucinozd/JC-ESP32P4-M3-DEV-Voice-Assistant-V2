/**
 * @file local_music_player.h
 * @brief Local music player for SD card MP3 playback
 *
 * Manages playback of MP3 files from /sdcard/music directory.
 * Only active when Ethernet is connected (SD card mounted).
 */

#ifndef LOCAL_MUSIC_PLAYER_H
#define LOCAL_MUSIC_PLAYER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Music player state
 */
typedef enum {
    MUSIC_STATE_IDLE = 0,       ///< No music playing
    MUSIC_STATE_PLAYING,        ///< Music is playing
    MUSIC_STATE_PAUSED,         ///< Music is paused
    MUSIC_STATE_STOPPED         ///< Music stopped
} music_state_t;

/**
 * @brief Music player event callback
 */
typedef void (*music_event_callback_t)(music_state_t state, int current_track, int total_tracks);

/**
 * @brief Initialize local music player
 *
 * Scans /sdcard/music directory for MP3 files.
 * Must be called after SD card is mounted.
 *
 * @return ESP_OK on success
 *         ESP_FAIL if SD card not mounted or no music found
 */
esp_err_t local_music_player_init(void);

/**
 * @brief Deinitialize local music player
 *
 * Stops playback and releases resources.
 * Called when SD card is unmounted (WiFi fallback).
 *
 * @return ESP_OK on success
 */
esp_err_t local_music_player_deinit(void);

/**
 * @brief Start playing music
 *
 * Starts playing from first track or resumes if paused.
 *
 * @return ESP_OK on success
 *         ESP_FAIL if player not initialized or no tracks
 */
esp_err_t local_music_player_play(void);

/**
 * @brief Stop playing music
 *
 * @return ESP_OK on success
 */
esp_err_t local_music_player_stop(void);

/**
 * @brief Pause playing music
 *
 * @return ESP_OK on success
 */
esp_err_t local_music_player_pause(void);

/**
 * @brief Resume playing music
 *
 * @return ESP_OK on success
 */
esp_err_t local_music_player_resume(void);

/**
 * @brief Play next track
 *
 * @return ESP_OK on success
 */
esp_err_t local_music_player_next(void);

/**
 * @brief Play previous track
 *
 * @return ESP_OK on success
 */
esp_err_t local_music_player_previous(void);

/**
 * @brief Play specific track by index
 *
 * @param track_index Track index (0-based)
 * @return ESP_OK on success
 *         ESP_ERR_INVALID_ARG if index out of range
 */
esp_err_t local_music_player_play_track(int track_index);

/**
 * @brief Get current player state
 *
 * @return Current music state
 */
music_state_t local_music_player_get_state(void);

/**
 * @brief Get current track index
 *
 * @return Current track index (0-based), -1 if no track
 */
int local_music_player_get_current_track(void);

/**
 * @brief Get total number of tracks
 *
 * @return Total tracks found, 0 if none
 */
int local_music_player_get_total_tracks(void);

/**
 * @brief Get current track name
 *
 * @param name Buffer to store track name
 * @param max_len Maximum buffer length
 * @return ESP_OK on success
 */
esp_err_t local_music_player_get_track_name(char *name, size_t max_len);

/**
 * @brief Check if player is initialized
 *
 * @return true if initialized, false otherwise
 */
bool local_music_player_is_initialized(void);

/**
 * @brief Register event callback
 *
 * @param callback Callback function
 */
void local_music_player_register_callback(music_event_callback_t callback);

#ifdef __cplusplus
}
#endif

#endif // LOCAL_MUSIC_PLAYER_H
